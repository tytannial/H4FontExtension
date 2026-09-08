# H4FontExtension

Chinese / CJK text support for **Heroes of Might & Magic IV** (2003, Complete).

`H4CN.asi` teaches the game's text engine to display, measure, and line-wrap
extended-encoding characters — Chinese first, and the design covers other CJK
languages that rely on multi-byte encodings such as GBK.

## Why this exists

Heroes IV was built around a single-byte text model. Every string is measured,
broken into lines, and drawn one byte at a time, so Chinese text — two bytes
per character in GBK — comes out as mojibake: split glyphs, wrong widths, and
lines that break in the middle of a character. No font substitution or
language setting can fix that; the text pipeline itself is the problem.

This plugin replaces that pipeline. It installs itself into the game's font
routines at runtime and swaps them for encoding-aware equivalents, so Chinese
and mixed Chinese/English text renders correctly everywhere the game draws
text — with no modification of the game files.

## What it does for you

- Chinese menus and UI at every font size on screen simultaneously
- Mission briefings, story text, and tooltips that wrap by character, not by byte
- Mixed Chinese + English text laid out side by side, correctly
- Combat labels, scrollable text windows, and popups
- Crisp anti-aliased glyphs with proper spacing and line height
- A safety check on startup: if your `heroes4.exe` is not the supported build,
  the plugin steps aside and the game runs vanilla instead of misbehaving

## Usage

The build product is a standard 32-bit Windows DLL, shipped with the `.asi`
suffix by convention. Load it into `heroes4.exe` with whatever method you
prefer — a plugin/ASI loader, manual injection, or otherwise.

## Configuration (optional)

Drop an `H4CN.toml` next to the `.asi` (e.g. `plugins\H4CN.toml`) to choose
the substitute font and cell size the game's bitmap fonts map to. Every
setting is optional and defaults to the built-in behaviour (`LiSu`, nominal
size + 4 px); a broken or unknown value only produces a line in
`plugins\H4CN.log` and falls back to its default — never a crash.

See [`H4CN.example.toml`](H4CN.example.toml) for the full annotated schema:
`[general]` channel switches, a global `[render].supersample` glyph-AA factor,
`[fonts]` defaults, and per-size `[fonts.<size>] face/size/bias/supersample`
overrides keyed by the game's nominal font size (the `H4CN.log` lists the
sizes it actually observed each run).

## Compatibility and known limitations

- Written against one specific build of Heroes IV Complete (2003); other
  exe revisions are detected and refused rather than patched.
- A few layout quirks remain in corner cases, such as some popups sizing
  themselves without measuring the text.

## Disclaimer

This is an unofficial, fan-made compatibility plugin. It is not affiliated
with or endorsed by Ubisoft or New World Computing. The repository contains
no game assets, no game code, and no data files of any kind.

## License

Copyright (c) 2026 Tytannial

This project is licensed under the **MIT License with a Non-Commercial
Restriction** — see [LICENSE](LICENSE).

You are free to use, study, modify, and share this software for
**non-commercial** purposes. **Any commercial use or distribution without the
copyright holder's prior written permission is prohibited.**
