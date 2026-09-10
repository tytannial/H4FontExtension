// GBK glyph rasterisation and measurement, plus the game<->GDI font size
// mapping.
//
// Two caches live here and they are deliberately separate:
//   * advance width - needed by every measuring/wrapping call, cheap to get
//     (one GetTextExtentPoint32W), and primed for ASCII up front;
//   * pixels - only needed when a character is actually drawn.
// The stock plugin rasterised on measurement, so t_font::get_width and the
// wrapping passes paid for TextOut + a full DIB read-back per new character.
#ifndef H4CN_FONT_CACHE_H_
#define H4CN_FONT_CACHE_H_

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "game_types.h"
#include "wrap.h"

namespace h4cn {

// One rasterised character in the encoding the game's own blitter uses: a byte
// per pixel, high nibble = foreground coverage (0..15), low nibble = the 1px
// drop shadow baked in at raster time.
struct Glyph {
  std::vector<uint8_t> pixels;  // cell_w * cell_h, row-major; empty if blank
  int cell_w = 0;               // Width of the rendered cell, in pixels.
  int cell_h = 0;               // Height of the rendered cell, in pixels.
  int advance = 0;              // Pen advance, in pixels.
  int ink_x = 0;                // Bounding box of non-zero bytes, shadow
  int ink_y = 0;                // included, relative to the cell's top left.
  int ink_w = 0;
  int ink_h = 0;
  bool measured = false;    // advance is valid
  bool rasterized = false;  // Rasterize() ran; pixels may still be empty
};

// GDI state for one (face, cell height) pair chosen from H4CN.toml. The game
// mixes a dozen-plus distinct t_font::size values on one screen (see
// docs/PROGRESS.md §4.2), so a context per pair avoids destroying the font and
// throwing the glyph cache away on every switch.
class FontContext : public Advancer {
 public:
  // `face_utf8` is the configured font family name; converted to UTF-16 for
  // CreateFontW. `supersample` is the (already-resolved, per game size) Nx
  // factor this context rasterises at; 1 is the raw v3.1.0 path. Creates the
  // DC, DIB section and font. Check valid() before use: GDI or memory
  // exhaustion leaves the object unusable rather than throwing.
  FontContext(std::string face_utf8, int render_size, int supersample);
  ~FontContext();

  FontContext(const FontContext&) = delete;
  FontContext& operator=(const FontContext&) = delete;

  bool valid() const { return valid_; }

  // The configured family, UTF-8, for diagnostics.
  const std::string& face() const { return face_utf8_; }

  // lfHeight handed to CreateFontW.
  int render_size() const { return render_size_; }

  // Real cell height reported by GetTextMetrics. This is what PatchFontMetrics
  // writes into t_font::line_height and what the hooks advance by, so rendered
  // cells and layout can never disagree.
  int line_height() const { return line_height_; }

  // Advancer: pen advance of `code` in pixels, rasterising nothing.
  int Advance(uint32_t code) override;

  // Glyph for `code`, rasterising on first use. Never null.
  //
  // The pointer stays valid until ShutdownFontCache(); unordered_map keeps
  // element addresses stable across rehash. Do not hold two of them across a
  // call that could clear the cache.
  const Glyph* GetGlyph(uint32_t code);

  // Lock-free variants. The caller must already hold this context's lock (the
  // hooks batch-lock it once per pass and build their GlyphRouter in locked
  // mode). Using these without the lock is a data race; they exist so a
  // whole wrap/draw pass takes the lock once instead of once per character.
  int AdvanceLocked(uint32_t code);
  const Glyph* GetGlyphLocked(uint32_t code);

  // Lockable for `std::lock_guard<FontContext>`: the hooks batch-lock one
  // context for the whole wrap/draw pass, so a 200-character measurement pays
  // one lock instead of 200. Recursive because Advance/GetGlyph keep their
  // own guards for callers that do not batch - a hook holding this lock
  // re-enters them on the same thread. It serialises the glyph caches and the
  // shared GDI DC of *this* context; contexts for other sizes are unaffected.
  // The lowercase names are the std Mutex requirement, not a style slip.
  void lock() { mutex_.lock(); }
  void unlock() { mutex_.unlock(); }
  bool try_lock() { return mutex_.try_lock(); }

 private:
  // All private helpers run with this context's lock already held (Advance /
  // GetGlyph acquire it; the hooks batch-acquire it around a whole pass).
  void Measure(Glyph* glyph, uint32_t code);
  void Rasterize(Glyph* glyph, uint32_t code);

  // Per-context recursive lock guarding glyphs_ and the shared GDI DC.
  // Per-context (not one global lock) so the dozens of live sizes never contend
  // with each other; recursive so a hook that batch-locks can still call
  // Advance/GetGlyph, which keep their own guards.
  std::recursive_mutex mutex_;

  std::string face_utf8_;
  int render_size_ = 0;
  int line_height_ = 0;
  bool valid_ = false;

  // Rasterisation supersample factor (>= 1). 1 means glyphs are drawn straight
  // at render_size_; N means drawn at N * render_size_ and box-filtered back,
  // which gives small CJK strokes sub-pixel positioning GDI cannot express on
  // its own 1x grid. Fixed for the life of the context.
  int supersample_ = 1;

  // GDI handles kept as void* so this header does not pull in Windows.h.
  void* font_ = nullptr;  // HFONT
  void* dc_ = nullptr;    // HDC, font_ and dib_ selected into it
  void* dib_ = nullptr;   // HBITMAP, 32bpp top-down DIB section
  void* font_ss_ =
      nullptr;  // HFONT at supersample_ x, null when supersample_==1
  uint8_t* bits_ = nullptr;  // dib_ pixels, BGRA, dib_stride_ bytes per row
  int dib_w_ = 0;
  int dib_h_ = 0;
  int dib_stride_ = 0;

  std::unordered_map<uint32_t, Glyph> glyphs_;
};

// The context that replaces `font`, creating it on demand.
//
// Ensures the configuration is loaded (first call happens on a game thread,
// never in DllMain), then resolves t_font::size (+0x04) through H4CN.toml:
// the nominal pixel size is the identity of a game bitmap font
// (get_font@0x875BC0 binary-searches g_fonts by it) and picks the face and
// cell height - by default the size plus the configured bias, because the
// substitute GDI face needs a slightly taller cell to fill the same space.
// t_font::line_height (+0x0C) is *not* an input: it is overwritten by
// PatchFontMetrics on the first hook call and would feed the bias back into
// itself.
//
// Returns null when the context could not be created; callers must then skip
// drawing or report a zero width rather than fall back to the stock
// byte-oriented code.
FontContext* GetFontContext(const game::Font& font);

// Whether `font`'s printable ASCII should be drawn from the original .fon
// bitmap rather than the GDI substitute, per H4CN.toml ([render].ascii_original
// with a [fonts.<size>] override). Resolves the game size the same way
// GetFontContext does, so the two always agree on which size is meant. The
// per-code range check happens later in GlyphRouter; this is only the size's
// on/off decision.
bool KeepOriginalAscii(const game::Font& font);

// Rewrites font->line_height so that code paths this plugin does not hook
// (t_text_window_paint, t_text_window_update_layout, ...) lay out with the
// height the glyphs actually occupy. Called on every hook entry.
void PatchFontMetrics(game::Font* font, int line_height);

// Called from DllMain(DLL_PROCESS_ATTACH).
void InitFontCache();

// Releases every context, its glyph pixels and its GDI objects. Called from
// DllMain(DLL_PROCESS_DETACH), after UninstallAllHooks.
void ShutdownFontCache();

}  // namespace h4cn

#endif  // H4CN_FONT_CACHE_H_
