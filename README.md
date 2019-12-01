# fcft

**fcft** is a small font loading and glyph rasterization library built
on-top of FontConfig, FreeType2 and pixman.

It can load and cache fonts from a fontconfig-formatted name string,
e.g. `Monospace:size=12`, optionally with user configured fallback fonts.

After a font has been loaded, you can rasterize glyphs. When doing so,
the primary font is first considered. If it does not have the
requested glyph, the user configured fallback fonts (if any) are
considered. If none of the user configured fallback fonts has the
requested glyph, the FontConfig generated list of fallback fonts are
checked.


## Requirements

* fontconfig
* freetype
* pixman
* [tllist](https://codeberg.org/dnkl/tllist), _unless_ built as a subproject


## Features

* Supports all fonts loadable by FreeType2
* Antialiasing
* Subpixel antialiasing
* Color bitmap fonts (_emoji_ fonts)
* Font caching
* Glyph caching


## Not supported

* Subpixel positioning
* Kerning

Remember, this is a _simple_ library, not a full blown layout engine.
