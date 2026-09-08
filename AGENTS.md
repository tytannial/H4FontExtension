# AGENTS.md

Win32 **x86** DLL built as `H4CN.asi`: an ASI plugin that inline-hooks 7 font functions in HoMM IV
`heroes4.exe` (2003 Complete) so text renders / measures / wraps GBK-aware and Chinese displays.
No tests, no CI, no package manager — a clean build is the only automated signal.

Sources are Google C++ style (`.cc`/`.h`, `clang-format --style=file`, 80 columns, `kCamelCase`
constants, `PascalCase` functions, `snake_case` variables).

| File | Role |
|---|---|
| `game_types.h` | game structs, `static_assert`-locked layouts |
| `game_addrs.h` | every absolute address + each hook's expected prologue and length |
| `inline_hook.*` | prologue check, patch, trampoline, restore on detach (all 7 pass `orig=nullptr`) |
| `diagnostics.*` | `OutputDebugStringA` + `plugins\H4CN.log`; startup summary logged every run |
| `config.*` | H4CN.toml (`toml++`): channel switches + `[render].supersample` (global, per-`t_font::size` overridable) + per-`t_font::size` face/cell overrides |
| `font_cache.*` | `FontContext` per (face, size, supersample), advance + pixel caches, font metrics |
| `blit.*` | 4-bit alpha → RGB565 blend + single-line drawing (`DrawLine`) |
| `wrap.*` | line breaking (`WrapText`) via the `Advancer` interface, no game memory — unit-tested |
| `hooks.*` / `main.cc` | the 7 hooks; `DllMain` install/restore |

## Build

```powershell
./build.ps1 -Config Debug            # or -Config Release, -DeployDir '<game dir>\plugins', -Clean
./build.ps1 -Config Debug -Test      # additionally builds + runs the WrapText/Config host tests (ctest)
```

`build.ps1` does the three things that are easy to get wrong: locates VS via `vswhere`, enters the
**x86** developer environment (`Enter-VsDevShell -DevCmdArguments '-arch=x86 -host_arch=x64'`), and
prepends the VS-bundled cmake/ninja to `PATH` — a stray `cmake.exe` resolved first from `PATH`
(e.g. Strawberry Perl's) dies with `0xC000007B` (bad image). It then runs
`cmake --preset x86-<cfg>` / `cmake --build --preset x86-<cfg>` (`CMakePresets.json`, Ninja,
`architecture.strategy: external`). Output: `out/build/x86-<cfg>/H4CN.asi`, always a 32-bit PE —
`heroes4.exe` is 32-bit, and `game_types.h`'s `static_assert` values fail an x64 compile on purpose.

CRT is the default `/MD` (and `/MDd` for Debug): **Release needs the VC++ 2015-2022 x86
redistributable on the player's machine, and the Debug `.asi` cannot load without VS installed —
never ship Debug.**

## Verify (only loop that exists)

Copy `H4CN.asi` into your game folder's **`plugins\`** (`-DeployDir` does it on every build) and run
the game. The loader is Ultimate ASI Loader (`version.dll` proxy); its `version.ini` sets
`LoadPlugins=1` **`LoadFromScriptsOnly=1`**, so it scans **only the `plugins\` subfolder** — the game
root is the wrong place and `Mp3dec.asi` there is *not* a loader plugin, it is the game's own Miles
Sound System MP3 decoder (loaded by `Mss32.dll`). The `.asi` suffix in CMakeLists is for the loader's
scan pattern, nothing to do with `Mp3dec.asi`. Check: multi-size UI on one screen, combat labels
(draw_to_rect), mission briefing / story text (wrap), scrollable text windows, popups, mixed CN+EN
text, number abbreviations. Proof-of-life: `plugins\H4CN.log` gets one summary line per run
(`7/7 font hooks installed`) plus `game size NN -> '<face>' cell K px ssN` per created context.
Optionally place `H4CN.toml` (schema: `H4CN.example.toml`) in `plugins\` — face/cell overrides and
channel switches; a broken file must only log and fall back to defaults.

## Hard constraints

- Every address in `game_addrs.h` is an absolute VA valid for **that one exe build**. Confirm against
  IDA (`heroes4.exe.i64` of the 2003 Complete exe) before adding or changing one.
- A hook's byte length **must end on a real instruction boundary**: the trampoline re-executes a
  verbatim copy of those bytes and then jumps to `addr + len`, so a partial instruction makes it run
  the following jump as operand bytes. The length is pinned together with the prologue in each
  `HookTarget` (0x71BD50→7, 0x71BDE0→6, 0x71BE40→8, 0x71C100/0x71C7F0/0x71C900→7 SEH,
  0x71C5E0→5). Check the prologue in IDA before hooking anything new.
- `InstallInlineHook` compares the prologue bytes and **refuses to patch on mismatch**, so a wrong
  exe shows mojibake instead of corrupting code. Keep that behaviour. All 7 hooks fully replace the
  original and pass `orig == nullptr` (the old `kFontWrapTextMm` delegation to the stock width search
  was replaced by `WrapTextOptimal`); an unused trampoline is only a way to run stale machine code.
- Hook signatures mirror the game's `__thiscall` methods by being `__fastcall` with an unnamed second
  `uint32_t` (EDX) parameter. Do not "clean that up".
- Line breaking lives in exactly one function, `WrapText` in `wrap.cc`, used by `HookFontWrapText`
  (0x71C100), `HookFontDrawToRect` (0x71C7F0), `HookFontGetWrappedHeight` (0x71C900) and, through
  `WrapTextOptimal`, `HookFontWrapTextMm` (0x71C5E0). If those ever disagree, the game's scrollbar
  init gets a zero scroll range → integer divide-by-zero crash. Never re-grow a second copy of the
  wrap logic; build new search behaviour on top of `WrapText`, not beside it.
- Measurement must not rasterize: `FontContext::Advance` (used by get_width / get_column /
  wrapping / height) only reads the advance table, ASCII is primed at context creation, and
  `GetGlyph` is the only path that runs GDI text output. Re-unifying them re-introduces the bug this
  split was made to fix.
- One `FontContext` **per (face, rendered size, supersample)**. The game mixes a dozen-plus sizes
  (`t_font::size` observed: 8/10/12/13/15/18/20/21/23/25/26/28 so far, see docs/PROGRESS.md §4.2).
  A single
  global font + cache keyed by code only is the old design: it destroyed and rebuilt the font and
  emptied the whole cache on every size change.
- Never write into a buffer the game owns. `Line` is a `{ptr, len}` range; the old code temporarily
  NUL-terminated the caller's string, which is unsafe for `.data`/`.rdata` strings and shared buffers.
- The game uses the VC6 STL ABI. `game::Vc6String` / `game::StringVector` mirror it, and results are
  built with the game's own helpers (`0x5ABC70` clear, `0x440F50` insert, `0x401D30` assign,
  `0x401CE0` `_Tidy` — `_Tidy(&s, 0)` is the default-constructor idiom). Never hand a modern MSVC
  `std::string`/`vector` to game memory.
- A game font's identity *for this plugin* is `t_font::size` (+0x04, the only read-only metric, read
  from the `.fon` header): `Config::Resolve` keys H4CN.toml's `[fonts.<size>]` by it to pick the
  substitute face and GDI cell height (default = size + `default_bias` **4**). Do NOT confuse it with
  the `get_font@0x875BC0` *request* sizes that binary-search `g_fonts` — different numbering,
  see docs/PROGRESS.md §4.2.
  `PatchFontMetrics` then writes the *measured* `TEXTMETRIC.tmHeight` into `font->line_height`
  (+0x0C) on every hook entry so unhooked paths (`t_text_window_paint`, `t_text_window_update_layout`)
  use the same line height we draw with — gated by `[general].patch_line_height`. `line_height` is
  destroyed on the first hook call, so never derive the render size from it.
- Glyph pixels are 1 byte per pixel: high nibble = foreground coverage, low nibble = a baked 1px
  down-right shadow, which is what the blitter's two blend passes consume. The ink bounding box is
  computed *after* the shadow is baked, and `BlitGlyph` visits only that box. RGB565 masks are read
  live from game globals `0xAAF0D8..0xAAF104` (falling back to the calibrated constants when unset).
- Glyph rendering is GDI into a DIB section whose `bits` pointer is read directly (no `GetDIBits`);
  `ANTIALIASED_QUALITY`, and `GB2312_CHARSET` only for the default face. The DIB and a second HFONT are
  sized for the size's resolved supersample (`[fonts.<size>].supersample`, else global
  `[render].supersample`, default 2 on this branch): the glyph is drawn at N× and each N×N
  box of per-sub-pixel coverage is averaged back to the 1× cell before it is quantised to the nibble,
  which recovers sub-pixel stroke positioning GDI cannot express on its own grid; `1` is the identity
  and reproduces v3.1.0 bit-for-bit. Only the bitmap is supersampled — `Measure`/advance/line height
  stay on the 1× font (which `Rasterize` restores on the shared DC). The face and cell height come
  from H4CN.toml (`fallback_face` default `LiSu`, per-size `[fonts.<size>]` overrides). GBK bytes are
  converted with codepage 936 explicitly, so rendering does not depend on the system ANSI codepage.
- H4CN.toml is read **lazily on the first hooked call** (`EnsureConfigLoaded` in `config.cc`),
  never in `DllMain`, and a parse/type error degrades to defaults + one log line — never a crash. Only
  `config.cc` includes the single-header `deps/toml.hpp` (one TU).
- Links `gdi32`, `kernel32`, `user32` only (`user32` for `GetDC`/`ReleaseDC`). All installation happens
  in `DllMain(DLL_PROCESS_ATTACH)`; `DLL_PROCESS_DETACH` restores the patched prologues *before*
  releasing GDI and caches. Keep it that way — no loader/injector in this repo, and no heavy work in
  `DllMain`. The one deliberate exception: `diagnostics.*` appends the startup summary to
  `H4CN.log` (a single best-effort `CreateFile`/`WriteFile`/`CloseHandle`, silently dropped on any
  error) so the file is always proof-of-life. Do not turn that into a general logging path that does
  disk I/O per frame.

## Docs

Repo layout: sources and build config in `src/`, the annotated example config
`H4CN.example.toml` at the root, project docs under `docs/`. The debug link map
(`heroes4_debug.map`) is external reference material and is **not shipped with this repo**. Read and
edit docs here only:

- `docs/PROGRESS.md` — the single doc: reverse-engineering calibration (map↔exe version caveats, IDB
  traps, font/text_window/button clusters, un-hooked "system 2" §6) **and** the plugin
  implementation (§7 hook table with prologue bytes + lengths, module map §7.6, build/static-analysis
  commands §10). The old `doc.md` files were merged into it; its § numbers are unchanged. Read it
  before touching any address or struct layout.
- `docs/TODO.md` — every unfinished item with priority, evidence and how to verify. Status/tracking
  lives here, never in `docs/PROGRESS.md`.


## Conventions

- Commit messages: conventional prefixes (`fix:`, `feat:`, `refactor:`, `chore:`, `docs:`), English summary.
- Format before committing: `clang-format -i --style=file *.cc *.h` and check with
  `--dry-run -Werror`. The formatter lives at `<VS>\VC\Tools\Llvm\x64\bin\`.
- `clang-tidy` needs a compilation database (presets export one) **and** `--extra-arg=-m32`,
  otherwise it parses as x64 and every layout `static_assert` in `game_types.h` fails spuriously:
  `clang-tidy -p out/build/x86-debug --extra-arg=-m32 --quiet *.cc`.
- Sources and docs are UTF-8 without BOM; comments are English, docs are Chinese+English mixed. This
  pwsh console is GBK, so `Get-Content`/`Select-String` renders Chinese paths and text as mojibake —
  use the Read/Grep tools instead.
