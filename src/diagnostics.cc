#include "diagnostics.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>

namespace h4cn {
namespace {

// Module directory with trailing backslash; empty when unknown or when the
// path computation failed.
char g_module_dir[MAX_PATH] = {};
char g_log_path[4 * MAX_PATH] = {};

// Lines logged before the config chose the channels. Only the hook summary
// can reach this, so eight is generous; overflow drops the new line rather
// than emitting it under not-yet-known switches.
constexpr size_t kPendingMax = 8;
struct PendingLine {
  char stamp[24];
  char text[256];
};
PendingLine g_pending[kPendingMax];
size_t g_pending_count = 0;

bool g_channels_ready = false;
bool g_log_file = true;
bool g_debug_view = true;

// "2026-09-07 17:33:01 " - a plain local stamp. The log is one line per
// startup (plus warnings), so without a stamp a stack of identical
// "7/7 installed" lines cannot be tied to a run.
void AppendStamp(char* out, size_t cap) {
  SYSTEMTIME t = {};
  GetLocalTime(&t);
  sprintf_s(out, cap, "%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth,
            t.wDay, t.wHour, t.wMinute, t.wSecond);
}

void AppendToLogFile(const char* stamp, const char* line) {
  if (g_log_path[0] == 0) return;
  const HANDLE file = CreateFileA(g_log_path, FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;  // Locked, read-only or no access:
  DWORD written = 0;                         // diagnostics must never fail loud
  WriteFile(file, stamp, static_cast<DWORD>(std::strlen(stamp)), &written,
            nullptr);
  WriteFile(file, " ", 1, &written, nullptr);
  WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &written,
            nullptr);
  WriteFile(file, "\n", 1, &written, nullptr);
  CloseHandle(file);
}

void Emit(const char* stamp, const char* line) {
  if (g_debug_view) OutputDebugStringA(line);
  if (g_log_file) AppendToLogFile(stamp, line);
}

}  // namespace

void InitDiagnostics(void* module) {
  char path[MAX_PATH];
  const DWORD len = GetModuleFileNameA(static_cast<HMODULE>(module), path,
                                       static_cast<DWORD>(sizeof(path)));
  if (len == 0 || len >= sizeof(path)) return;
  char* slash = std::strrchr(path, '\\');
  if (slash == nullptr) return;
  slash[1] = 0;
  sprintf_s(g_module_dir, sizeof(g_module_dir), "%s", path);

  strcpy_s(g_log_path, sizeof(g_log_path), g_module_dir);
  strcat_s(g_log_path, sizeof(g_log_path), "H4CN.log");
}

const char* DiagModuleDir() { return g_module_dir; }

void DiagApplyChannels(bool log_file, bool log_debug_view) {
  if (g_channels_ready) return;
  g_log_file = log_file;
  g_debug_view = log_debug_view;
  g_channels_ready = true;
  for (size_t i = 0; i < g_pending_count; ++i) {
    Emit(g_pending[i].stamp, g_pending[i].text);
  }
  g_pending_count = 0;
}

void DiagLog(const char* message) {
  PendingLine entry;
  AppendStamp(entry.stamp, sizeof(entry.stamp));
  sprintf_s(entry.text, "H4CN: %s", message);
  if (!g_channels_ready) {
    if (g_pending_count < kPendingMax) {
      g_pending[g_pending_count++] = entry;
    }
    return;
  }
  Emit(entry.stamp, entry.text);
}

}  // namespace h4cn
