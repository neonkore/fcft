fcft_log_init(3) "2.3.3" "fcft"

# NAME

fcft_log_init - configure logging in fcft

# SYNOPSIS

*\#include <fcft/fcft.h>*

*void fcft_log_init(
	enum fcft_log_colorize* _colorize_*, bool* _do\_syslog_*,
	enum fcft_log_facility* _syslog\_facility_*,
	enum fcft_log_class* _log\_level_*);*

# DESCRIPTION

By default, fcft does not log anything; use *fcft_log_init*(3) to
enable, and configure logging in fcft.

*fcft_log_init*(3) is typically called early, at program startup. If
it is used, *log_deinit*(3) should be called once you are done with
fcft, to cleanup.

_colorize_ is one of:

```
enum fcft_log_colorize {
    FCFT_LOG_COLORIZE_NEVER,
    FCFT_LOG_COLORIZE_ALWAYS,
    FCFT_LOG_COLORIZE_AUTO
};
```

*FCFT_LOG_COLORIZE_AUTO* enables colors if stderr is a TTY.

if _do\_syslog_ is set, fcft will log to syslog, in addition to the
regular logging on stderr.

_syslog\_facility_ sets the facility, and corresponds to the
_facility_ argument in *openlog*(3). Possible values are:

```
enum fcft_log_facility {
    FCFT_LOG_FACILITY_USER,
    FCFT_LOG_FACILITY_DAEMON
};
```

Finally, _log\_level_ configures which types of log messages that will be shown.
```

enum fcft_log_class {
    FCFT_LOG_CLASS_NONE,
    FCFT_LOG_CLASS_ERROR,
    FCFT_LOG_CLASS_WARNING,
    FCFT_LOG_CLASS_INFO,
    FCFT_LOG_CLASS_DEBUG
};
```

Note that this is not a bitmask; setting *FCFT_LOG_CLASS_INFO*, also
enables *FCFT_LOG_CLASS_WARNING* and *FCFT_LOG_CLASS_ERROR*.



# SEE ALSO

*fcft_log_deinit*(3)
