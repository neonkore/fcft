fcft_text_run_rasterize(3) "2.4.1" "fcft"

# NAME

fcft_text_run_rasterize - rasterize a series of glyphs for a text string

# SYNOPSIS

*\#include <fcft/fcft.h>*

*struct fcft_text_run \*fcft_text_run_rasterize(*
	*struct fcft_font \**_font_*, size_t *_len_*,*
	*const wchar_t *_text_*[static len], enum fcft_subpixel *_subpixel_*);*

# DESCRIPTION

*fcft_text_run_rasterize*() shapes and rasterizes the string _text_
into a series of glyphs using the primary font, and possibly one or
more fallback fonts, in _font_.

_subpixel_ allows you to specify which subpixel mode to use. See
*fcft_glyph_rasterize*() for details.

The string is passed to HarfBuzz for shaping, initially using the
primary font. The shaping data is parsed, and when an unshaped
codepoint is encountered, it is retried with the first fallback
font. If that fails too, the next fallback font is tried. After
successfully shaping the failed codepoint, the primary font is used to
shape the rest of the string. This continues until the entire string
has been shaped.

There is limited support for right-to-left
scripts. *hb_buffer_guess_segment_properties*() is used to guess
whether to use left-to-right, or right-to-left, logic, and this is
done each time we try to shape a part of the string.

As a consequence, the result of mixed LTR and RTL strings depends on
the font; if the font has glyphs for all characters in the string,
*hb_buffer_guess_segment_properties*() is only called once, on the
entire string. Meaning the entire string is subject to either LTR, or
RTL logic, but not both.

If the (primary) font does not have glyphs for all characters, then
*hb_buffer_guess_segment_properties*() will be called for each segment
where shaping failed. In this case, parts of the string may be treated
as LTR while others are treated as RTL.

# RETURN VALUE

On error, NULL is returned.

On success, a pointer to a dynamically allocated text-run is returned:

```
struct fcft_text_run {
    const struct fcft_glyph **glyphs;
    int *cluster;
    size_t count;
};
```

Both _glyphs_ and _cluster_ are arrays with _count_
elements. _cluster_ contains the character offsets (in the original
string) of each corresponding glyph.

Note that the glyphs' _cols_ member are invalid.

The text-run is not cached in fcft. The calling application may cache
it for as long as it likes, including after the font has been
destroyed.

The text-run must be free:d with *fcft_text_run_destroy*().

# SEE ALSO

*fcft_text_run_destroy*(), *fcft_grapheme_rasterize*(),
*fcft_glyph_rasterize*()
