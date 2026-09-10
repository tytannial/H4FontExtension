#include "wrap.h"

namespace h4cn {
namespace {

constexpr uint8_t kSpace = ' ';
constexpr uint8_t kNewLine = '\n';

bool IsGbkLead(uint8_t byte) { return byte >= 0x81 && byte <= 0xFE; }

bool IsGbkTrail(uint8_t byte) {
  return (byte >= 0x40 && byte <= 0x7E) || (byte >= 0x80 && byte <= 0xFE);
}

}  // namespace

uint32_t DecodeChar(const uint8_t* text, int* len) {
  if (IsGbkLead(text[0]) && IsGbkTrail(text[1])) {
    *len = 2;
    return (static_cast<uint32_t>(text[0]) << 8) | text[1];
  }
  *len = 1;
  return text[0];
}

namespace {

// Width of the widest word (run between spaces and newlines), the same
// segmentation the stock longest-word pass at 0x71C5E0 uses. Measured with the
// advancer, which is the one number the stock glyph table got wrong for GBK.
int LongestWordWidth(const uint8_t* text, Advancer& advancer) {
  int longest = 0;
  const uint8_t* p = text;
  while (*p != 0) {
    while (*p == kSpace || *p == kNewLine) ++p;
    int word = 0;
    while (*p != 0 && *p != kSpace && *p != kNewLine) {
      int len = 0;
      word += advancer.Advance(DecodeChar(p, &len));
      p += len;
    }
    if (word > longest) longest = word;
  }
  return longest;
}

}  // namespace

OptimalWrap WrapTextOptimal(const uint8_t* text, int min_width, int max_width,
                            int line_height, Advancer& advancer,
                            std::vector<Line>* lines) {
  OptimalWrap optimal;
  if (text == nullptr || line_height <= 0) return optimal;

  const int upper = max_width < 0 ? 0 : max_width;
  int width = LongestWordWidth(text, advancer);
  if (width > upper) width = upper;
  if (min_width > width) width = min_width;

  WrapResult result;
  // Reusable scratch rows: the growth loop re-wraps the whole text once per
  // width step, and this is the only place those rows exist between steps.
  // The caller gets a copy-assign at the end - Line is a trivial {ptr,len}
  // view, copying a dozen of them is cheaper than the malloc a move-out would
  // leave the scratch to re-pay on the next call. thread_local keeps it safe
  // with no shared state; WrapText itself stays pure.
  static thread_local std::vector<Line> wrapped;
  for (;;) {
    wrapped.clear();
    result = WrapText(text, width, advancer, &wrapped);
    // One line is the ideal shape, the original's first break.
    if (result.line_count <= 1) break;
    if (result.max_width == 0) break;
    if (width >= upper) break;
    // Stop once the block stops being tall for its width, as the stock aspect
    // ratio does: four times the text height no longer exceeds the width.
    if (4 * result.line_count * line_height <= width) break;
    width = width * 3 / 2;
    if (width > upper) width = upper;
  }

  if (lines != nullptr) *lines = wrapped;
  optimal.width = width;
  optimal.result = result;
  return optimal;
}

WrapResult WrapText(const uint8_t* text, int width, Advancer& advancer,
                    std::vector<Line>* lines) {
  WrapResult result;
  if (text == nullptr || width <= 0) return result;

  const uint8_t* line_start = text;
  const uint8_t* p = text;
  int line_w = 0;     // committed width, internal spaces included
  int pending_w = 0;  // width of the trailing space run, not committed yet
  const uint8_t* trim_end = text;   // committed content end, spaces excluded
  const uint8_t* resume = nullptr;  // first byte after that space run
  int trim_w = 0;                   // width of [line_start, trim_end)

  auto emit = [&](const uint8_t* end, int w) {
    ++result.line_count;
    if (w > result.max_width) result.max_width = w;
    if (lines != nullptr) {
      lines->push_back(Line{line_start, static_cast<int>(end - line_start)});
    }
  };
  auto start_line = [&](const uint8_t* next) {
    line_start = next;
    p = next;
    line_w = 0;
    pending_w = 0;
    trim_end = next;
    trim_w = 0;
    resume = nullptr;
  };

  while (*p != 0) {
    if (*p == kNewLine) {
      // A newline emits a line even when it is empty, as the stock wrap does.
      emit(pending_w > 0 ? trim_end : p, pending_w > 0 ? trim_w : line_w);
      start_line(p + 1);
      continue;
    }

    if (*p == kSpace) {
      // Record the run as the preferred break point, but do not commit its
      // width: trailing spaces are dropped when the line ends here.
      trim_end = p;
      trim_w = line_w;
      const int space_advance = advancer.Advance(kSpace);
      int space_w = 0;
      while (*p == kSpace) {
        space_w += space_advance;
        ++p;
      }
      pending_w = space_w;
      resume = p;
      continue;
    }

    int len = 0;
    const uint32_t code = DecodeChar(p, &len);
    const int advance = advancer.Advance(code);
    const int used = line_w + pending_w;
    if (used > 0 && used + advance > width) {
      if (resume != nullptr) {
        // Break at the last space run so the current word moves whole. A line
        // that held nothing but spaces is dropped instead of emitted.
        if (trim_end > line_start) emit(trim_end, trim_w);
        start_line(resume);
      } else {
        // No break point: CJK text, or a word longer than the line. Break
        // before this character; pending_w is always 0 in this branch.
        emit(p, line_w);
        start_line(p);
      }
      continue;  // the current character is re-examined on the new line
    }

    line_w = used + advance;
    pending_w = 0;
    p += len;
  }

  // Like the stock wrap, a tail holding nothing but spaces yields no line.
  const uint8_t* tail_end = (pending_w > 0) ? trim_end : p;
  if (tail_end > line_start) {
    emit(tail_end, (pending_w > 0) ? trim_w : line_w);
  }
  return result;
}

}  // namespace h4cn
