#include "blit.h"

#include <algorithm>

#include "game_addrs.h"

namespace h4cn {
namespace {

// The game sets these once while initialising its RGB565 surface. Read them on
// every blit so a resolution or pixel-format change is followed, but fall back
// to the calibrated values if we are ever called before that happens.
uint32_t ReadBlendMask(uint32_t addr, uint32_t fallback) {
  const uint32_t value = *reinterpret_cast<const uint32_t*>(addr);
  return value != 0 ? value : fallback;
}

}  // namespace

void BlitGlyph(const Glyph& glyph, game::Bitmap* dst, int x, int y,
               uint16_t fg_color, bool draw_shadow, uint16_t shadow_color) {
  if (glyph.pixels.empty() || dst == nullptr || dst->buffer == nullptr) return;
  if (glyph.ink_w <= 0 || glyph.ink_h <= 0) return;

  const int pitch_px = dst->pitch / 2;  // bytes per row -> RGB565 pixels
  if (pitch_px < dst->width || dst->width <= 0 || dst->height <= 0) return;

  // Cell coordinates of the ink box, then intersect with the framebuffer.
  const int box_x = x + glyph.ink_x;
  const int box_y = y + glyph.ink_y;
  const int col0 = std::max(box_x, 0);
  const int row0 = std::max(box_y, 0);
  const int col1 = std::min(box_x + glyph.ink_w, dst->width);
  const int row1 = std::min(box_y + glyph.ink_h, dst->height);
  if (col0 >= col1 || row0 >= row1) return;

  const uint32_t m0 =
      ReadBlendMask(game::kAddrBlendMaskM0, game::kFallbackBlendMaskM0);
  const uint32_t m1 =
      ReadBlendMask(game::kAddrBlendMaskM1, game::kFallbackBlendMaskM1);
  const uint32_t m2 =
      ReadBlendMask(game::kAddrBlendMaskM2, game::kFallbackBlendMaskM2);
  const uint32_t m3 =
      ReadBlendMask(game::kAddrBlendMaskM3, game::kFallbackBlendMaskM3);

  // Hoisted out of the pixel loop: the stock blitter recomputes them per pixel
  // only because its colour arguments live in registers that get reused.
  const uint32_t m1_fg = m1 & fg_color;
  const uint32_t m3_fg = m3 & fg_color;
  const uint32_t m1_shadow = m1 & shadow_color;
  const uint32_t m3_shadow = m3 & shadow_color;

  const int cell_col = col0 - x;
  const int cell_row = row0 - y;
  if (cell_col < 0 || cell_row < 0 || cell_row >= glyph.cell_h) return;

  const uint8_t* src = glyph.pixels.data() +
                       static_cast<size_t>(cell_row) * glyph.cell_w + cell_col;
  uint16_t* out = dst->buffer + static_cast<size_t>(row0) * pitch_px + col0;

  for (int row = row0; row < row1; ++row) {
    const uint8_t* in = src;
    uint16_t* pixel = out;
    for (int col = col0; col < col1; ++col) {
      const uint8_t coverage = *in++;
      if (coverage != 0) {
        uint32_t color = *pixel;
        if (draw_shadow && (coverage & 0x0F) != 0) {
          const uint32_t n = ((coverage & 0x0F) >> 3) + (coverage & 0x0F);
          color =
              ((m0 & (16 * (m1 & color) + n * (m1_shadow - (m1 & color)))) +
               (m2 & (16 * (m3 & color) + n * (m3_shadow - (m3 & color))))) >>
              4;
        }
        if ((coverage >> 4) != 0) {
          const uint32_t n = ((coverage >> 7) & 1) + (coverage >> 4);
          color = ((m0 & (16 * (m1 & color) + n * (m1_fg - (m1 & color)))) +
                   (m2 & (16 * (m3 & color) + n * (m3_fg - (m3 & color))))) >>
                  4;
        }
        *pixel = static_cast<uint16_t>(color);
      }
      ++pixel;
    }
    src += glyph.cell_w;
    out += pitch_px;
  }
}

void DrawLine(const Line& line, game::Bitmap* dst, int x, int y,
              FontContext* ctx, uint16_t fg_color, bool draw_shadow,
              uint16_t shadow_color, int clip_x1) {
  if (line.text == nullptr || line.len <= 0 || dst == nullptr ||
      ctx == nullptr) {
    return;
  }

  int pen_x = x;
  const uint8_t* p = line.text;
  const uint8_t* const end = line.text + line.len;
  while (p < end) {
    int len = 0;
    const uint32_t code = DecodeChar(p, &len);
    if (len <= 0 || p + len > end) break;

    const Glyph* glyph = ctx->GetGlyph(code);
    if (clip_x1 > 0 && pen_x + glyph->ink_x + glyph->ink_w > clip_x1) break;

    BlitGlyph(*glyph, dst, pen_x, y, fg_color, draw_shadow, shadow_color);
    pen_x += glyph->advance;
    p += len;
  }
}

}  // namespace h4cn
