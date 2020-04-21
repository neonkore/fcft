#pragma once

#include <stdbool.h>
#include <wchar.h>

#include <pixman.h>

enum subpixel_order {
    FCFT_SUBPIXEL_ORDER_DEFAULT,
    FCFT_SUBPIXEL_ORDER_NONE,
    FCFT_SUBPIXEL_ORDER_HORIZONTAL_RGB,
    FCFT_SUBPIXEL_ORDER_HORIZONTAL_BGR,
    FCFT_SUBPIXEL_ORDER_VERTICAL_RGB,
    FCFT_SUBPIXEL_ORDER_VERTICAL_BGR,
};

struct glyph {
    wchar_t wc;
    int cols;              /* wcwidth(wc) */

    pixman_image_t *pix;
    int x;
    int y;
    int x_advance;
    int width;
    int height;

    /* Internal */
    enum subpixel_order subpixel;
    bool valid;
};

struct font {
    /* font extents */
    int height;
    int descent;
    int ascent;
    int max_x_advance;    /* Width of font's widest glyph */
    int space_x_advance;  /* Width of space (0x20), if available, -1 otherwise */

    struct {
        int position;
        int thickness;
    } underline;

    struct {
        int position;
        int thickness;
    } strikeout;
};

/* First entry is the main/primary font, the remaining (if any) are
 * custom fallback fonts */
struct font *fcft_from_name(
    size_t count, const char *names[static count], const char *attributes);
struct font *fcft_clone(const struct font *font);
void fcft_destroy(struct font *font);

/* Returns a *new* font instance */
struct font *fcft_size_adjust(const struct font *font, double amount);

const struct glyph *fcft_glyph_for_wc(
    struct font *font, wchar_t wc, enum subpixel_order subpixel);

bool fcft_kerning(
    struct font *font, wchar_t left, wchar_t right,
    long *restrict x, long *restrict y);
