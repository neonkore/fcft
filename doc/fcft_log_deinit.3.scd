fcft_log_deinit(3) "2.3.3" "fcft"

# NAME

fcft_log_deinit - fcft logging cleanup

# SYNOPSIS

*\#include <fcft/fcft.h>*

*void fcft_log_deinit(void);*

# DESCRIPTION

Call *fcft_log_deinit*(3) when you are done using fcft, to perform
cleanup of fcft's logging facility. This is only needed if you have
enabled logging with *fcft_log_init*(3).

# SEE ALSO

*fcft_log_init*(3)
