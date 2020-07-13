# Changelog

* [2.2.1](#2-2-1)
* [2.2.0](#2-2-0)
* [2.1.3](#2-1-3)
* [2.1.2](#2-1-2)
* [2.1.1](#2-1-1)
* [2.1.0](#2-1-0)
* [2.0.0](#2-0-0)
* [1.1.7](#1-1-7)

## 2.2.1

### Changed

* Color bitmap glyphs with a pixel-size fixup factor other than 1.0
  are now pre-scaled. Previously, fcft would only set a pixman scale
  transform on the glyph, causing actual scaling to occur **every**
  time the glyph was blended. This improves the performance when
  rendering color emojis.


## 2.2.0

### Changed

* Internal representation of the primary and fallback fonts.
* Do not load a fallback font if it does not contain the requested glyph.


### Deprecated

* `fcft_size_adjust()`


## 2.1.3

### Fixed

* Advance width and height of scaled bitmap fonts.


## 2.1.2

### Changed

* Glyph cache now resizes dynamically. This fixes a performance
  problem when loading **a lot** of glyphs, as we ended up scanning
  very long lists when looking up a glyph in the cache.


## 2.1.1

### Changed

* Prefer user-provided `charset`. This can be used to e.g. limit a
  fallback font's usage to a custom Unicode point range.


### Fixed

* LCD RGB/BGR modes were reversed.


## 2.1.0

### Added

* When looking for a glyph in the fallback fonts, don't discard
  (destroy/unload) the fonts that did not contain the glyph. This
  improves performance massively when loading lots of glyphs that does
  not exist in the primary font, or in the first fallback font(s).
* Synthetic bold and italics (FontConfig's _embolden_ and _matrix_
  properties).
* `fcft_precompose()` - combines a base- and a combining wide
  character into a single pre-composed character.


### Changed

* `fcft_from_name()` and `fcft_size_adjust()` no longer calls
  `setlocale()` to set a suitable locale for `FcParseName()`, as this
  was not thread safe. The caller is responsible for ensuring
  `LC_NUMERICAL` is set to a locale that correctly recognizes _x.y_
  decimal values.


### Fixed

* `fcft_from_name()` was not thread safe
* `fcft_clone()` was not thread safe
* `fcft_size_adjust()` was not thread safe
* `fcft_destroy` was not thread safe


## 2.0.0

### Changed

* API: `font_` prefix changed to `fcft_`.
* API: renamed `struct font` to `struct fcft_font`.
* API: renamed `struct glyph` to `struct fcft_glyph`.
* API: internal members of `struct fcft_glyph` removed.
* API: renamed `enum subpixel_order` to `enum fcft_subpixel`, and
  `ORDER` was removed from the enum values.
* API: renamed `fcft_glyph.x_advance` to `fcft_glyph.advance.x`, and
  added `fcft_glyph.advance.y`
* API: renamed `fcft_font.max_x_advance` to `fcft_font.max_advance.x`
  and added `fcft_font.max_advance.y`.
* API: renamed `fcft_font.space_x_advance` to
  `fcft_font.space_advance.x` and added `fcft_font.space_advance.y`.
* API: renamed `fcft_glyph_for_wc()` to `fcft_glyph_rasterize()`.
* Require meson >= 0.54.
* Use `meson.override_dependency()`.


### Fixed

* `fcft_kerning()` did not scale the returned kerning distances with
  the font's pixel size fixup multiplier.


## 1.1.7

### Fixed

* LCD RGB/BGR modes were reversed
