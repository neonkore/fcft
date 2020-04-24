fcft_clone(3) "2.0.0" "fcft"

# NAME

fcft_clone - copy an already instantiated font

# SYNOPSIS

*\#include <fcft/fcft.h>*

*struct fcft_font \*fcft_clone(const struct fcft_font \**_font_*);*


# DESCRIPTION

*fcft_clone*() creates a new instance by copying _font_. fcft
internally uses reference counting, making this operation very fast
(much faster than explicitly instantiating a new font using
*fcft_from_name*(3)).

To free all resources associated with the both the original font, and
the cloned font, *fcft_destroy*(3) must be called for *both* the
original font, *and* the clone.

# RETURN VALUE

On success, *fcft_clone*() returns a pointer to a new *fcft_font*
object. On error, NULL is returned.

# SEE ALSO

*fcft_from_name*(3), *fcft_destroy*(3)