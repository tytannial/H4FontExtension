// Host tests for ParseConfig / Config::Resolve - the pure half of H4CN.toml.
// Built only with -DH4CN_BUILD_TESTS=ON; they exercise no file, no registry,
// no game memory and no GDI.
#include <string>
#include <vector>

#include "../config.h"
#include "test_check.h"

namespace {

int CountProblems(const std::vector<std::string>& problems,
                  const std::string& needle) {
  int count = 0;
  for (const std::string& p : problems) {
    if (p.find(needle) != std::string::npos) ++count;
  }
  return count;
}

void Resolve(const h4cn::Config& cfg, int game_size, std::string* face,
             int* render, int* supersample = nullptr) {
  int ss = 0;
  cfg.Resolve(game_size, face, render, &ss);
  if (supersample != nullptr) *supersample = ss;
}

void TestEmptyTextKeepsDefaults() {
  std::vector<std::string> problems;
  const h4cn::Config cfg = h4cn::ParseConfig("", &problems);
  CHECK(problems.empty());
  CHECK(cfg.log_file);
  CHECK(cfg.log_debug_view);
  CHECK(cfg.patch_line_height);
  CHECK(cfg.fallback_face == "LiSu");
  CHECK(cfg.default_bias == 4);
  CHECK(cfg.render_supersample == 2);  // branch experimental default
  CHECK(cfg.per_size.empty());
  std::string face;
  int render = 0;
  int ss = 0;
  Resolve(cfg, 16, &face, &render, &ss);
  CHECK(face == "LiSu" && render == 20);
  CHECK(ss == 2);  // no per-size override -> [render] default
}

void TestFullSchema() {
  const std::string text =
      "[general]\n"
      "log_file = false\n"
      "log_debug_view = true\n"
      "patch_line_height = false\n"
      "[render]\n"
      "supersample = 3\n"
      "[fonts]\n"
      "fallback_face = \"SimHei\"\n"
      "default_bias = 6\n"
      "[fonts.16]\n"
      "face = \"Microsoft YaHei\"\n"
      "bias = 2\n"
      "[fonts.9]\n"
      "size = 14\n";
  std::vector<std::string> problems;
  const h4cn::Config cfg = h4cn::ParseConfig(text, &problems);
  CHECK(problems.empty());
  CHECK(!cfg.log_file && cfg.log_debug_view && !cfg.patch_line_height);
  CHECK(cfg.render_supersample == 3);
  CHECK(cfg.fallback_face == "SimHei");
  CHECK(cfg.default_bias == 6);
  CHECK(cfg.per_size.size() == 2);

  std::string face;
  int render = 0;
  Resolve(cfg, 16, &face, &render);
  CHECK(face == "Microsoft YaHei" && render == 18);  // bias 2 wins per-size
  Resolve(cfg, 9, &face, &render);
  CHECK(face == "SimHei" && render == 14);  // explicit size, fallback face
  Resolve(cfg, 12, &face, &render);
  CHECK(face == "SimHei" && render == 18);  // default_bias 6
}

void TestPrioritySizeOverBiasOverDefault() {
  const h4cn::Config cfg = h4cn::ParseConfig(
      "[fonts.20]\nface = \"LiSu\"\nsize = 30\nbias = -15\n", nullptr);
  std::string face;
  int render = 0;
  Resolve(cfg, 20, &face, &render);
  CHECK(render == 30);  // size wins over bias, whatever the bias says
}

void TestSyntaxErrorFallsBackToDefaults() {
  std::vector<std::string> problems;
  const h4cn::Config cfg =
      h4cn::ParseConfig("[general\nlog_file = ", &problems);
  CHECK(problems.size() == 1);
  CHECK(CountProblems(problems, "syntax error") == 1);
  CHECK(cfg.fallback_face == "LiSu" && cfg.log_file);
}

void TestTypeAndRangeProblems() {
  // Root-level key first (bare keys must precede any [table] header), then a
  // battery of broken values.
  const std::string text =
      "top_level_junk = 1\n"
      "[general]\n"
      "log_file = 3\n"
      "nonsense = true\n"
      "[fonts]\n"
      "fallback_face = 12\n"
      "default_bias = \"wide\"\n"
      "mystery = 1\n"
      "[fonts.16]\n"
      "face = 7\n"
      "size = 999\n"
      "bias = -999\n"
      "oops = 1\n"
      "[fonts.0]\n"
      "size = 10\n"
      "[fonts.16x]\n"
      "size = 10\n";
  std::vector<std::string> problems;
  const h4cn::Config cfg = h4cn::ParseConfig(text, &problems);
  // Every broken item is named, and every one of them falls back to its
  // default instead of poisoning the neighbours.
  CHECK(CountProblems(problems, "'log_file' must be true/false") == 1);
  CHECK(CountProblems(problems, "[general]: unknown key 'nonsense'") == 1);
  CHECK(CountProblems(problems, "'fallback_face' must be a string") == 1);
  CHECK(CountProblems(problems, "'default_bias' must be an integer") == 1);
  CHECK(CountProblems(problems, "[fonts]: unknown key 'mystery'") == 1);
  CHECK(CountProblems(problems, "'face' must be a string") == 1);
  CHECK(CountProblems(problems, "size out of range") == 1);
  CHECK(CountProblems(problems, "smaller than 1 px") == 1);
  CHECK(CountProblems(problems, "unknown key 'oops'") == 1);
  CHECK(CountProblems(problems, "[fonts] key '0' is not a game font size") ==
        1);
  CHECK(CountProblems(problems, "'16x' is not a game font size") == 1);
  CHECK(CountProblems(problems, "unknown key 'top_level_junk'") == 1);
  CHECK(cfg.log_file);  // bad value -> default kept
  CHECK(cfg.fallback_face == "LiSu");
  CHECK(cfg.default_bias == 4);
  // [fonts.16] survived as an empty-but-present mapping: face falls back, and
  // both broken measurements fall back to the (default) bias as well.
  std::string face;
  int render = 0;
  Resolve(cfg, 16, &face, &render);
  CHECK(face == "LiSu" && render == 20);
  CHECK(cfg.per_size.count(0) == 0 && cfg.per_size.count(16) == 1);
}

void TestEmptyFacesKeepDefaults() {
  std::vector<std::string> problems;
  const h4cn::Config cfg = h4cn::ParseConfig(
      "[fonts]\nfallback_face = \"\"\n[fonts.12]\nface = \"\"\n", &problems);
  CHECK(CountProblems(problems, "fallback_face is empty") == 1);
  CHECK(cfg.fallback_face == "LiSu");
  std::string face;
  int render = 0;
  Resolve(cfg, 12, &face, &render);
  CHECK(face == "LiSu");  // per-size empty face means "use the fallback"
}

void TestUtf8FaceNameRoundTrips() {
  // 微软雅黑 in UTF-8, as a player writes it and as toml++ hands it out.
  const h4cn::Config cfg = h4cn::ParseConfig(
      "[fonts.34]\nface = "
      "\"\xe5\xbe\xae\xe8\xbd\xaf\xe9\x9b\x85\xe9\xbb\x91\"\n",
      nullptr);
  std::string face;
  int render = 0;
  Resolve(cfg, 34, &face, &render);
  CHECK(face == "\xe5\xbe\xae\xe8\xbd\xaf\xe9\x9b\x85\xe9\xbb\x91");
  CHECK(render == 38);
}

void TestOversizedAndNegativeBounds() {
  std::vector<std::string> problems;
  const h4cn::Config cfg = h4cn::ParseConfig(
      "[fonts.16]\nsize = 0\n[fonts.18]\nsize = -4\n[fonts.20]\nsize = 256\n",
      &problems);
  CHECK(CountProblems(problems, "size out of range") == 2);
  std::string face;
  int render = 0;
  Resolve(cfg, 16, &face, &render);
  CHECK(render == 20);  // fell back to the default bias
  Resolve(cfg, 18, &face, &render);
  CHECK(render == 22);
  Resolve(cfg, 20, &face, &render);
  CHECK(render == 256);  // the 256 limit is inclusive
}

void TestRenderSupersample() {
  // 1 is the accepted identity value; it must parse through unchanged.
  CHECK(h4cn::ParseConfig("[render]\nsupersample = 1\n", nullptr)
            .render_supersample == 1);
  // Below the 1..4 window (0) and above it (9) fall back to the default and
  // are reported; a non-integer is rejected the same way.
  std::vector<std::string> too_small;
  CHECK(h4cn::ParseConfig("[render]\nsupersample = 0\n", &too_small)
            .render_supersample == 2);
  CHECK(CountProblems(too_small, "supersample out of range") == 1);
  std::vector<std::string> too_big;
  CHECK(h4cn::ParseConfig("[render]\nsupersample = 9\n", &too_big)
            .render_supersample == 2);
  CHECK(CountProblems(too_big, "supersample out of range") == 1);
  std::vector<std::string> bad_type;
  CHECK(h4cn::ParseConfig("[render]\nsupersample = \"x4\"\n", &bad_type)
            .render_supersample == 2);
  CHECK(CountProblems(bad_type, "'supersample' must be an integer") == 1);
  // A non-table [render] and an unknown key inside it are both named.
  std::vector<std::string> not_table;
  h4cn::ParseConfig("render = 5\n", &not_table);
  CHECK(CountProblems(not_table, "[render] must be a table") == 1);
  std::vector<std::string> unknown;
  h4cn::ParseConfig("[render]\nblur = true\n", &unknown);
  CHECK(CountProblems(unknown, "unknown key 'blur'") == 1);
}

void TestUnparseableBiasKeepsMappingUsable() {
  // A broken [fonts.16] must still apply its valid keys: here only face.
  std::vector<std::string> problems;
  const h4cn::Config cfg = h4cn::ParseConfig(
      "[fonts.16]\nface = \"SimHei\"\nbias = 1000000\n", &problems);
  CHECK(CountProblems(problems, "out of range") == 1);
  std::string face;
  int render = 0;
  Resolve(cfg, 16, &face, &render);
  CHECK(face == "SimHei");
  CHECK(render == 20);  // bias rejected, default bias used
}

void TestPerSizeSupersampleOverride() {
  // Global is 2; size 8 opts back to point-to-point (1). Size 16 (no override)
  // keeps the global default. supersample must not trip the unknown-key path.
  std::vector<std::string> problems;
  const h4cn::Config cfg = h4cn::ParseConfig(
      "[fonts.8]\nsupersample = 1\n[fonts.12]\nsize = 14\nsupersample = 3\n",
      &problems);
  CHECK(problems.empty());
  std::string face;
  int render = 0;
  int ss = 0;
  Resolve(cfg, 8, &face, &render, &ss);
  CHECK(ss == 1);  // per-size override wins
  Resolve(cfg, 12, &face, &render, &ss);
  CHECK(render == 14 && ss == 3);  // explicit size + per-size supersample
  Resolve(cfg, 16, &face, &render, &ss);
  CHECK(ss == 2);  // no override -> [render] default

  // An out-of-range per-size value is ignored and the global default applies.
  std::vector<std::string> bad;
  const h4cn::Config cfg2 =
      h4cn::ParseConfig("[fonts.20]\nsupersample = 7\n", &bad);
  CHECK(CountProblems(bad, "supersample out of range") == 1);
  CHECK(cfg2.render_supersample == 2);
  Resolve(cfg2, 20, &face, &render, &ss);
  CHECK(ss == 2);  // rejected override -> global default
}

}  // namespace

int main() {
  try {
    TestEmptyTextKeepsDefaults();
    TestFullSchema();
    TestPrioritySizeOverBiasOverDefault();
    TestSyntaxErrorFallsBackToDefaults();
    TestTypeAndRangeProblems();
    TestEmptyFacesKeepDefaults();
    TestRenderSupersample();
    TestPerSizeSupersampleOverride();
    TestUtf8FaceNameRoundTrips();
    TestOversizedAndNegativeBounds();
    TestUnparseableBiasKeepsMappingUsable();
  } catch (...) {
    std::printf("config_tests: unexpected exception escaped a test\n");
    return 2;
  }

  return testcheck::Finish("config_tests");
}
