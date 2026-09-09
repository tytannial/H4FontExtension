// H4CN.toml - the player-facing configuration, read from the .asi's own
// directory. Everything has a default that reproduces the pre-config build
// exactly, except the two look changes under evaluation on this branch:
// [render].supersample (default 2; set 1 - or a per-size override - for the
// v3.1.0 rasterisation) and [render].ascii_original (default true; set false -
// or a per-size override - to draw printable ASCII with the GDI substitute like
// v3.1.0 did). So a missing file changes only those, and a broken one changes
// nothing else - it only complains in H4CN.log.
//
// The loader is deliberately not a DllMain participant: the file is read on
// the first hooked call (see EnsureConfigLoaded), outside the loader lock.
//
//   [general]
//   log_file          = true   # append to H4CN.log
//   log_debug_view    = true   # OutputDebugStringA
//   patch_line_height = true   # write the measured tmHeight back to the game
//
//   [render]
//   supersample = 2            # default Nx supersampling; a [fonts.<size>]
//                              # entry's supersample key overrides it per size
//   ascii_original = true      # draw printable ASCII (0x20-0x7E: space,
//                              # 0-9, letters, / , . % etc.) with the original
//                              # .fon bitmap instead of the substitute GDI face
//
//   [fonts]                    # defaults for every game font size
//   fallback_face = "LiSu"
//   default_bias  = 4          # GDI cell = game size + bias
//
//   [fonts.18]                 # per t_font::size override (not the
//   face = "SimHei"            # get_font request size; docs/PROGRESS.md
//   size = 20                  # §4.2). Seen so far: 8 10 12 13 15 18 20
//   bias = 6                   # 21 23 25 26 28; unseen sizes keep
//   supersample = 1            # working and show up in H4CN.log for
//   ascii_original = false     # later tuning. Each key overrides the global.
#ifndef H4CN_CONFIG_H_
#define H4CN_CONFIG_H_

#include <map>
#include <string>
#include <vector>

namespace h4cn {

// One [fonts.<size>] table. Faces are UTF-8 (that is what toml++ hands out);
// the only consumer converts to UTF-16 for CreateFontW.
struct FontMapping {
  std::string face;  // empty = use the [fonts] fallback_face
  bool has_size = false;
  int size = 0;  // explicit GDI cell height
  bool has_bias = false;
  int bias = 0;  // this size's bias over the game's nominal size
  bool has_supersample = false;
  int supersample = 0;  // overrides [render].supersample for this size only
  bool has_ascii_original = false;
  bool ascii_original =
      false;  // overrides [render].ascii_original for this size
};

struct Config {
  // [general]
  bool log_file = true;
  bool log_debug_view = true;
  bool patch_line_height = true;

  // [render]
  // Default supersampling factor for glyph rasterisation. The glyph is rendered
  // at N x the cell and box-filtered back to 1 x, which recovers sub-pixel
  // stroke positioning that GDI's own grid cannot express at these small sizes.
  // 1 is the raw GDI path and is bit-identical to v3.1.0. A
  // [fonts.<size>].supersample overrides this per game font size.
  int render_supersample = 2;

  // Default "draw printable ASCII (0x20-0x7E) with the original .fon bitmap"
  // flag. The substitute faces are Chinese fonts whose Latin/digit/punctuation
  // glyphs are the weak point, so authentic bitmaps are the default; only GBK
  // and any code the original font lacks go through GDI. A
  // [fonts.<size>].ascii_original overrides this per game font size.
  bool render_ascii_original = true;

  // [fonts] defaults
  std::string fallback_face = "LiSu";
  int default_bias = 4;

  // [fonts.<size>] keyed by the game's nominal size.
  std::map<int, FontMapping> per_size;

  // The GDI recipe for one game font: which face to substitute, how tall to
  // render it, and the supersample factor to rasterise its glyphs at. Priority
  // for the cell: size > bias > default_bias. Priority for supersample: the
  // per-size override > [render].supersample. `game_size` is the nominal
  // t_font::size (> 0).
  void Resolve(int game_size, std::string* face, int* render_size,
               int* supersample) const;

  // Whether printable ASCII in this size's font should use the original .fon
  // bitmap. Priority: [fonts.<size>].ascii_original > [render].ascii_original.
  bool AsciiOriginal(int game_size) const;
};

// Parse TOML text into a Config. Defaults are always applied; a key whose
// value is of the wrong type or out of range is skipped and described in
// *problems (each entry is one loggable line). Never throws on content
// problems; a syntax error is reported the same way and yields all defaults.
Config ParseConfig(const std::string& toml_text,
                   std::vector<std::string>* problems);

// Read H4CN.toml from the .asi's directory once, parse it, install the result
// for GetConfig() and log the outcome. Subsequent calls are no-ops. Safe to
// call from any game thread; must not be called from DllMain.
void EnsureConfigLoaded();

// The loaded configuration. Valid only after EnsureConfigLoaded() returned.
const Config& GetConfig();

}  // namespace h4cn

#endif  // H4CN_CONFIG_H_
