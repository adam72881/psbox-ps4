# UI font assets

Noto Sans Regular and SemiBold are redistributed under the SIL Open Font
License 1.1. The unchanged font files and complete copyright/license text
are retained here. `SOURCE.json` pins the official Noto repository revision,
download locations and SHA-256 values.

`noto_sans.x4font` is an offline alpha atlas, generated from those files by
`scripts/generate_font_assets.py` using Pillow 12.3.0 and its PC-side FreeType
support. The application uses no FreeType/Pillow library and does not
rasterize fonts at runtime. The atlas remains under the font's OFL license.

The sizes are 18, 26, 28, 32, 38, 46 and 56 pixels, each in Regular and
SemiBold. 442 codepoints cover printable ASCII, Latin-1, Latin Extended-A,
combining diacritics, selected typographic punctuation, the ellipsis and
the replacement character. Other scripts use the replacement glyph. This
renderer uses independent glyph advances; complex-script shaping, full
Unicode normalization and pair kerning are not implemented.

The packaged atlas occupies **3,346,263 bytes**. A compiled smooth 32-pixel
Regular fallback occupies **184,531 bytes** so a missing/corrupt asset
still leaves a readable UI. Atlas plus fallback is **3,530,794 bytes**;
the loader rejects inputs exceeding the combined **4 MiB** ceiling.
Text drawing and measuring allocate no memory and perform no file access.
Only init/close load or free the optional atlas, from the UI thread.

Pixels follow the existing framebuffer's RGBA byte order. Exact atlas
sizes use alpha blits. Other sizes use bounded alpha resampling, without
rasterizing a font. The font loader validates lengths, faces, glyph order,
advance limits and every alpha range before accepting a file.

Reproduce offline from the repository root:

```sh
python3 scripts/generate_font_assets.py
```

Package `noto_sans.x4font` and this license/notice with the application.
The TTF files are generator inputs and are not runtime dependencies.
