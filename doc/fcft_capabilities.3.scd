fcft_capabilities(3) "3.0.0" "fcft"

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
    FCFT_CAPABILITY_GRAPHEME_SHAPING = 0x1,  /* Since 2.3.0 */
    FCFT_CAPABILITY_TEXT_RUN_SHAPING = 0x2,  /* Since 2.4.0 */
};
```

# FCFT_CAPABILITY_GRAPHEME_SHAPING

fcft can do grapheme shaping (using HarfBuzz), and
*fcft_grapheme_rasterize*() can be expected to return non-NULL.

# FCFT_CAPABILITY_TEXT_RUN_SHAPING

fcft can do text-run shaping (using HarfBuzz), and
*fcft_text_run_rasterize*() can be expected to return non-NULL.

# SEE ALSO

*fcft_grapheme_glyph_rasterize*(), *fcft_text_run_rasterize*()
