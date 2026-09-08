// Host tests for WrapText - the one line-breaking implementation the game
// hooks share. Built only with -DH4CN_BUILD_TESTS=ON; a plain console exe, it
// links neither the GDI cache nor any game memory.
#include <cstring>
#include <string>
#include <vector>

#include "../wrap.h"
#include "test_check.h"

namespace {

// Monospace stand-in: one byte = 8 px, one GBK character = 16 px.
class FakeAdvancer : public h4cn::Advancer {
 public:
  int Advance(uint32_t code) override { return code > 0xFF ? 16 : 8; }
};

const uint8_t* B(const char* s) { return reinterpret_cast<const uint8_t*>(s); }

std::vector<std::string> ToStringLines(const std::vector<h4cn::Line>& lines) {
  std::vector<std::string> out;
  out.reserve(lines.size());
  for (const h4cn::Line& line : lines) {
    out.emplace_back(reinterpret_cast<const char*>(line.text), line.len);
  }
  return out;
}

struct Wrapped {
  h4cn::WrapResult result;
  std::vector<std::string> lines;
};

Wrapped Run(const char* text, int width) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
  std::vector<h4cn::Line> lines;
  FakeAdvancer advancer;
  Wrapped w{h4cn::WrapText(bytes, width, advancer, &lines),
            ToStringLines(lines)};
  return w;
}

void TestNullAndEmpty() {
  FakeAdvancer advancer;
  CHECK(h4cn::WrapText(nullptr, 100, advancer, nullptr).line_count == 0);
  const uint8_t empty[] = "";
  CHECK(h4cn::WrapText(empty, 100, advancer, nullptr).line_count == 0);
  // Non-positive width yields nothing, as the callers already guard for it.
  CHECK(h4cn::WrapText(empty, 0, advancer, nullptr).line_count == 0);
  CHECK(h4cn::WrapText(empty, -5, advancer, nullptr).line_count == 0);
}

void TestFitsOnOneLine() {
  const Wrapped w = Run("abcd efgh", 72);  // exactly 9 cells of 8 px
  CHECK(w.result.line_count == 1);
  CHECK(w.result.max_width == 72);
  CHECK(w.lines.size() == 1 && w.lines[0] == "abcd efgh");
}

void TestAsciiBreaksAtWordBoundary() {
  const Wrapped w = Run("abcd efgh", 48);
  CHECK(w.result.line_count == 2);
  CHECK(w.result.max_width == 32);
  CHECK(w.lines.size() == 2 && w.lines[0] == "abcd" && w.lines[1] == "efgh");
}

void TestTrailingSpacesAreDropped() {
  const Wrapped w = Run("ab  ", 16);
  CHECK(w.result.line_count == 1);
  CHECK(w.result.max_width == 16);
  CHECK(w.lines.size() == 1 && w.lines[0] == "ab");
}

void TestTrailingSpaceAfterWordBreak() {
  // The break point is the space run; it is dropped from the first line but
  // the second line must not re-absorb it.
  const Wrapped w = Run("ab  cd", 24);
  CHECK(w.result.line_count == 2);
  CHECK(w.lines.size() == 2 && w.lines[0] == "ab" && w.lines[1] == "cd");
}

void TestNewlinesAndBlankLines() {
  const Wrapped w = Run("a\n\nb", 100);
  CHECK(w.result.line_count == 3);
  CHECK(w.lines.size() == 3 && w.lines[0] == "a" && w.lines[1] == "" &&
        w.lines[2] == "b");
}

void TestNoTrailingLineForSpaceTail() {
  // Stock behaviour: text ending in the middle of nothing emits nothing, and a
  // line holding only spaces is dropped rather than emitted empty.
  const Wrapped w = Run("ab cd   ", 24);
  // width 24 forces: "ab" | "cd", the trailing spaces hang off "cd" and drop.
  CHECK(w.result.line_count == 2);
  CHECK(w.result.max_width == 16);
  CHECK(w.lines.size() == 2 && w.lines[0] == "ab" && w.lines[1] == "cd");
}

void TestCjkBreaksCharByChar() {
  // Three GBK characters (D6D0 CEC4 D7D6), 16 px each, width 32: two + one.
  const Wrapped w = Run("\xd6\xd0\xce\xc4\xd7\xd6", 32);
  CHECK(w.result.line_count == 2);
  CHECK(w.result.max_width == 32);
  CHECK(w.lines.size() == 2 && w.lines[0] == "\xd6\xd0\xce\xc4" &&
        w.lines[1] == "\xd7\xd6");
}

void TestGbkTrailByteLookLikeAsciiNeverBreaksInside() {
  // GBK B0 4F: the trail byte 'O' is ASCII-range; the pair must still stay
  // on one line and never be treated as a Latin word character.
  const Wrapped w = Run("\xb0\x4f", 16);
  CHECK(w.result.line_count == 1);
  CHECK(w.result.max_width == 16);
  CHECK(w.lines.size() == 1 && w.lines[0] == "\xb0\x4f");
}

void TestMixedCjkAndLatinKeepsWordsWhole() {
  // "hi [CJK CJK] x" at 24 px: hi | CJK | CJK | x - the Latin word never
  // splits and CJK never joins a Latin word across its trailing space.
  const Wrapped w = Run("hi \xd6\xd0\xce\xc4 x", 24);
  CHECK(w.result.line_count == 4);
  CHECK(w.lines.size() == 4 && w.lines[0] == "hi" && w.lines[1] == "\xd6\xd0" &&
        w.lines[2] == "\xce\xc4" && w.lines[3] == "x");
}

void TestUnbreakableWordStillMakesProgress() {
  // "abcdef" at 8 px: no space at all, every character on its own line. The
  // loop must terminate - a width smaller than one advance cannot stall it.
  const Wrapped w = Run("abcdef", 8);
  CHECK(w.result.line_count == 6);
  CHECK(w.result.max_width == 8);
}

void TestCountOnlyMode() {
  const uint8_t text[] = "one two three";
  std::vector<h4cn::Line> lines;
  FakeAdvancer advancer;
  const h4cn::WrapResult counted = h4cn::WrapText(text, 40, advancer, nullptr);
  const h4cn::WrapResult filled = h4cn::WrapText(text, 40, advancer, &lines);
  // The height hook passes nullptr; it must agree with the drawing hooks.
  CHECK(counted.line_count == filled.line_count);
  CHECK(counted.max_width == filled.max_width);
  CHECK(lines.size() == static_cast<size_t>(filled.line_count));
}

void TestMaxWidthIsTheWidestLine() {
  const Wrapped w = Run("aaaaaaaaaa bb cc", 88);
  // "aaaaaaaaaa bb" is 13 cells = 104 px, so it breaks after "aaaaaaaaaa"
  // (80 px), then "bb cc" (40 px). The widest line is the first.
  CHECK(w.result.line_count == 2);
  CHECK(w.result.max_width == 80);
}

void TestOptimalSingleWordKeepsMinWidth() {
  FakeAdvancer advancer;
  std::vector<h4cn::Line> lines;
  // "abc" is 24 px, below min_width 50: the search starts at min_width and
  // stops immediately (already one line). Return value = widest line = 24.
  const h4cn::OptimalWrap o =
      h4cn::WrapTextOptimal(B("abc"), 50, 100, 20, advancer, &lines);
  CHECK(o.width == 50);
  CHECK(o.result.line_count == 1);
  CHECK(o.result.max_width == 24);
  CHECK(lines.size() == 1);
}

void TestOptimalGrowsThroughTheStockSequence() {
  FakeAdvancer advancer;
  std::vector<h4cn::Line> lines;
  // Twenty "ab" words (16 px each, 8 px spaces) in [50..200], line height 10:
  // 50 -> 2 words/line, 10 lines, aspect 4*10*10=400>50  -> grow
  // 75 -> 3/line,            4*7*10=280>75   -> grow
  // 112 -> 5/line,           4*4*10=160>112  -> grow
  // 168 -> 7/line, 3 lines,  4*3*10=120<=168 -> stop
  std::string text;
  for (int i = 0; i < 20; ++i) {
    if (!text.empty()) text += ' ';
    text += "ab";
  }
  const h4cn::OptimalWrap o =
      h4cn::WrapTextOptimal(B(text.c_str()), 50, 200, 10, advancer, &lines);
  CHECK(o.width == 168);
  CHECK(o.result.line_count == 3);
  CHECK(o.result.max_width == 160);
  CHECK(lines.size() == 3);

  // The filled lines must be exactly what a plain wrap at the returned width
  // produces - callers wrap once but measure with the other hooks.
  std::vector<h4cn::Line> direct;
  const h4cn::WrapResult check =
      h4cn::WrapText(B(text.c_str()), o.width, advancer, &direct);
  CHECK(check.line_count == o.result.line_count);
  CHECK(check.max_width == o.result.max_width);
}

void TestOptimalNegativeMaxWidthClampsToZero() {
  FakeAdvancer advancer;
  // max_width < 0 clamps the search ceiling to 0 (stock behaviour), so the
  // first wrap already ends the loop at max(min_width, 0).
  const h4cn::OptimalWrap o =
      h4cn::WrapTextOptimal(B("ab ab ab ab ab"), 50, -1, 10, advancer, nullptr);
  // Ceiling 0 stops the growth after the first wrap; 5 words at 2/line -> 3.
  CHECK(o.width == 50);
  CHECK(o.result.line_count == 3);
  CHECK(o.result.max_width == 40);
}

void TestOptimalCjkLongestWordNoSpaceClamp() {
  FakeAdvancer advancer;
  // A 10-character CJK run has no spaces: longest word = 160 px, clamped to
  // the 100 px ceiling. The stock glyph-table measurement would have sized it
  // as ten space glyphs - exactly the popup bug this hook fixes.
  const char text[] =
      "\xd6\xd0\xce\xc4\xd6\xd0\xce\xc4\xd6\xd0\xce\xc4"
      "\xd6\xd0\xce\xc4\xd6\xd0\xce\xc4";  // 10 glyphs
  const h4cn::OptimalWrap o =
      h4cn::WrapTextOptimal(B(text), 50, 100, 20, advancer, nullptr);
  CHECK(o.width == 100);  // 6 glyphs fit (96 px), 4*2*20=160 > 100 and
                          // width >= ceiling stops the growth
  CHECK(o.result.line_count == 2);
  CHECK(o.result.max_width == 96);
}

void TestOptimalNullAndEmpty() {
  FakeAdvancer advancer;
  CHECK(h4cn::WrapTextOptimal(nullptr, 50, 100, 20, advancer, nullptr).width ==
        0);
  CHECK(h4cn::WrapTextOptimal(B(""), 50, 100, 20, advancer, nullptr).width ==
        50);
  CHECK(h4cn::WrapTextOptimal(B(""), 50, 100, 0, advancer, nullptr).width == 0);
}

void TestExactFitBoundary() {
  // 88 px of content ("aaaaaaaa bb") in 88 px of width stays on one line
  // (width is inclusive); 87 px forces a break after "aaaaaaaa" (64 px).
  const Wrapped fits = Run("aaaaaaaa bb", 88);
  CHECK(fits.result.line_count == 1);
  CHECK(fits.result.max_width == 88);
  const Wrapped breaks = Run("aaaaaaaa bb", 87);
  CHECK(breaks.result.line_count == 2);
  CHECK(breaks.result.max_width == 64);
}

}  // namespace

// All test bodies are plain calls; a throw here would be a logic error in the
// harness (allocation failure), which we report rather than let escape main().
int main() {
  try {
    TestNullAndEmpty();
    TestFitsOnOneLine();
    TestAsciiBreaksAtWordBoundary();
    TestTrailingSpacesAreDropped();
    TestTrailingSpaceAfterWordBreak();
    TestNewlinesAndBlankLines();
    TestNoTrailingLineForSpaceTail();
    TestCjkBreaksCharByChar();
    TestGbkTrailByteLookLikeAsciiNeverBreaksInside();
    TestMixedCjkAndLatinKeepsWordsWhole();
    TestUnbreakableWordStillMakesProgress();
    TestCountOnlyMode();
    TestMaxWidthIsTheWidestLine();
    TestExactFitBoundary();
    TestOptimalSingleWordKeepsMinWidth();
    TestOptimalGrowsThroughTheStockSequence();
    TestOptimalNegativeMaxWidthClampsToZero();
    TestOptimalCjkLongestWordNoSpaceClamp();
    TestOptimalNullAndEmpty();
  } catch (...) {
    std::printf("wrap_tests: unexpected exception escaped a test\n");
    return 2;
  }

  return testcheck::Finish("wrap_tests");
}
