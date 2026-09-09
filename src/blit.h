// Blending of rasterised glyphs into the game's RGB565 framebuffer, and the
// per-character routing between the original .fon bitmaps and the GDI
// substitute.
#ifndef H4CN_BLIT_H_
#define H4CN_BLIT_H_

#include <cstdint>

#include "font_cache.h"
#include "game_types.h"
#include "wrap.h"

namespace h4cn {

// Blends a GDI-rendered `glyph` into `dst` with its cell's top left at (x, y).
//
// The arithmetic reproduces t_font_bitmap::draw_to @0x71B820 exactly (verified
// instruction by instruction): 4-bit alpha, high nibble foreground and low
// nibble drop shadow, each blended through the RGB565 masks the game keeps at
// 0xAAF0D8..0xAAF104. Only the glyph's ink bounding box is visited, and pixels
// with no coverage at all are left untouched instead of being written back
// unchanged.
//
// Clipping is to the framebuffer; the caller decides whether a glyph that would
// cross a widget's right edge is drawn at all (the stock renderer drops it).
void BlitGlyph(const Glyph& glyph, game::Bitmap* dst, int x, int y,
               uint16_t fg_color, bool draw_shadow, uint16_t shadow_color);

// Decides, per character, whether to draw the authentic original bitmap or the
// GDI substitute. Printable ASCII (0x20-0x7E) that the game's .fon actually
// contains is drawn from the original glyph table (crisp, and the substitute
// faces are Chinese fonts whose Latin glyphs are the weak point); GBK and any
// code the font lacks fall through to `ctx`.
//
// It is also the Advancer the width / column / wrap / height hooks measure
// with, so a line breaks against exactly the advances DrawLine lays out. The
// two must never disagree - a mismatch is an integer divide-by-zero in the
// stock scrollbar code, not just a visual bug (docs/AGENTS.md).
class GlyphRouter : public Advancer {
 public:
  // `ctx` (the GDI substitute) and `font` (the live t_font, whose original
  // glyph table is read) must outlive the router; both are non-null in every
  // hook.
  GlyphRouter(FontContext* ctx, const game::Font* font, bool ascii_original)
      : ctx_(ctx), font_(font), ascii_original_(ascii_original) {}

  FontContext* context() const { return ctx_; }

  // Advancer: pen advance of `code`, rasterising nothing. Original table for a
  // kept glyph, GDI advance otherwise. Kept glyphs use the same no-trim model
  // as GDI ones (margin_left + width + margin_right summed per character).
  int Advance(uint32_t code) override;

  // The original bitmap cell to draw `code` with, or null when it should go
  // through GDI. Non-null only when ASCII-original is on, `code` is a decoded
  // single byte in 0x20-0x7E, and it lands inside the font's contiguous
  // [first_char, first_char + glyph_count) glyph range.
  const game::FontBitmap* OriginalGlyph(uint32_t code) const;

 private:
  FontContext* ctx_;
  const game::Font* font_;
  bool ascii_original_;
};

// Draws one wrapped line with its first character's pen at (x, y), routing each
// character through `router`: kept ASCII is blitted via the game's own
// t_font_bitmap::draw_to, everything else via the GDI substitute.
//
// `clip_x1` is an exclusive right edge in framebuffer coordinates: a glyph that
// would cross it is not drawn at all, matching t_font::draw_to @0x71C6E0. Pass
// 0 to clip only to the framebuffer.
void DrawLine(const Line& line, game::Bitmap* dst, int x, int y,
              GlyphRouter& router, uint16_t fg_color, bool draw_shadow,
              uint16_t shadow_color, int clip_x1);

}  // namespace h4cn

#endif  // H4CN_BLIT_H_
