#pragma once

#include <stdbool.h>
#include <wchar.h>

#include <pixman.h>

struct glyph {
    wchar_t wc;
    int cols;

    pixman_image_t *pix;
    int x;
    int y;
    int x_advance;
    int width;
    int height;

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

/* First entry is the main/primary font, the remaining (if any) are custom fallback fonts */
struct font *font_from_name(const char *names[], size_t count, const char *attributes);
struct font *font_clone(const struct font *font);
const struct glyph *font_glyph_for_wc(struct font *font, wchar_t wc);
void font_destroy(struct font *font);

bool font_kerning(struct font *font, wchar_t left, wchar_t right, long *x, long *y);