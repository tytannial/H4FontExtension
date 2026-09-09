// Byte-exact mirrors of the heroes4.exe (2003 Complete) structures the plugin
// touches. Offsets and field names follow the IDB type library
// (heroes4.exe.i64); see docs/PROGRESS.md sections 2 and 3.
//
// These describe *game* memory. Never allocate, resize or reinterpret them with
// a modern MSVC STL type: the exe was built with the VC6 ABI.
#ifndef H4CN_GAME_TYPES_H_
#define H4CN_GAME_TYPES_H_

#include <cstddef>
#include <cstdint>

namespace game {

// t_bitmap (20 bytes): RGB565 framebuffer. `pitch` is in bytes.
struct Bitmap {
  uint32_t unk0;
  int32_t width;
  int32_t height;
  int32_t pitch;
  uint16_t* buffer;
};

// t_rect (16 bytes): screen rectangle, edges exclusive.
struct Rect {
  int32_t x;
  int32_t y;
  int32_t x1;
  int32_t y1;
};

// t_font_bitmap (44 bytes): one glyph's rasterised cell in the game's own
// encoding, produced by t_font_bitmap::read @0x71BAA0. `bits` is a `height` x
// `pitch` buffer, one byte per pixel, already converted at load time to the
// high-nibble foreground / low-nibble shadow form that t_font_bitmap::draw_to
// @0x71B820 (and our blit.cc) consume. `width`/`margin_left`/`margin_right`
// carry the horizontal spacing the stock get_width/draw_to use; `margin_top`
// exists in the resource but the per-line draw does not apply it.
struct FontBitmap {
  uint32_t flags;        // +0x00 dtor fn pointer, called by t_font::read
  int32_t width;         // +0x04
  int32_t height;        // +0x08
  int32_t pitch;         // +0x0C bytes per row of bits
  uint8_t* bits;         // +0x10 pixel data
  int32_t margin_left;   // +0x14
  int32_t margin_right;  // +0x18
  int32_t margin_top;    // +0x1C
  uint8_t* bits2;        // +0x20 allocation cursor used only by read()
  uint32_t pad24;        // +0x24
  uint32_t pad28;        // +0x28
};

// t_font (28 bytes). `size`, `first_char`, `line_height`, `unk10` and
// `glyph_count` are each read as one byte from the .fon resource by
// t_font::read @0x71BEE0, so glyph_count can never exceed 255: the stock glyph
// tables have no room for GBK, which is why this plugin exists.
//
// `glyphs` is the original bitmap table, indexed by `ch - first_char` (a
// contiguous range, verified at runtime: first_char 0x1F, count 225 -> covers
// all of 0x20-0x7E). We never modify it; "keep original ASCII" blits straight
// from it via t_font_bitmap::draw_to @0x71B820.
//
// `size` is the only field the plugin treats as read-only; `line_height` is
// rewritten on every hook entry (see font_cache.h PatchFontMetrics).
struct Font {
  uint32_t res_id;      // +0x00
  int32_t size;         // +0x04 nominal pixel size
  int32_t first_char;   // +0x08
  int32_t line_height;  // +0x0C line advance used by every layout path
  int32_t unk10;        // +0x10
  int32_t glyph_count;  // +0x14
  FontBitmap* glyphs;   // +0x18 t_font_bitmap[glyph_count]
};

// VC6 std::basic_string<char> (16 bytes). The text buffer is reference counted
// and the count lives at ptr[-1]; `unk0` is the (unused) allocator base.
struct Vc6String {
  uint32_t unk0;
  char* ptr;
  size_t size;
  size_t capacity;
};

// VC6 std::vector<std::string> (16 bytes).
struct StringVector {
  uint32_t unk0;
  Vc6String* begin;
  Vc6String* end;
  Vc6String* myend;
};

static_assert(sizeof(Bitmap) == 20, "t_bitmap layout drifted");
static_assert(sizeof(Rect) == 16, "t_rect layout drifted");
static_assert(sizeof(FontBitmap) == 44, "t_font_bitmap layout drifted");
static_assert(sizeof(Font) == 28, "t_font layout drifted");
static_assert(offsetof(FontBitmap, bits) == 0x10, "bits must stay at +0x10");
static_assert(offsetof(FontBitmap, width) == 0x04, "width must stay at +0x04");
static_assert(offsetof(Font, line_height) == 0x0C,
              "line_height must stay at +0x0C");
static_assert(sizeof(Vc6String) == 16, "VC6 std::string layout drifted");
static_assert(sizeof(StringVector) == 16, "VC6 std::vector layout drifted");
static_assert(offsetof(Font, line_height) == 0x0C,
              "line_height must stay at +0x0C");
static_assert(offsetof(Bitmap, buffer) == 0x10, "buffer must stay at +0x10");
static_assert(offsetof(Vc6String, ptr) == 0x04, "_Ptr must stay at +0x04");
static_assert(offsetof(StringVector, begin) == 0x04,
              "_Begin must stay at +0x04");

}  // namespace game

#endif  // H4CN_GAME_TYPES_H_
