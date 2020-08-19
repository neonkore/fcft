#pragma once

#include <stdbool.h>
#include <wchar.h>

#include <pixman.h>

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

enum fcft_scaling_filter {
    FCFT_SCALING_FILTER_NONE,
    FCFT_SCALING_FILTER_NEAREST,
    FCFT_SCALING_FILTER_BILINEAR,
    FCFT_SCALING_FILTER_CUBIC,
    FCFT_SCALING_FILTER_LANCZOS3,
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

bool fcft_set_scaling_filter(enum fcft_scaling_filter filter);

/* First entry is the main/primary font, the remaining (if any) are
 * custom fallback fonts */
struct fcft_font *fcft_from_name(
    size_t count, const char *names[static count], const char *attributes);
struct fcft_font *fcft_clone(const struct fcft_font *font);
void fcft_destroy(struct fcft_font *font);

/* Returns a *new* font instance */
struct fcft_font *fcft_size_adjust(const struct fcft_font *font, double amount) __attribute__((deprecated));

/* Rasterize 'wc' using 'font'. Use the defined subpixel mode *if*
 * antialiasing is enabled for this font */
const struct fcft_glyph *fcft_glyph_rasterize(
    struct fcft_font *font, wchar_t wc, enum fcft_subpixel subpixel);

bool fcft_kerning(
    struct fcft_font *font, wchar_t left, wchar_t right,
    long *restrict x, long *restrict y);

wchar_t fcft_precompose(const struct fcft_font *font,
                        wchar_t base, wchar_t comb,
                        bool *base_is_from_primary,
                        bool *comb_is_from_primary,
                        bool *composed_is_from_primary);

const struct fcft_glyph **fcft_glyph_rasterize_grapheme(
    struct fcft_font *font, const wchar_t *grapheme, size_t len,
    enum fcft_subpixel subpixel,
    unsigned *count);
