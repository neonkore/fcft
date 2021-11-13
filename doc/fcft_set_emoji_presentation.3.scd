fcft_set_emoji_presentation(3) "2.5.0" "fcft"

# NAME

fcft_set_emoji_presentation - configures the default emoji presentation

# SYNOPSIS

*\#include <fcft/fcft.h>*

*bool fcft_set_emoji_presentation(struct fcft_font \**_font_*,
	enum fcft_emoji_presentation *_presentation_*);*

# DESCRIPTION

*fcft_set_emoji_presentation*() configures the default presentation
style to use with emojis with both a text presentation, and an emoji
presentation style.

One example of such an emoji is U+263A - WHITE SMILING FACE:

- unqualified: ☺
- text: ☺︎
- emoji: ☺️

These emojis can be followed by an (invisible) variation
selector. When they are not followed by a selector, the implementation
must choose a style.

The Unicode standard defines the default presentation styles, and this
is what fcft defaults to.

However, in some cases, the application may want to always use either
the text style, or the emoji style.

This is what *fcft_set_emoji_presentation*() is for. Possible values
for _presentation_ are:

- *FCFT\_EMOJI\_PRESENTATION\_DEFAULT*
- *FCFT\_EMOJI\_PRESENTATION\_TEXT*
- *FCFT\_EMOJI\_PRESENTATION\_EMOJI*

To re-iterate; this setting affects emoji's without an explicit
variation selector. In other words, it overrides the *default*
presentation style.

*FCFT\_EMOJI\_PRESENTATION\_DEFAULT* is the default, and causes fcft
to use the default presentation as defined by Unicode.

*FCFT\_EMOJI\_PRESENTATION\_TEXT* forces all multi-presentation style
emojis to be rendered in their text presentation style.

*FCFT\_EMOJI\_PRESENTATION\_EMOJI* forces all multi-presentation style
emojis to be rendered in their emoji presentation style.

This function does *not* clear the glyph caches and should therefore be
called before rasterizing any glyphs.

# SEE ALSO

*fcft_glyph_rasterize*(), *fcft_grapheme_rasterize*(),
*fcft_text_run_rasterize*()
