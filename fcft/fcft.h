#pragma once

#include <stdbool.h>
#include <wchar.h>

#include <pixman.h>

struct fcft_font {
    /* font extents */
    int height;
    int descent;
    int ascent;

    /* Width/height of font's widest glyph */
    struct {
        int x;
        int y;
    } max_advance;

    /* Width/height of space (0x20), if available, -1 otherwise */
    struct {
        int x;
        int y;
    } space_advance;

    struct {
        int position;
        int thickness;
    } underline;

    struct {
        int position;
        int thickness;
    } strikeout;
};

/* Bitmask of optional capabilities */
enum fcft_capabilities {
    FCFT_CAPABILITY_GRAPHEME_SHAPING = 0x1,
};

enum fcft_capabilities fcft_capabilities(void);

/* First entry is the main/primary font, the remaining (if any) are
 * custom fallback fonts */
struct fcft_font *fcft_from_name(
    size_t count, const char *names[static count], const char *attributes);
struct fcft_font *fcft_clone(const struct fcft_font *font);
void fcft_destroy(struct fcft_font *font);

/* Returns a *new* font instance */
struct fcft_font *fcft_size_adjust(const struct fcft_font *font, double amount) __attribute__((deprecated));

/*
 * Defines the subpixel order to use.
 *
 * Note that this is *ignored* if antialiasing has been disabled.
 */
enum fcft_subpixel {
    FCFT_SUBPIXEL_DEFAULT,          /* Use subpixel order from FontConfig */
    FCFT_SUBPIXEL_NONE,             /* Disable subpixel antialiasing (use grayscale antialiasing) */
    FCFT_SUBPIXEL_HORIZONTAL_RGB,
    FCFT_SUBPIXEL_HORIZONTAL_BGR,
    FCFT_SUBPIXEL_VERTICAL_RGB,
    FCFT_SUBPIXEL_VERTICAL_BGR,
};

struct fcft_glyph {
    wchar_t wc;
    int cols;              /* wcwidth(wc) */

    pixman_image_t *pix;

    int x;
    int y;
    int width;
    int height;

    struct {
        int x;
        int y;
    } advance;
};

/* Rasterize 'wc' using 'font'. Use the defined subpixel mode *if*
 * antialiasing is enabled for this font */
const struct fcft_glyph *fcft_glyph_rasterize(
    struct fcft_font *font, wchar_t wc, enum fcft_subpixel subpixel);

struct fcft_grapheme {
    int cols;  /* wcswidth(grapheme) */

    size_t count;
    const struct fcft_glyph **glyphs;
};

struct fcft_layout_tag {
    char tag[4];
    unsigned value;
};

const struct fcft_grapheme *fcft_grapheme_rasterize(
    struct fcft_font *font,
    size_t len, const wchar_t grapheme_cluster[static len],
    size_t tag_count, const struct fcft_layout_tag *tags,
    enum fcft_subpixel subpixel);

struct fcft_text_run {
    const struct fcft_glyph **glyphs;
    int *cluster;
    size_t count;
};

struct fcft_text_run *fcft_text_run_rasterize(
    struct fcft_font *font, size_t len, const wchar_t text[static len],
    enum fcft_subpixel subpixel);

void fcft_text_run_destroy(struct fcft_text_run *run);

bool fcft_kerning(
    struct fcft_font *font, wchar_t left, wchar_t right,
    long *restrict x, long *restrict y);

wchar_t fcft_precompose(const struct fcft_font *font,
                                    wchar_t base, wchar_t comb,
                                    bool *base_is_from_primary,
                                    bool *comb_is_from_primary,
                                    bool *composed_is_from_primary);

enum fcft_scaling_filter {
    FCFT_SCALING_FILTER_NONE,
    FCFT_SCALING_FILTER_NEAREST,
    FCFT_SCALING_FILTER_BILINEAR,
    FCFT_SCALING_FILTER_CUBIC,
    FCFT_SCALING_FILTER_LANCZOS3,
};

bool fcft_set_scaling_filter(enum fcft_scaling_filter filter);

/*
 * Logging
 *
 * By default, fcft does not log anything at all. This can be changed
 * by calling fcft_log_init() (typically at program start).
 *
 * Note that fcft_log_init() does *not* call openlog(3), even when
 * do_syslog==true.
 */
enum fcft_log_colorize {
    FCFT_LOG_COLORIZE_NEVER,
    FCFT_LOG_COLORIZE_ALWAYS,
    FCFT_LOG_COLORIZE_AUTO
};

/* Which log messages to show. If you enable e.g. FCFT_LOG_CLASS_INFO,
 * then WARNINGs and ERRORs will also be shown. */
enum fcft_log_class {
    FCFT_LOG_CLASS_NONE,
    FCFT_LOG_CLASS_ERROR,
    FCFT_LOG_CLASS_WARNING,
    FCFT_LOG_CLASS_INFO,
    FCFT_LOG_CLASS_DEBUG
};

void fcft_log_init(
    enum fcft_log_colorize colorize, bool do_syslog,
    enum fcft_log_class log_level);
