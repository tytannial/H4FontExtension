// Line breaking - the one place that decides where a line ends.
//
// This file is deliberately free of game memory and GDI: WrapText measures
// through the Advancer interface, so the exact function the shipping hooks call
// is unit-testable on the host (tests/wrap_test.cc).
//
// Everything that has to agree about *where a line ends* funnels through
// WrapText: HookFontWrapText (which fills the game's t_string_vector),
// HookFontDrawToRect (which draws the wrapped text) and
// HookFontGetWrappedHeight (whose line count the game turns into a scrollbar
// range and then divides by). Two independent implementations drifting apart is
// an integer divide-by-zero crash in the stock UI code, not just a visual bug.
#ifndef H4CN_WRAP_H_
#define H4CN_WRAP_H_

#include <cstdint>
#include <vector>

namespace h4cn {

// A wrapped line: the byte range [text, text + len) of the caller's string.
//
// Deliberately not NUL-terminated and deliberately not a copy. The old code
// wrote a temporary 0 into the game's buffer to reuse the single-line renderer
// and restored it afterwards, which is unsafe as soon as the buffer lives in
// .data/.rdata or is shared with another window.
struct Line {
  const uint8_t* text = nullptr;
  int len = 0;
};

struct WrapResult {
  int line_count = 0;
  int max_width = 0;  // widest line in pixels, the stock wrap_text return value
};

// Pen-advance source for measuring text. FontContext is the production
// implementation; the tests substitute a fake. Non-const because measuring
// populates the advance cache - but it must never rasterise.
class Advancer {
 public:
  virtual ~Advancer() = default;
  // Pixel advance of the character `code` (a byte, or a GBK lead<<8|trail).
  virtual int Advance(uint32_t code) = 0;
};

// Decodes one character at `text`: a GBK double byte when the lead and trail
// bytes are both in range, otherwise a single byte. Stores the byte length
// (1 or 2) in *len and returns the code used as cache key.
uint32_t DecodeChar(const uint8_t* text, int* len);

// Splits `text` into lines no wider than `width` pixels, measured with
// `advancer`.
//
// ASCII runs break at spaces and drop the trailing ones, which is what the
// stock t_font::wrap_text @0x71C100 does; GBK characters may break anywhere,
// since Chinese has no word separators. '\n' always ends a line. A single
// character wider than `width` is placed on a line of its own rather than
// looping forever.
//
// Appends one Line per wrapped line to *lines, unless it is null - counting
// without materialising is what the height hook and the optimal-width search
// of wrap_text(min, max) need.
WrapResult WrapText(const uint8_t* text, int width, Advancer& advancer,
                    std::vector<Line>* lines);

// Outcome of the optimal-width search: the chosen line width and the wrap at
// exactly that width.
struct OptimalWrap {
  int width = 0;
  WrapResult result;
};

// t_font::wrap_text(min, max) @0x71C5E0, stock control flow with GBK-aware
// measurement. The stock version sized its search from the game's glyph table,
// which clamps every GBK byte to the space glyph, so the starting width - and
// with it the popup size - was always wrong for Chinese.
//
// Same search as the original: start at max(min_width, min(longest word,
// max_width')), then grow the width by 3/2 per round until the text is one
// line, the width hits max_width', or the shape is tall-but-narrow
// (4 * line_count * line_height <= width). max_width' is max_width clamped to
// 0 when negative, like the stock code.
//
// Appends the final lines to *lines unless null. `result.max_width` is what
// the stock returned (the callers centre their rectangles on it).
OptimalWrap WrapTextOptimal(const uint8_t* text, int min_width, int max_width,
                            int line_height, Advancer& advancer,
                            std::vector<Line>* lines);

}  // namespace h4cn

#endif  // H4CN_WRAP_H_
