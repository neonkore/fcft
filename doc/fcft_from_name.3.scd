fcft_from_name(3) "2.0.0" "fcft"

# NAME

fcft_from_name - instantiate a new font

# SYNOPSIS

*\#include <fcft/fcft.h>*

*struct fcft_font \*fcft_from_name(
	size_t* _count_*, const char \**_names_*[static* _count_*],
	const char \**_attributes_*);*

# DESCRIPTION

*fcft_from_name*() instantiates a new fcft font object from the
FontConfig formatted font names. The first element in _names_ is the
primary font, and the remaining elements (if any) are fallback fonts.

You *must* supply at least one font name.

All aspects of the font (size, DPI, variant etc) are configured
through the font _name_, using colon separated *attribute=value* pairs
(e.g. *"Serif:size=26:slant=italic"*).

_attributes_ is a convenient way to apply a set of attributes to all
fonts in _names_. _attributes_ may be NULL, in which case no extra
attributes are appended to the strings in _names_.

The primary font will be instantiated immediately, and any failure to
do so will result in an error. Fallback fonts are instantiated on
demand, and any failure to do so will result in the that fallback font
being ignored, and the next one in the list is tried instead.

# RETURN VALUE

On success, *fcft_from_name*() returns a pointer to an allocated
*fcft_font* object. On error, NULL is returned.

# EXAMPLE

In this example, we instantiate _Times New Roman_ at a point size of 8
as the primary font.

We also tell it to use _Serif Bold_ (point size 10) as a fallback font
(note that it is usually better to let FontConfig handle fallback to
generic fonts like this).

Furthermore, both fonts will be _Italic_, and will be using DPI=140.


```
#include <stdlib.h>
#include <fcft/fcft.h>

int
main(void)
{
    struct fcft_font *font = fcft_from_name(
        2,
        (const char *[]){
            "Times New Roman:size=8",
            "Serif:size=10:weight=bold",
        },
        "slant=italic:dpi=140");

    if (font == NULL)
        return EXIT_FAILURE;

    fcft_destroy(font);
    return EXIT_SUCCESS;
}
```

# SEE ALSO

*fcft_clone*(3), *fcft_destroy*(3), *fcft_glyph_rasterize*(3),
*fcft_kerning*(3), *fcft_size_adjust*(3)