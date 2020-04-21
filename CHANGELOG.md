# Changelog

* [Unreleased](#unreleased)


## Unreleased
### Added
### Changed

* API: `font_` prefix changed to `fcft_`.
* API: renamed `struct font` to `struct fcft_font`.
* API: renamed `struct glyph` to `struct fcft_glyph`.
* API: internal members of `struct fcft_glyph` removed.
* API: renamed `enum subpixel_order` to `enum fcft_subpixel`, and
  `ORDER` was removed from the enum values.


### Deprecated
### Removed
### Fixed

* LCD RGB/BGR modes where reversed

### Security
