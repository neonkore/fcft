fcft_capabilities(3) "2.2.7" "fcft"

# NAME

fcft_capabilities - available optional library capabilities

# SYNOPSIS

*\#include <fcft/fcft.h>*

*enum fcft_capabilities fcft_capabilities(*void*);*

# DESCRIPTION

*fcft_capabilities*() returns a bitmask of enabled optional library
capabilities:

```
enum fcft_capabilities {
    FCFT_CAPABILITY_GRAPHEME_SHAPING = 0x1,
};
```

# FCFT_CAPABILITY_GRAPHEME_SHAPING

fcft can do grapheme shaping (using HarfBuzz), and
*fcft_glyph_rasterize_grapheme*() can be expected to return non-NULL.

# SEE ALSO

*fcft_glyph_rasterize_grapheme*()
