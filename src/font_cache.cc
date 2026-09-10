#include "font_cache.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <tuple>
#include <utility>

#include "config.h"
#include "diagnostics.h"

namespace h4cn {
namespace {

// GBK, decoded explicitly so rendering does not depend on the process ANSI code
// page being 936 (many players run the game under another locale).
constexpr int kGbkCodePage = 936;
constexpr int kUtf8CodePage = CP_UTF8;

// Only used when the game hands us a t_font whose size is 0 or negative; the
// face and cell then come from the configuration as for any other size.
constexpr int kFallbackFontSize = 16;

constexpr int kFirstAscii = 0x20;
constexpr int kLastAscii = 0x7E;
constexpr size_t kGlyphBucketReserve = 4096;

// Guards the context map only (creation/clearing in GetFontContext's cold
// path, Init/Shutdown). Glyph-cache locking is per-context since the memo
// took the map lookup off the hot path.
std::mutex g_mutex;

// Defensive ceiling for the configured supersample factor; the config layer
// already restricts it to 1..4, but the DIB size below multiplies by it.
constexpr int kSupersampleCeil = 4;

// Allocated by InitFontCache() instead of at static-initialisation time: a
// container whose constructor could throw must not run before DllMain, where
// there is nothing to catch it, and this way ShutdownFontCache() can also
// release it deterministically. Keyed by (face, cell height, supersample):
// H4CN.toml can point two game sizes at the same GDI cell (or one size at
// several faces), and now supersample is per size, so two sizes sharing a
// (face, cell) but wanting different Nx must not collide onto one context.
std::map<std::tuple<std::string, int, int>, std::unique_ptr<FontContext>>*
    g_contexts = nullptr;

// game size -> FontContext* memo (the hot path).
//
// Every hooked call resolves its font through GetFontContext, and 9 of the
// get_width call sites sit in per-frame paths. The full resolution builds a
// face string, a (string,int,int) tuple and searches the map under the lock -
// pure overhead once the answer is known. H4CN.toml is immutable after
// EnsureConfigLoaded, so (game size -> context) is fixed for the whole
// process and the size alone is a complete key. Failed lookups are memoised
// too (kMemoFailed), so a dead size cannot re-pay the cold path per call.
//
// Tagged pointer: an aligned FontContext* never equals 0 or 1. The release
// store pairs with the acquire load so a thread that sees the pointer also
// sees the fully-constructed FontContext. ShutdownFontCache resets every
// entry, so no stale pointer survives a re-init.
constexpr int kMemoMaxSize = 256;  // config caps sizes at 256; observed <= 28
constexpr uintptr_t kMemoFailed = 1;
std::atomic<uintptr_t> g_size_memo[kMemoMaxSize + 1];

void MemoPublish(int game_size, FontContext* ctx) {
  if (game_size < 1 || game_size > kMemoMaxSize) return;
  g_size_memo[game_size].store(
      ctx == nullptr ? kMemoFailed : reinterpret_cast<uintptr_t>(ctx),
      std::memory_order_release);
}

void MemoReset() {
  for (std::atomic<uintptr_t>& slot : g_size_memo) {
    slot.store(0, std::memory_order_relaxed);
  }
}

// Converts one decoded character back to UTF-16. Returns 0 when the sequence is
// not valid GBK, which leaves the glyph blank rather than drawing a best-fit
// replacement.
int CodeToWide(uint32_t code, wchar_t* out, int out_len) {
  char narrow[2];
  int narrow_len = 0;
  if (code > 0xFF) {
    narrow[0] = static_cast<char>((code >> 8) & 0xFF);
    narrow[1] = static_cast<char>(code & 0xFF);
    narrow_len = 2;
  } else {
    narrow[0] = static_cast<char>(code & 0xFF);
    narrow_len = 1;
  }
  return MultiByteToWideChar(kGbkCodePage, 0, narrow, narrow_len, out, out_len);
}

std::wstring Utf8ToWide(const std::string& utf8) {
  std::wstring wide;
  const int len =
      MultiByteToWideChar(kUtf8CodePage, 0, utf8.c_str(), -1, nullptr, 0);
  if (len > 0) {
    wide.resize(static_cast<size_t>(len) - 1);
    MultiByteToWideChar(kUtf8CodePage, 0, utf8.c_str(), -1, wide.data(), len);
  }
  return wide;
}

// The inverse. printf's %S must NOT be used for wide names in these messages:
// under the default "C" locale it converts nothing and sprintf_s leaves the
// buffer empty, which is how the SimSun-substitution warning surfaced as the
// empty "H4CN: " line in a real player's log.
std::string WideToUtf8(const wchar_t* wide) {
  std::string out;
  const int len = WideCharToMultiByte(kUtf8CodePage, 0, wide, -1, nullptr, 0,
                                      nullptr, nullptr);
  if (len > 1) {
    out.resize(static_cast<size_t>(len) - 1);
    WideCharToMultiByte(kUtf8CodePage, 0, wide, -1, out.data(), len, nullptr,
                        nullptr);
  }
  return out;
}

// GDI reports the *installed* face's name, and Windows localises it: on a
// zh-CN system LiSu comes back as 隶书, SimSun as 宋体 - the same face, no
// substitution at all. Without these aliases H4CN.toml faces whose ASCII name
// differs from the registered localised name would log a spurious warning (a
// real player log showed the one that was hardcoded firing for SimSun).
bool IsSameFace(const wchar_t* actual, const wchar_t* requested) {
  if (lstrcmpiW(actual, requested) == 0) return true;
  struct FaceAlias {
    const wchar_t* ascii;
    const wchar_t* localized;
  };
  static constexpr FaceAlias kAliases[] = {
      {L"LiSu", L"\u96B6\u4E66"},                         // 隶书
      {L"SimSun", L"\u5B8B\u4F53"},                       // 宋体
      {L"NSimSun", L"\u65B0\u5B8B\u4F53"},                // 新宋体
      {L"SimHei", L"\u9ED1\u4F53"},                       // 黑体
      {L"Microsoft YaHei", L"\u5FAE\u8F6F\u96C5\u9ED1"},  // 微软雅黑
      {L"KaiTi", L"\u6977\u4F53"},                        // 楷体
      {L"FangSong", L"\u4EFF\u5B8B"},                     // 仿宋
  };
  for (const FaceAlias& alias : kAliases) {
    if (lstrcmpiW(requested, alias.ascii) == 0 &&
        lstrcmpW(actual, alias.localized) == 0) {
      return true;
    }
  }
  return false;
}

// Windows silently replaces a missing face with a fallback; the metrics and
// the look then depend on whatever got substituted. Report once per process:
// every context shares the same installed-fonts list, so more lines add
// nothing. Runs with the module lock held (called from the FontContext ctor).
void ReportFaceSubstitution(HDC dc, const wchar_t* requested) {
  static bool reported = false;
  if (reported) return;
  wchar_t actual[64] = {};
  if (GetTextFaceW(dc, 64, actual) == 0) return;
  if (IsSameFace(actual, requested)) return;
  reported = true;
  const std::string requested_utf8 = WideToUtf8(requested);
  const std::string actual_utf8 = WideToUtf8(actual);
  char message[320];  // 64 wchars per name can occupy 3x the bytes in UTF-8
  sprintf_s(message, "font '%s' unavailable, substituted by '%s'",
            requested_utf8.c_str(), actual_utf8.c_str());
  DiagLog(message);
}

}  // namespace

FontContext::FontContext(std::string face_utf8, int render_size,
                         int supersample)
    : face_utf8_(std::move(face_utf8)), render_size_(render_size) {
  if (render_size <= 0) return;

  // Supersample factor resolved for this context's game size (per-size override
  // or the [render] default, both range-checked at parse time). Clamped here
  // too because the DIB size below multiplies by it. 1 keeps the exact v3.1.0
  // rasterisation; N > 1 draws the glyph at N x and box-filters it back, so a
  // thin stroke can sit between device pixels instead of snapping to the grid.
  int ss = supersample;
  if (ss < 1) ss = 1;
  if (ss > kSupersampleCeil) ss = kSupersampleCeil;
  supersample_ = ss;

  // The DIB holds the N x render, so it grows with the factor. The extra
  // (3 * ss rather than 3 * ss + margin) is left over from the 1 x sizing where
  // a full-width CJK cell is ~ render_size and one row of descent.
  dib_w_ = render_size * 3 * ss;
  dib_h_ = render_size * 3 * ss;
  dib_stride_ = dib_w_ * 4;

  HDC screen = GetDC(nullptr);
  if (screen == nullptr) return;
  dc_ = CreateCompatibleDC(screen);

  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = dib_w_;
  info.bmiHeader.biHeight = -dib_h_;  // top-down: row 0 is the cell's top row
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  dib_ = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (dc_ == nullptr || dib_ == nullptr || bits == nullptr) return;
  bits_ = static_cast<uint8_t*>(bits);

  HDC dc = static_cast<HDC>(dc_);
  SelectObject(dc, dib_);
  const std::wstring face_wide = Utf8ToWide(face_utf8_);
  // GB2312_CHARSET is proven for the shipped default and also steers the
  // matcher towards the Chinese face; an arbitrary configured face is better
  // off with the system default.
  const BYTE charset = lstrcmpiW(face_wide.c_str(), L"LiSu") == 0
                           ? static_cast<BYTE>(GB2312_CHARSET)
                           : static_cast<BYTE>(DEFAULT_CHARSET);
  font_ =
      CreateFontW(render_size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, charset,
                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                  DEFAULT_PITCH | FF_DONTCARE, face_wide.c_str());
  if (font_ == nullptr) return;
  SelectObject(dc, font_);

  TEXTMETRICW metrics = {};
  if (GetTextMetricsW(dc, &metrics) == 0) return;
  line_height_ = metrics.tmHeight;
  ReportFaceSubstitution(dc, face_wide.c_str());
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(0, 0, 0));
  valid_ = true;

  // The supersample font is selected only inside Rasterize(); if it fails we
  // simply fall back to the 1x path (font_ss_ stays null, Rasterize checks).
  if (supersample_ > 1) {
    font_ss_ = CreateFontW(render_size * supersample_, 0, 0, 0, FW_NORMAL,
                           FALSE, FALSE, FALSE, charset, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, face_wide.c_str());
    if (font_ss_ == nullptr) supersample_ = 1;
  }

  // Prime ASCII advances so that measuring Latin text never touches GDI.
  glyphs_.reserve(kGlyphBucketReserve);
  for (int code = kFirstAscii; code <= kLastAscii; ++code) {
    Glyph& glyph = glyphs_[static_cast<uint32_t>(code)];
    Measure(&glyph, static_cast<uint32_t>(code));
    glyph.measured = true;
  }
}

FontContext::~FontContext() {
  // DeleteDC first: it deselects the font and the bitmap, so both can then be
  // deleted without still being selected into a DC.
  if (dc_ != nullptr) DeleteDC(static_cast<HDC>(dc_));
  if (font_ != nullptr) DeleteObject(static_cast<HFONT>(font_));
  if (font_ss_ != nullptr) DeleteObject(static_cast<HFONT>(font_ss_));
  if (dib_ != nullptr) DeleteObject(static_cast<HBITMAP>(dib_));
}

void FontContext::Measure(Glyph* glyph, uint32_t code) {
  if (!valid_) return;

  wchar_t wide[4] = {};
  const int wide_len = CodeToWide(code, wide, 4);
  SIZE extent = {};
  if (wide_len > 0 &&
      GetTextExtentPoint32W(static_cast<HDC>(dc_), wide, wide_len, &extent) !=
          0 &&
      extent.cx > 0) {
    glyph->advance = extent.cx;
    return;
  }

  // Unmappable or unmeasurable. Keep the pen moving so that wrapping always
  // makes progress and no layout divides by a zero-width string.
  glyph->advance = (code > 0xFF) ? render_size_ : std::max(render_size_ / 2, 1);
}

void FontContext::Rasterize(Glyph* glyph, uint32_t code) {
  if (!valid_) return;

  wchar_t wide[4] = {};
  const int wide_len = CodeToWide(code, wide, 4);
  if (wide_len <= 0) return;

  HDC dc = static_cast<HDC>(dc_);

  // Draw at supersample_ x. The N x font is selected only for this call and
  // font_ is restored before returning, because Measure() and the ASCII priming
  // rely on the 1 x font staying selected on this shared DC.
  const int ss = supersample_;
  HFONT render_font = (ss > 1 && font_ss_ != nullptr)
                          ? static_cast<HFONT>(font_ss_)
                          : static_cast<HFONT>(font_);
  SelectObject(dc, render_font);

  SIZE extent = {};
  if (GetTextExtentPoint32W(dc, wide, wide_len, &extent) == 0) {
    SelectObject(dc, font_);
    return;
  }
  const int extent_w =
      extent.cx > 0 ? static_cast<int>(extent.cx) : glyph->advance * ss;
  const int extent_h =
      extent.cy > 0 ? static_cast<int>(extent.cy) : line_height_ * ss;
  const int w2 = std::clamp(extent_w, 1, dib_w_);
  const int h2 = std::clamp(extent_h, 1, dib_h_);

  // The DIB is shared by every glyph of this size: clear the supersampled cell
  // we read back, then rasterise the glyph into it.
  for (int row = 0; row < h2; ++row) {
    std::memset(bits_ + static_cast<size_t>(row) * dib_stride_, 0xFF,
                static_cast<size_t>(w2) * 4);
  }
  if (TextOutW(dc, 0, 0, wide, wide_len) == 0) {
    SelectObject(dc, font_);
    return;
  }
  GdiFlush();

  // Per-sub-pixel linear coverage (0 = background, 255 = solid ink), read
  // straight off the DIB section's bits.
  std::vector<uint8_t> cov2(static_cast<size_t>(w2) * h2);
  for (int row = 0; row < h2; ++row) {
    const uint8_t* src = bits_ + static_cast<size_t>(row) * dib_stride_;
    uint8_t* dst = &cov2[static_cast<size_t>(row) * w2];
    for (int col = 0; col < w2; ++col) {
      const uint8_t* px = src + static_cast<size_t>(col) * 4;  // BGRA
      const int gray = (px[0] * 77 + px[1] * 150 + px[2] * 29) >> 8;
      dst[col] = static_cast<uint8_t>(255 - gray);
    }
  }
  SelectObject(dc, font_);  // restore the 1 x grid for future Measure()

  // Box-filter each ss x ss block back to the 1 x cell. Right/bottom edge
  // blocks average only the sub-pixels that fit. ss == 1 is a no-op pass that
  // reproduces the v3.1.0 value bit for bit.
  const int w = (w2 + ss - 1) / ss;
  const int h = (h2 + ss - 1) / ss;
  std::vector<uint8_t>& pixels = glyph->pixels;
  pixels.assign(static_cast<size_t>(w) * h, 0);
  for (int oy = 0; oy < h; ++oy) {
    uint8_t* dst = &pixels[static_cast<size_t>(oy) * w];
    const int y_end = std::min((oy + 1) * ss, h2);
    for (int ox = 0; ox < w; ++ox) {
      const int x_end = std::min((ox + 1) * ss, w2);
      int sum = 0;
      int count = 0;
      for (int yy = oy * ss; yy < y_end; ++yy) {
        const uint8_t* srow = &cov2[static_cast<size_t>(yy) * w2];
        for (int xx = ox * ss; xx < x_end; ++xx) {
          sum += srow[xx];
          ++count;
        }
      }
      const int cov = count > 0 ? (sum + count / 2) / count : 0;  // 0..255
      const int alpha = (cov * 15 + 127) / 255;                   // 0..15
      dst[ox] = static_cast<uint8_t>(alpha << 4);
    }
  }

  // Bake the drop shadow into the low nibble: the foreground shifted one pixel
  // down-right, which is what BlitGlyph's shadow pass blends with.
  for (int row = h - 1; row >= 1; --row) {
    uint8_t* dst = &pixels[static_cast<size_t>(row) * w];
    const uint8_t* prev = &pixels[static_cast<size_t>(row - 1) * w];
    for (int col = w - 1; col >= 1; --col) {
      dst[col] = static_cast<uint8_t>(dst[col] | (prev[col - 1] >> 4));
    }
  }

  // Ink bounding box (shadow included). BlitGlyph touches only these pixels,
  // which for digits, punctuation and Latin text is a fraction of the cell.
  int x0 = w;
  int y0 = h;
  int x1 = -1;
  int y1 = -1;
  for (int row = 0; row < h; ++row) {
    const uint8_t* src = &pixels[static_cast<size_t>(row) * w];
    for (int col = 0; col < w; ++col) {
      if (src[col] == 0) continue;
      if (col < x0) x0 = col;
      if (col > x1) x1 = col;
      if (row < y0) y0 = row;
      if (row > y1) y1 = row;
    }
  }
  if (x1 < 0) {
    // Blank glyph (space): nothing to blit, but the advance still counts.
    pixels.clear();
    return;
  }

  glyph->cell_w = w;
  glyph->cell_h = h;
  glyph->ink_x = x0;
  glyph->ink_y = y0;
  glyph->ink_w = x1 - x0 + 1;
  glyph->ink_h = y1 - y0 + 1;
  if (glyph->advance <= 0) glyph->advance = w;
}

int FontContext::Advance(uint32_t code) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return AdvanceLocked(code);
}

int FontContext::AdvanceLocked(uint32_t code) {
  if (!valid_) return 0;
  Glyph& glyph = glyphs_[code];
  if (!glyph.measured) {
    Measure(&glyph, code);
    glyph.measured = true;
  }
  return glyph.advance;
}

const Glyph* FontContext::GetGlyph(uint32_t code) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return GetGlyphLocked(code);
}

const Glyph* FontContext::GetGlyphLocked(uint32_t code) {
  Glyph& glyph = glyphs_[code];
  if (!valid_) return &glyph;
  if (!glyph.measured) {
    Measure(&glyph, code);
    glyph.measured = true;
  }
  if (!glyph.rasterized) {
    ScopedPerf perf(kPerfGlyphRasterize);
    Rasterize(&glyph, code);
    glyph.rasterized = true;
  }
  return &glyph;
}

FontContext* GetFontContext(const game::Font& font) {
  const int game_size = font.size > 0 ? font.size : kFallbackFontSize;

  // Hot path: a previously resolved size returns straight from the memo, no
  // config re-read, no string/tuple, no lock. Only an acquire load + bounds
  // check. Unresolved sizes (0) and failed sizes (kMemoFailed) fall through.
  if (game_size >= 1 && game_size <= kMemoMaxSize) {
    const uintptr_t memo =
        g_size_memo[game_size].load(std::memory_order_acquire);
    if (memo > kMemoFailed) {
      return reinterpret_cast<FontContext*>(memo);
    }
    if (memo == kMemoFailed) {
      return nullptr;
    }
  }

  // Cold path, once per game size: ensure the config is read (the first call
  // happens on a game thread, never in DllMain), resolve the size through
  // H4CN.toml, and find-or-create the context.
  EnsureConfigLoaded();
  const Config& config = GetConfig();

  std::string face;
  int render_size = 0;
  int supersample = 1;
  config.Resolve(game_size, &face, &render_size, &supersample);
  if (face.empty() || render_size <= 0) {
    MemoPublish(game_size, nullptr);  // permanent config-resolution failure
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_contexts == nullptr) return nullptr;  // InitFontCache() never ran

  const std::tuple<std::string, int, int> key(face, render_size, supersample);
  auto it = g_contexts->find(key);
  if (it == g_contexts->end()) {
    // One line per created context: a player can read the observed
    // size-to-face table straight out of H4CN.log and copy the [fonts.NN]
    // keys (and per-size supersample) from it.
    char message[192];
    sprintf_s(message, "game size %d -> '%s' cell %d px ss%d", game_size,
              face.c_str(), render_size, supersample);
    DiagLog(message);
    std::unique_ptr<FontContext> created(
        new FontContext(face, render_size, supersample));
    if (!created->valid()) {
      // Creation is attempted exactly once per (face, size, supersample)
      // triple (the failed context stays cached), so this is already a
      // once-per-key report.
      char failed[192];
      sprintf_s(failed, "GDI context for '%s' cell %d failed to create",
                face.c_str(), render_size);
      DiagLog(failed);
    }
    it = g_contexts->emplace(key, std::move(created)).first;
  }
  // A context that failed to create stays in the map: returning it once and
  // then null forever beats retrying CreateFont on every draw call.
  FontContext* ctx = it->second.get();
  FontContext* result = ctx->valid() ? ctx : nullptr;
  MemoPublish(game_size, result);
  return result;
}

bool KeepOriginalAscii(const game::Font& font) {
  // Same lazy config load and the same game-size resolution as
  // GetFontContext, so the routing decision can never disagree with the
  // substitute face/cell already chosen for this size.
  EnsureConfigLoaded();
  const int game_size = font.size > 0 ? font.size : kFallbackFontSize;
  return GetConfig().AsciiOriginal(game_size);
}

void PatchFontMetrics(game::Font* font, int line_height) {
  if (font == nullptr || line_height <= 0) return;
  if (font->line_height != line_height) font->line_height = line_height;
}

void InitFontCache() {
  std::lock_guard<std::mutex> lock(g_mutex);
  MemoReset();
  if (g_contexts == nullptr) {
    g_contexts = new (std::nothrow) std::map<std::tuple<std::string, int, int>,
                                             std::unique_ptr<FontContext>>();
  }
}

void ShutdownFontCache() {
  std::lock_guard<std::mutex> lock(g_mutex);
  // The memoised FontContext* die with the map; clear first so a re-Init can
  // never hand out a dangling pointer.
  MemoReset();
  if (g_contexts == nullptr) return;
  // Releases every FontContext, its glyph pixels and its GDI objects.
  g_contexts->clear();
  delete g_contexts;
  g_contexts = nullptr;
}

}  // namespace h4cn
