fcft_size_adjust(3) "2.2.3" "fcft"

# NAME

fcft_size_adjust - increase or decrease a font's point size

# SYNOPSIS

*\#include <fcft/fcft.h>*

*struct fcft_font \*fcft_size_adjust(
	const struct fcft_font \**_font_*, double *_amount_*);*

# DESCRIPTION

*fcft_size_adjust*() returns a clone of _font_, with its point size
adjusted by _amount_ points. _amount_ can be either positive or
negative.

# RETURN VALUE

On success, *fcft_size_adjust*() returns a new font object. On error,
NULL is returned.

# SEE ALSO

*fcft_from_name*(3), *fcft_clone*(3), *fcft_destroy*(3)
