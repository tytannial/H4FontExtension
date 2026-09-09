#include "config.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string_view>

// Single translation unit pays for the parser; the whole library arrives in
// MSVC's 0-warning mode so a /W4 build never sees third-party diagnostics.
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "toml.hpp"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "diagnostics.h"

namespace h4cn {
namespace {

constexpr int kMaxFacePx = 256;  // DIB sides are 3x this; more is a typo.

// Supersample factor window. 1 is the raw GDI path; beyond ~4x the enlarged
// DIB and the per-glyph readback stop paying for themselves.
constexpr int kMinSupersample = 1;
constexpr int kMaxSupersample = 4;

void Add(std::vector<std::string>* problems, const std::string& line) {
  if (problems != nullptr) problems->push_back(line);
}

// Strict integer key: digits only, no leading sign, 1..256. "16" is fine,
// "16x" or "0" are not.
bool ToSizeKey(std::string_view text, int* out) {
  if (text.empty() || text.size() > 3) return false;
  int value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + (c - '0');
  }
  if (value < 1 || value > kMaxFacePx) return false;
  *out = value;
  return true;
}

// Both getters return whether a usable value was applied (false: absent,
// wrong type or out of range); callers that must tell "absent" from
// "rejected" check the key themselves.
bool GetBool(const toml::table& tbl, std::string_view key, bool* target,
             const std::string& path, std::vector<std::string>* problems) {
  const toml::node* node = tbl.get(key);
  if (node == nullptr) return false;
  if (const toml::value<bool>* v = node->as_boolean()) {
    *target = v->get();
    return true;
  }
  Add(problems, path + ": '" + std::string(key) +
                    "' must be true/false, "
                    "ignored");
  return false;
}

// Returns whether a usable value was applied (false: absent, wrong type or
// out of range - so a rejected value can never masquerade as a zero).
bool GetInt(const toml::table& tbl, std::string_view key, int* target,
            const std::string& path, std::vector<std::string>* problems) {
  const toml::node* node = tbl.get(key);
  if (node == nullptr) return false;
  const toml::value<int64_t>* v = node->as_integer();
  if (v == nullptr) {
    Add(problems, path + ": '" + std::string(key) +
                      "' must be an integer, "
                      "ignored");
    return false;
  }
  const int64_t value = v->get();
  if (value < -1024 || value > 4096) {
    Add(problems, path + ": '" + std::string(key) + "' out of range, ignored");
    return false;
  }
  *target = static_cast<int>(value);
  return true;
}

void GetString(const toml::table& tbl, std::string_view key,
               std::string* target, const std::string& path,
               std::vector<std::string>* problems) {
  const toml::node* node = tbl.get(key);
  if (node == nullptr) return;
  if (const toml::value<std::string>* v = node->as_string()) {
    *target = v->get();
  } else {
    Add(problems,
        path + ": '" + std::string(key) + "' must be a string, ignored");
  }
}

void WarnUnknown(const toml::table& tbl, const std::string& path,
                 std::initializer_list<std::string_view> known,
                 std::vector<std::string>* problems) {
  for (const auto& [key, value] : tbl) {
    (void)value;
    const std::string_view name = key;
    bool found = false;
    for (std::string_view k : known) {
      if (k == name) {
        found = true;
        break;
      }
    }
    if (!found) {
      Add(problems, path + ": unknown key '" + std::string(name) + "'");
    }
  }
}

}  // namespace

void Config::Resolve(int game_size, std::string* face, int* render_size,
                     int* supersample) const {
  const auto it = per_size.find(game_size);
  if (it != per_size.end() && !it->second.face.empty()) {
    *face = it->second.face;
  } else {
    *face = fallback_face;
  }

  int size = 0;
  if (it != per_size.end()) {
    if (it->second.has_size) {
      size = it->second.size;
    } else if (it->second.has_bias) {
      size = game_size + it->second.bias;
    }
  }
  if (size == 0) size = game_size + default_bias;
  *render_size = size < 1 ? 1 : size;

  // Per-size supersample wins; otherwise the [render] default. The value was
  // range-checked at parse time, so it is only ever 1..kMaxSupersample here.
  *supersample = (it != per_size.end() && it->second.has_supersample)
                     ? it->second.supersample
                     : render_supersample;
}

bool Config::AsciiOriginal(int game_size) const {
  const auto it = per_size.find(game_size);
  if (it != per_size.end() && it->second.has_ascii_original) {
    return it->second.ascii_original;
  }
  return render_ascii_original;
}

Config ParseConfig(const std::string& toml_text,
                   std::vector<std::string>* problems) {
  Config cfg;
  std::optional<toml::table> doc;
  try {
    doc = toml::parse(toml_text);
  } catch (const toml::parse_error& error) {
    Add(problems, "config: syntax error at line " +
                      std::to_string(error.source().begin.line) + ": " +
                      std::string(error.description()));
    return cfg;
  }
  toml::table& root = *doc;

  if (const toml::table* gen = root.get_as<toml::table>("general")) {
    GetBool(*gen, "log_file", &cfg.log_file, "[general]", problems);
    GetBool(*gen, "log_debug_view", &cfg.log_debug_view, "[general]", problems);
    GetBool(*gen, "patch_line_height", &cfg.patch_line_height, "[general]",
            problems);
    WarnUnknown(*gen, "[general]",
                {"log_file", "log_debug_view", "patch_line_height"}, problems);
  } else if (root.get("general") != nullptr) {
    Add(problems, "config: [general] must be a table");
  }

  if (const toml::table* render = root.get_as<toml::table>("render")) {
    int supersample = cfg.render_supersample;
    if (GetInt(*render, "supersample", &supersample, "[render]", problems)) {
      if (supersample >= kMinSupersample && supersample <= kMaxSupersample) {
        cfg.render_supersample = supersample;
      } else {
        Add(problems, "config: [render] supersample out of range " +
                          std::to_string(kMinSupersample) + ".." +
                          std::to_string(kMaxSupersample) + ", ignored");
      }
    }
    GetBool(*render, "ascii_original", &cfg.render_ascii_original, "[render]",
            problems);
    WarnUnknown(*render, "[render]", {"supersample", "ascii_original"},
                problems);
  } else if (root.get("render") != nullptr) {
    Add(problems, "config: [render] must be a table");
  }

  if (const toml::table* fonts = root.get_as<toml::table>("fonts")) {
    GetString(*fonts, "fallback_face", &cfg.fallback_face, "[fonts]", problems);
    if (fonts->get("fallback_face") != nullptr && cfg.fallback_face.empty()) {
      Add(problems, "config: [fonts] fallback_face is empty, keeping default");
      cfg.fallback_face = "LiSu";
    }
    GetInt(*fonts, "default_bias", &cfg.default_bias, "[fonts]", problems);
    for (const auto& [key, value] : *fonts) {
      const std::string_view name = key;
      if (name == "fallback_face" || name == "default_bias") continue;
      if (!value.is_table()) {
        Add(problems,
            "config: [fonts]: unknown key '" + std::string(name) + "'");
        continue;
      }
      int size = 0;
      if (!ToSizeKey(name, &size)) {
        Add(problems, "config: [fonts] key '" + std::string(name) +
                          "' is not a game font size (expected e.g. "
                          "[fonts.16])");
        continue;
      }
      const std::string path = "[fonts." + std::to_string(size) + "]";
      const toml::table* m = value.as_table();
      FontMapping mapping;
      const bool has_size_key = m->get("size") != nullptr;
      const bool has_bias_key = m->get("bias") != nullptr;
      GetString(*m, "face", &mapping.face, path, problems);
      const bool size_ok = GetInt(*m, "size", &mapping.size, path, problems);
      const bool bias_ok = GetInt(*m, "bias", &mapping.bias, path, problems);
      if (has_size_key) {
        mapping.has_size =
            size_ok && mapping.size >= 1 && mapping.size <= kMaxFacePx;
        if (!mapping.has_size && size_ok) {
          Add(problems, "config: " + path + " size out of range 1.." +
                            std::to_string(kMaxFacePx) + ", ignored");
        }
      }
      if (has_bias_key && bias_ok) {
        if (size + mapping.bias >= 1) {
          mapping.has_bias = true;
        } else {
          Add(problems, "config: " + path +
                            " bias would make the cell smaller than 1 px, "
                            "ignored");
        }
      }
      const bool has_ss_key = m->get("supersample") != nullptr;
      const bool ss_ok =
          GetInt(*m, "supersample", &mapping.supersample, path, problems);
      if (has_ss_key) {
        mapping.has_supersample = ss_ok &&
                                  mapping.supersample >= kMinSupersample &&
                                  mapping.supersample <= kMaxSupersample;
        if (!mapping.has_supersample && ss_ok) {
          Add(problems, "config: " + path + " supersample out of range " +
                            std::to_string(kMinSupersample) + ".." +
                            std::to_string(kMaxSupersample) + ", ignored");
        }
      }
      mapping.has_ascii_original = GetBool(
          *m, "ascii_original", &mapping.ascii_original, path, problems);
      WarnUnknown(*m, path,
                  {"face", "size", "bias", "supersample", "ascii_original"},
                  problems);
      cfg.per_size[size] = mapping;
    }
  } else if (root.get("fonts") != nullptr) {
    Add(problems, "config: [fonts] must be a table");
  }

  WarnUnknown(root, "config file", {"general", "render", "fonts"}, problems);
  return cfg;
}

namespace {

std::once_flag g_load_once;
// Default construction of string/map members allocates nothing, hence cannot
// throw; the check does not know that.
Config g_config;  // NOLINT(bugprone-throwing-static-initialization)

enum class FileRead : std::uint8_t { kOk, kMissing, kUnreadable, kOversized };

FileRead ReadWholeFile(const std::string& path, std::string* text) {
  const HANDLE file =
      CreateFileA(path.c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return GetLastError() == ERROR_FILE_NOT_FOUND ||
                   GetLastError() == ERROR_PATH_NOT_FOUND
               ? FileRead::kMissing
               : FileRead::kUnreadable;
  }
  const DWORD size = GetFileSize(file, nullptr);
  // A config nobody can read by eye is a bug in the player's editor, not a
  // reason to load megabytes.
  if (size == INVALID_FILE_SIZE) {
    CloseHandle(file);
    return FileRead::kUnreadable;
  }
  if (size > 64 * 1024) {
    CloseHandle(file);
    return FileRead::kOversized;
  }
  text->assign(size, '\0');
  DWORD read = 0;
  const bool ok =
      size == 0 ||
      (ReadFile(file, text->data(), size, &read, nullptr) != 0 && read == size);
  CloseHandle(file);
  return ok ? FileRead::kOk : FileRead::kUnreadable;
}

}  // namespace

void EnsureConfigLoaded() {
  std::call_once(g_load_once, [] {
    std::vector<std::string> problems;
    Config cfg;
    const std::string path = std::string(DiagModuleDir()) + "H4CN.toml";
    std::string text;
    const FileRead read = ReadWholeFile(path, &text);
    if (read == FileRead::kMissing) {
      DiagLog("config: no H4CN.toml, defaults in use");
    } else if (read == FileRead::kOversized) {
      DiagLog("config: H4CN.toml is >64KB, defaults in use");
    } else if (read != FileRead::kOk) {
      DiagLog("config: H4CN.toml present but unreadable, defaults in use");
    } else {
      cfg = ParseConfig(text, &problems);
      DiagLog("config: H4CN.toml loaded");
    }
    // Applying the channels also flushes everything buffered so far, including
    // the hook-install summary, under the switches this file just chose.
    DiagApplyChannels(cfg.log_file, cfg.log_debug_view);
    for (const std::string& problem : problems) {
      DiagLog(problem.c_str());
    }
    g_config = std::move(cfg);
  });
}

const Config& GetConfig() { return g_config; }

}  // namespace h4cn
