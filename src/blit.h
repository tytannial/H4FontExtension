// Blending of rasterised glyphs into the game's RGB565 framebuffer.
#ifndef H4CN_BLIT_H_
#define H4CN_BLIT_H_

#include <cstdint>

#include "font_cache.h"
#include "game_types.h"
#include "wrap.h"

namespace h4cn {

// Blends `glyph` into `dst` with its cell's top left at (x, y).
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

// Draws one wrapped line with its first character's pen at (x, y).
//
// `clip_x1` is an exclusive right edge in framebuffer coordinates: a glyph that
// would cross it is not drawn at all, matching t_font::draw_to @0x71C6E0.
// Pass 0 to clip only to the framebuffer.
void DrawLine(const Line& line, game::Bitmap* dst, int x, int y,
              FontContext* ctx, uint16_t fg_color, bool draw_shadow,
              uint16_t shadow_color, int clip_x1);

}  // namespace h4cn

#endif  // H4CN_BLIT_H_
