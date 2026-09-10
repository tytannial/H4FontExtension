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

// t_font_bitmap::draw_to @0x71B820: the game's own per-glyph RGB565 blitter.
// It is NOT one of the seven patched entry points (it sits below all of them)
// and is fully intact at runtime, so calling it reproduces the stock pixels
// exactly instead of re-deriving the blend. __thiscall: the t_font_bitmap* goes
// in ECX, the rest on the stack - the same shape the other called game helpers
// use.
using FnFontBlitGlyph = void(__thiscall*)(const game::FontBitmap*,
                                          game::Bitmap*, int, int, uint16_t,
                                          uint8_t, uint16_t);

void BlitOriginal(const game::FontBitmap& glyph, game::Bitmap* dst, int x,
                  int y, uint16_t fg_color, bool draw_shadow,
                  uint16_t shadow_color) {
  if (dst == nullptr || dst->buffer == nullptr) return;
  auto blit = reinterpret_cast<FnFontBlitGlyph>(game::kAddrFontBlitGlyph);
  blit(&glyph, dst, x, y, fg_color, draw_shadow ? 1 : 0, shadow_color);
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

const game::FontBitmap* GlyphRouter::OriginalGlyph(uint32_t code) const {
  if (!ascii_original_ || font_ == nullptr || font_->glyphs == nullptr) {
    return nullptr;
  }
  // Only single-byte printable ASCII; every GBK code DecodeChar produces is
  // >= 0x8000 and fails this bound, so no multi-byte character is ever split.
  if (code < 0x20 || code > 0x7E) return nullptr;
  const int idx = static_cast<int>(code) - font_->first_char;
  if (idx < 0 || idx >= font_->glyph_count) return nullptr;
  return &font_->glyphs[idx];
}

int GlyphRouter::Advance(uint32_t code) {
  const game::FontBitmap* glyph = OriginalGlyph(code);
  if (glyph != nullptr) {
    // Same no-trim per-glyph model the draw loop uses below, so measuring and
    // laying out can never drift apart.
    return glyph->margin_left + glyph->width + glyph->margin_right;
  }
  return locked_ ? ctx_->AdvanceLocked(code) : ctx_->Advance(code);
}

void DrawLine(const Line& line, game::Bitmap* dst, int x, int y,
              GlyphRouter& router, uint16_t fg_color, bool draw_shadow,
              uint16_t shadow_color, int clip_x1) {
  FontContext* ctx = router.context();
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

    const game::FontBitmap* original = router.OriginalGlyph(code);
    if (original != nullptr) {
      // draw_to_pt places the bitmap at the pen after the left bearing and
      // advances by width + right bearing (plus the left bearing here, matching
      // Advance()); y is the shared line origin, like the stock renderer.
      const int glyph_x = pen_x + original->margin_left;
      if (clip_x1 > 0 && glyph_x + original->width > clip_x1) break;
      BlitOriginal(*original, dst, glyph_x, y, fg_color, draw_shadow,
                   shadow_color);
      pen_x += original->margin_left + original->width + original->margin_right;
      p += len;
      continue;
    }

    const Glyph* glyph = router.Glyph(code);
    if (clip_x1 > 0 && pen_x + glyph->ink_x + glyph->ink_w > clip_x1) break;

    BlitGlyph(*glyph, dst, pen_x, y, fg_color, draw_shadow, shadow_color);
    pen_x += glyph->advance;
    p += len;
  }
}

}  // namespace h4cn
