#include "hooks.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "blit.h"
#include "config.h"
#include "diagnostics.h"
#include "font_cache.h"
#include "game_addrs.h"
#include "game_types.h"
#include "inline_hook.h"
#include "version.h"
#include "wrap.h"

namespace h4cn {
namespace {

// The hooked functions are __thiscall members: `this` in ECX, the rest on the
// stack, callee cleans up. MSVC refuses __thiscall on a free function, so the
// hooks are declared __fastcall with a dummy second parameter. That keeps
// `this` in ECX, absorbs whatever the caller happened to leave in EDX, and
// cleans the same number of stack bytes - which is what makes the two calling
// conventions interchangeable at these entry points.

// VC6 std::vector<std::string> / std::string helpers instantiated in the exe.
using FnStrVecClear = void(__thiscall*)(game::StringVector*, game::Vc6String*,
                                        game::Vc6String*);
using FnStrVecInsert = game::Vc6String*(__thiscall*)(game::StringVector*,
                                                     game::Vc6String*,
                                                     const game::Vc6String*);
using FnStrAssign = game::Vc6String*(__thiscall*)(game::Vc6String*, const char*,
                                                  size_t);
using FnStrTidy = char(__thiscall*)(game::Vc6String*, char);

// Resolves this font's configured context (which also loads H4CN.toml on its
// first call) and patches t_font::line_height to the measured cell height,
// unless [general].patch_line_height says otherwise. Returns null when no GDI
// context could be created; every hook bails out on null: an undrawn string is
// recoverable, a null dereference in the render loop is not.
FontContext* PrepareContext(game::Font* font) {
  if (font == nullptr) return nullptr;
  FontContext* ctx = GetFontContext(*font);
  if (ctx != nullptr && GetConfig().patch_line_height) {
    PatchFontMetrics(font, ctx->line_height());
  }
  return ctx;
}

// Fills the game's t_string_vector with the wrapped lines using the exe's own
// VC6 helpers, so the refcounted-string ABI is never manipulated by hand.
void WriteLines(game::StringVector* lines, const std::vector<Line>& wrapped) {
  auto clear = reinterpret_cast<FnStrVecClear>(game::kAddrStrVecClear);
  auto insert = reinterpret_cast<FnStrVecInsert>(game::kAddrStrVecInsert);
  auto assign = reinterpret_cast<FnStrAssign>(game::kAddrStrAssign);
  auto tidy = reinterpret_cast<FnStrTidy>(game::kAddrStrTidy);

  clear(lines, lines->begin, lines->end);  // same first step as the stock wrap

  game::Vc6String line = {};
  tidy(&line, 0);  // VC6 default-constructor idiom: _Tidy(false) zeroes fields
  for (const Line& wrapped_line : wrapped) {
    assign(&line, reinterpret_cast<const char*>(wrapped_line.text),
           wrapped_line.len);
    insert(lines, lines->end, &line);
  }
  tidy(&line, 1);  // release our copy; the vector holds its own references
}

// ---------------------------------------------------------------------------
// t_font::draw_to(bitmap&, screen_point, const char*, u16, bool, u16) @0x71BD50
// Single-line draw. Caller: t_text_window_paint @0x886C80, which the stock code
// reaches for every non-wrapping label in the UI.
// ---------------------------------------------------------------------------
void __fastcall HookFontDrawToPt(game::Font* font, uint32_t /*edx*/,
                                 game::Bitmap* dst, int x, int y,
                                 const uint8_t* str, uint16_t fg_color,
                                 uint8_t draw_shadow, uint16_t shadow_color) {
  ScopedPerf perf(kPerfDrawToPt);
  if (str == nullptr || *str == 0 || dst == nullptr) return;
  FontContext* ctx = PrepareContext(font);
  if (ctx == nullptr) return;
  // One lock for the whole pass; the locked-mode router skips the per-char
  // re-acquire (see FontContext::Lock / GlyphRouter, blit.h + font_cache.h).
  std::lock_guard<FontContext> batch(*ctx);
  GlyphRouter router(ctx, font, KeepOriginalAscii(*font), /*locked=*/true);

  const Line line{
      str, static_cast<int>(std::strlen(reinterpret_cast<const char*>(str)))};
  // No right-edge clipping here, exactly like the stock draw_to.
  DrawLine(line, dst, x, y, router, fg_color, draw_shadow != 0, shadow_color,
           /*clip_x1=*/0);
}

// ---------------------------------------------------------------------------
// t_font::get_width(const char*) @0x71BDE0
// 9 call sites: abbreviate_number_font, t_combat_label_draw_to,
// t_text_edit_window_position_caret, t_text_window_ctor_point,
// t_text_window_get_row_start, t_text_window_paint, ...
//
// Returns the pen distance of the whole string. The stock version trims the
// first glyph's left bearing and the last glyph's right bearing because its
// glyph bitmaps are tight boxes; ours are full cells placed at the pen, so the
// sum of advances is the width this renderer actually occupies. Measuring never
// rasterises, which is what keeps the wrapping passes cheap.
// ---------------------------------------------------------------------------
int __fastcall HookFontGetWidth(game::Font* font, uint32_t /*edx*/,
                                const uint8_t* str) {
  ScopedPerf perf(kPerfGetWidth);
  if (str == nullptr || *str == 0) return 0;
  FontContext* ctx = PrepareContext(font);
  if (ctx == nullptr) return 0;
  // One lock for the whole pass; the locked-mode router skips the per-char
  // re-acquire (see FontContext::Lock / GlyphRouter, blit.h + font_cache.h).
  std::lock_guard<FontContext> batch(*ctx);
  GlyphRouter router(ctx, font, KeepOriginalAscii(*font), /*locked=*/true);

  int total = 0;
  for (const uint8_t* p = str; *p != 0;) {
    int len = 0;
    total += router.Advance(DecodeChar(p, &len));
    p += len;
  }
  return total;
}

// ---------------------------------------------------------------------------
// t_font::get_column(const char*, int x) @0x71BE40
// Caller: t_text_edit_window_left_button_up @0x885500 (mouse click -> caret).
// Returns a byte offset, so the caret lands on a character boundary.
// ---------------------------------------------------------------------------
int __fastcall HookFontGetColumn(game::Font* font, uint32_t /*edx*/,
                                 const uint8_t* str, int max_width) {
  ScopedPerf perf(kPerfGetColumn);
  if (str == nullptr || *str == 0 || max_width <= 0) return 0;
  FontContext* ctx = PrepareContext(font);
  if (ctx == nullptr) return 0;
  // One lock for the whole pass; the locked-mode router skips the per-char
  // re-acquire (see FontContext::Lock / GlyphRouter, blit.h + font_cache.h).
  std::lock_guard<FontContext> batch(*ctx);
  GlyphRouter router(ctx, font, KeepOriginalAscii(*font), /*locked=*/true);

  int used = 0;
  int bytes = 0;
  for (const uint8_t* p = str; *p != 0;) {
    int len = 0;
    const int advance = router.Advance(DecodeChar(p, &len));
    if (used + advance > max_width) {
      // Keep the character when it only barely overflows, mirroring the
      // half-glyph tolerance of the stock get_column.
      return (max_width - used >= advance / 2) ? bytes + len : bytes;
    }
    used += advance;
    bytes += len;
    p += len;
  }
  return bytes;
}

// ---------------------------------------------------------------------------
// t_font::wrap_text(t_string_vector&, const char*, int min, int max) @0x71C5E0
// Callers: sub_5584B0, sub_559760, sub_8CAE90 (popup sizing).
// Fully replaced. The stock search sizes the starting width with the game's
// glyph table, which clamps every GBK byte to the space glyph, so the chosen
// width - and every popup that centres on it - was wrong for Chinese. The
// search itself (max(min, min(longest word, max)), grow by 3/2) is reproduced
// in wrap.cc with Advancer measurement; return value and line content match
// what the stock code hands its callers.
// ---------------------------------------------------------------------------
int __fastcall HookFontWrapTextMm(game::Font* font, uint32_t /*edx*/,
                                  game::StringVector* lines, const uint8_t* str,
                                  int min_width, int max_width) {
  ScopedPerf perf(kPerfWrapTextMm);
  if (lines == nullptr) return 0;
  FontContext* ctx = PrepareContext(font);
  if (ctx == nullptr || str == nullptr) return 0;
  // One lock for the whole pass; the locked-mode router skips the per-char
  // re-acquire (see FontContext::Lock / GlyphRouter, blit.h + font_cache.h).
  std::lock_guard<FontContext> batch(*ctx);
  GlyphRouter router(ctx, font, KeepOriginalAscii(*font), /*locked=*/true);

  // Reused line buffer: draw_to_rect is the per-frame combat-label path and
  // WrapText push_backs one Line per visible row, so a fresh vector here paid
  // a malloc/free on every frame. thread_local + clear() keeps one warm
  // capacity per hook per thread; the hook bodies never re-enter each other
  // (they call GDI, the intact game blitter and the VC6 container helpers
  // only), so no second user of the same buffer can exist on a thread.
  static thread_local std::vector<Line> wrapped;
  wrapped.clear();
  const OptimalWrap optimal = WrapTextOptimal(
      str, min_width, max_width, ctx->line_height(), router, &wrapped);
  WriteLines(lines, wrapped);
  return optimal.result.max_width;
}

// ---------------------------------------------------------------------------
// t_font::wrap_text(t_string_vector&, const char*, int width) @0x71C100
// The central line breaker: 13 call sites, including t_text_window_set_text,
// t_text_window_ctor_rect, t_text_window_update_layout and both wrapped-draw
// functions below. Returns the widest line, as the stock one does.
// ---------------------------------------------------------------------------
int __fastcall HookFontWrapText(game::Font* font, uint32_t /*edx*/,
                                game::StringVector* lines, const uint8_t* str,
                                int width) {
  ScopedPerf perf(kPerfWrapText);
  if (lines == nullptr) return 0;
  FontContext* ctx = PrepareContext(font);
  if (ctx == nullptr || str == nullptr) return 0;
  // One lock for the whole pass; the locked-mode router skips the per-char
  // re-acquire (see FontContext::Lock / GlyphRouter, blit.h + font_cache.h).
  std::lock_guard<FontContext> batch(*ctx);
  GlyphRouter router(ctx, font, KeepOriginalAscii(*font), /*locked=*/true);

  static thread_local std::vector<Line> wrapped;
  wrapped.clear();
  const WrapResult result = WrapText(str, width, router, &wrapped);
  WriteLines(lines, wrapped);
  return result.max_width;
}

// ---------------------------------------------------------------------------
// t_font::draw_to(bitmap&, const screen_rect&, const char*, u16, bool, u16)
// @0x71C7F0. Caller: t_combat_label_draw_to @0x601260.
//
// Replaces the stock wrap + t_font_draw_to_vector @0x71C6E0 pair;
// draw_to_vector blits glyph bitmaps directly and would bypass
// HookFontDrawToPt.
// ---------------------------------------------------------------------------
int __fastcall HookFontDrawToRect(game::Font* font, uint32_t /*edx*/,
                                  game::Bitmap* dst, game::Rect* rect,
                                  const uint8_t* str, uint16_t fg_color,
                                  uint8_t draw_shadow, uint16_t shadow_color) {
  ScopedPerf perf(kPerfDrawToRect);
  if (str == nullptr || *str == 0 || dst == nullptr || rect == nullptr) {
    return 0;
  }
  const int width = rect->x1 - rect->x;
  const int height = rect->y1 - rect->y;
  if (width <= 0 || height <= 0) return 0;

  FontContext* ctx = PrepareContext(font);
  if (ctx == nullptr) return 0;
  // One lock for the whole pass; the locked-mode router skips the per-char
  // re-acquire (see FontContext::lock / GlyphRouter, blit.h + font_cache.h).
  std::lock_guard<FontContext> batch(*ctx);
  GlyphRouter router(ctx, font, KeepOriginalAscii(*font), /*locked=*/true);

  static thread_local std::vector<Line> wrapped;
  wrapped.clear();
  WrapText(str, width, router, &wrapped);

  const int line_height = ctx->line_height();
  int y = rect->y;
  for (const Line& line : wrapped) {
    if (y + line_height > rect->y1) break;  // same cut-off as draw_to_vector
    DrawLine(line, dst, rect->x, y, router, fg_color, draw_shadow != 0,
             shadow_color, rect->x1);
    y += line_height;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// t_font::get_wrapped_height(const char*, int width) @0x71C900
// Callers: sub_838630 (popup layout) and t_text_window_update_layout (4 sites).
//
// The stock version wraps into a throwaway vector and returns
// line_count * t_font::line_height. The line count has to come from the same
// WrapText the drawing hooks use: the scrollbar range is derived from this
// height and a too-small value leaves the game dividing by zero.
// ---------------------------------------------------------------------------
int __fastcall HookFontGetWrappedHeight(game::Font* font, uint32_t /*edx*/,
                                        const uint8_t* str, int width) {
  ScopedPerf perf(kPerfGetWrappedHeight);
  if (str == nullptr || *str == 0 || width <= 0) return 0;
  FontContext* ctx = PrepareContext(font);
  if (ctx == nullptr) return 0;
  // One lock for the whole pass; the locked-mode router skips the per-char
  // re-acquire (see FontContext::Lock / GlyphRouter, blit.h + font_cache.h).
  std::lock_guard<FontContext> batch(*ctx);
  GlyphRouter router(ctx, font, KeepOriginalAscii(*font), /*locked=*/true);

  const WrapResult result = WrapText(str, width, router, /*lines=*/nullptr);
  return result.line_count * ctx->line_height();
}

struct HookEntry {
  const game::HookTarget* target;
  void* hook;
};

}  // namespace

void InstallHooks() {
  // All seven fully replace their originals, so none takes a trampoline.
  const HookEntry entries[] = {
      {&game::kFontDrawToPt, reinterpret_cast<void*>(&HookFontDrawToPt)},
      {&game::kFontGetWidth, reinterpret_cast<void*>(&HookFontGetWidth)},
      {&game::kFontGetColumn, reinterpret_cast<void*>(&HookFontGetColumn)},
      {&game::kFontWrapTextMm, reinterpret_cast<void*>(&HookFontWrapTextMm)},
      {&game::kFontWrapText, reinterpret_cast<void*>(&HookFontWrapText)},
      {&game::kFontDrawToRect, reinterpret_cast<void*>(&HookFontDrawToRect)},
      {&game::kFontGetWrappedHeight,
       reinterpret_cast<void*>(&HookFontGetWrappedHeight)},
  };

  int installed = 0;
  char failures[256] = {};
  size_t failures_len = 0;
  for (const HookEntry& entry : entries) {
    const HookInstallResult result =
        InstallInlineHook(*entry.target, entry.hook, nullptr);
    if (result == HookInstallResult::kOk) {
      ++installed;
      continue;
    }
    if (failures_len < sizeof(failures) - 1) {
      const int appended = sprintf_s(
          failures + failures_len, sizeof(failures) - failures_len,
          " 0x%08X(%s)", entry.target->addr, HookInstallResultName(result));
      if (appended > 0) failures_len += static_cast<size_t>(appended);
    }
  }

  // One line per startup, always to the log: its presence and hook count are
  // the proof of life for a player, and a refused patch (wrong-exe signature:
  // the game keeps running, only the Chinese is missing) is localisable from
  // the file without a debugger.
  const int total = static_cast<int>(sizeof(entries) / sizeof(entries[0]));
  char message[512];
  if (installed == total) {
    sprintf_s(message, "v%s: %d/%d font hooks installed", H4CN_VERSION_STRING,
              installed, total);
  } else {
    sprintf_s(message, "v%s: %d/%d font hooks installed; failed:%s",
              H4CN_VERSION_STRING, installed, total, failures);
  }
  DiagLog(message);
}

}  // namespace h4cn
