#include "fcft/fcft.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <wchar.h>
#include <math.h>
#include <assert.h>
#include <threads.h>
#include <locale.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_LCD_FILTER_H
#include FT_TRUETYPE_TABLES_H
#include FT_SYNTHESIS_H
#include <fontconfig/fontconfig.h>

#include <tllist.h>

#define LOG_MODULE "fcft"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "fcft/stride.h"

#include "unicode-compose-table.h"

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

static FT_Library ft_lib;
static mtx_t ft_lock;

struct glyph_priv {
    struct fcft_glyph public;

    enum fcft_subpixel subpixel;
    bool valid;
};

/* Per-font glyph cache size */
static const size_t glyph_cache_size = 512;
typedef tll(struct glyph_priv) hash_entry_t;

struct font_fallback {
    char *pattern;
    struct font_priv *font;
};

struct font_priv {
    struct fcft_font public;

    char *name;
    char *pattern;

    mtx_t lock;
    FT_Face face;
    int load_flags;

    bool antialias;
    bool embolden;
    int render_flags_normal;
    int render_flags_subpixel;

    FT_LcdFilter lcd_filter;

    double pixel_size_fixup; /* Scale factor - should only be used with ARGB32 glyphs */
    bool bgr;  /* True for FC_RGBA_BGR and FC_RGBA_VBGR */

    bool is_fallback;
    tll(struct font_fallback) fallbacks;

    size_t ref_counter;

    /* Fields below are only valid for non-fallback fonts */
    FcPattern *fc_pattern;
    FcFontSet *fc_fonts;
    FcCharSet *charset;
    int fc_idx;
    struct font_priv **fc_loaded_fallbacks; /* fc_fonts->nfont array */

    hash_entry_t **glyph_cache;
};

/* Global font cache */
struct fcft_font_cache_entry {
    uint64_t hash;
    struct font_priv *font;

    int waiters;
    cnd_t cond;
};
static tll(struct fcft_font_cache_entry) font_cache = tll_init();
static mtx_t font_cache_lock;

static void __attribute__((constructor))
init(void)
{
    FcInit();
    FT_Init_FreeType(&ft_lib);
    mtx_init(&ft_lock, mtx_plain);
    mtx_init(&font_cache_lock, mtx_plain);

}

static void __attribute__((destructor))
fini(void)
{
    while (tll_length(font_cache) > 0)
        fcft_destroy(&tll_front(font_cache).font->public);

    mtx_destroy(&font_cache_lock);
    mtx_destroy(&ft_lock);
    FT_Done_FreeType(ft_lib);
    FcFini();
}

static void
log_version_information(void)
{
    static bool has_already_logged = false;

    mtx_lock(&font_cache_lock);
    if (has_already_logged) {
        mtx_unlock(&font_cache_lock);
        return;
    }
    has_already_logged = true;
    mtx_unlock(&font_cache_lock);

    {
        int raw_version = FcGetVersion();

        /* See FC_VERSION in <fontconfig/fontconfig.h> */
        const int major = raw_version / 10000; raw_version %= 10000;
        const int minor = raw_version / 100; raw_version %= 100;
        const int patch = raw_version;

        LOG_INFO("fontconfig: %d.%d.%d", major, minor, patch);
    }

    {
        int major, minor, patch;
        FT_Library_Version(ft_lib, &major, &minor, &patch);
        LOG_INFO("freetype: %d.%d.%d", major, minor, patch);
    }

}

static const char *
ft_error_string(FT_Error err)
{
    #undef FTERRORS_H_
    #undef __FTERRORS_H__
    #define FT_ERRORDEF( e, v, s )  case e: return s;
    #define FT_ERROR_START_LIST     switch (err) {
    #define FT_ERROR_END_LIST       }
    #include FT_ERRORS_H
    return "unknown error";
}

static void
underline_strikeout_metrics(struct font_priv *font)
{
    struct fcft_font *pub = &font->public;

    FT_Face ft_face = font->face;
    double y_scale = ft_face->size->metrics.y_scale / 65536.;
    double ascent = ft_face->size->metrics.ascender / 64.;
    double descent = ft_face->size->metrics.descender / 64.;

    double underline_position = ft_face->underline_position * y_scale / 64.;
    double underline_thickness = ft_face->underline_thickness * y_scale / 64.;

    if (underline_position == 0.) {
        LOG_DBG("estimating underline position and thickness");
        underline_thickness = fabs(descent / 5.);

        //underline_position = descent / 2.;           /* Alacritty's algorithm */
        underline_position = -2 * underline_thickness; /* My own */
    }

    /*
     * Position refers to the line's center, thus we need to take the
     * thickness into account to determine the line top.
     *
     * Since a *negative* number means the position is *under* the
     * baseline, we need to *add* half the width to adjust the
     * position "upwards".
     *
     * When rounding the thickness, take care not go below 1.0 as that
     * would make it invisible.
     */
    pub->underline.position = trunc(underline_position + underline_thickness / 2.);
    pub->underline.thickness = round(max(1., underline_thickness));

    LOG_DBG("underline: pos=%f, thick=%f -> pos=%f, pos=%d, thick=%d",
            underline_position, underline_thickness,
            underline_position + underline_thickness / 2.,
            pub->underline.position, pub->underline.thickness);

    double strikeout_position = 0., strikeout_thickness = 0.;
    TT_OS2 *os2 = FT_Get_Sfnt_Table(ft_face, ft_sfnt_os2);
    if (os2 != NULL) {
        strikeout_position = os2->yStrikeoutPosition * y_scale / 64.;
        strikeout_thickness = os2->yStrikeoutSize * y_scale / 64.;
    }

    if (strikeout_position == 0.) {
        LOG_DBG("estimating strikeout position and thickness");
        strikeout_thickness = underline_thickness;

        //strikeout_position = height / 2. + descent;                     /* Alacritty's algorithm */
        strikeout_position = 3. * ascent / 8. - underline_thickness / 2.; /* xterm's algorithm */
    }

    pub->strikeout.position = trunc(strikeout_position + strikeout_thickness / 2.);
    pub->strikeout.thickness = round(max(1., strikeout_thickness));

    LOG_DBG("strikeout: pos=%f, thick=%f -> pos=%f, pos=%d, thick=%d",
            strikeout_position, strikeout_thickness,
            strikeout_position + strikeout_thickness / 2.,
            pub->strikeout.position, pub->strikeout.thickness);
}

static bool
from_font_set(FcPattern *pattern, FcFontSet *fonts, int font_idx,
              struct font_priv *font, bool is_fallback)
{
    memset(font, 0, sizeof(*font));

    FcPattern *final_pattern = FcFontRenderPrepare(
        NULL, pattern, fonts->fonts[font_idx]);
    assert(final_pattern != NULL);

    FcChar8 *face_file = NULL;
    if (FcPatternGetString(final_pattern, FC_FT_FACE, 0, &face_file) != FcResultMatch &&
        FcPatternGetString(final_pattern, FC_FILE, 0, &face_file) != FcResultMatch)
    {
        LOG_ERR("no face file path in pattern");
        goto err_pattern_destroy;
    }

    FcCharSet *charset = NULL;
    if (FcPatternGetCharSet(final_pattern, FC_CHARSET, 0, &charset) != FcResultMatch) {
        LOG_ERR("%s: failed to get charset", face_file);
        goto err_pattern_destroy;
    }

    double dpi;
    if (FcPatternGetDouble(final_pattern, FC_DPI, 0, &dpi) != FcResultMatch)
        dpi = 75;

    double size;
    if (FcPatternGetDouble(final_pattern, FC_SIZE, 0, &size) != FcResultMatch)
        LOG_WARN("%s: failed to get size", face_file);

    double pixel_size;
    if (FcPatternGetDouble(final_pattern, FC_PIXEL_SIZE, 0, &pixel_size) != FcResultMatch) {
        LOG_ERR("%s: failed to get pixel size", face_file);
        goto err_pattern_destroy;
    }

    int face_index;
    if (FcPatternGetInteger(final_pattern, FC_INDEX, 0, &face_index) != FcResultMatch) {
        LOG_WARN("%s: failed to get face index", face_file);
        face_index = 0;
    }

    mtx_lock(&ft_lock);
    FT_Face ft_face;
    FT_Error ft_err = FT_New_Face(ft_lib, (const char *)face_file, face_index, &ft_face);
    mtx_unlock(&ft_lock);
    if (ft_err != 0) {
        LOG_ERR("%s: failed to create FreeType face; %s",
                face_file, ft_error_string(ft_err));
        goto err_pattern_destroy;
    }

    if ((ft_err = FT_Set_Pixel_Sizes(ft_face, 0, pixel_size)) != 0) {
        LOG_ERR("%s: failed to set character size: %s",
                face_file, ft_error_string(ft_err));
        goto err_ft_face_done;
    }

    FcBool scalable;
    if (FcPatternGetBool(final_pattern, FC_SCALABLE, 0, &scalable) != FcResultMatch)
        scalable = FcTrue;

    FcBool outline;
    if (FcPatternGetBool(final_pattern, FC_OUTLINE, 0, &outline) != FcResultMatch)
        outline = FcTrue;

    double pixel_fixup = 1.;
    if (FcPatternGetDouble(final_pattern, "pixelsizefixupfactor", 0, &pixel_fixup) != FcResultMatch) {
        /*
         * Force a fixup factor on scalable bitmap fonts (typically
         * emoji fonts). The fixup factor is
         *   requested-pixel-size / actual-pixels-size
         */
        if (scalable && !outline) {
            double requested_pixel_size;
            if (FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &requested_pixel_size) != FcResultMatch) {
                /* User didn't specify ":pixelsize=xy" */
                double requested_size;
                if (FcPatternGetDouble(pattern, FC_SIZE, 0, &requested_size) != FcResultMatch) {
                    /* User didn't specify ":size=xy" */
                    requested_size = size;
                }

                requested_pixel_size = size * dpi / 72;
            }

            pixel_fixup = requested_pixel_size / ft_face->size->metrics.y_ppem;
            LOG_DBG("estimated pixel fixup factor to %f (from pixel size: %f)",
                    pixel_fixup, requested_pixel_size);
        } else
            pixel_fixup = 1.;
    }

#if 0
    LOG_DBG("FIXED SIZES: %d", ft_face->num_fixed_sizes);
    for (int i = 0; i < ft_face->num_fixed_sizes; i++)
        LOG_DBG("  #%d: height=%d, y_ppem=%f", i, ft_face->available_sizes[i].height, ft_face->available_sizes[i].y_ppem / 64.);
#endif

    FcBool fc_hinting;
    if (FcPatternGetBool(final_pattern, FC_HINTING,0,  &fc_hinting) != FcResultMatch)
        fc_hinting = FcTrue;

    FcBool fc_antialias;
    if (FcPatternGetBool(final_pattern, FC_ANTIALIAS, 0, &fc_antialias) != FcResultMatch)
        fc_antialias = FcTrue;

    int fc_hintstyle;
    if (FcPatternGetInteger(final_pattern, FC_HINT_STYLE, 0, &fc_hintstyle) != FcResultMatch)
        fc_hintstyle = FC_HINT_SLIGHT;

    int fc_rgba;
    if (FcPatternGetInteger(final_pattern, FC_RGBA, 0, &fc_rgba) != FcResultMatch)
        fc_rgba = FC_RGBA_UNKNOWN;

    int load_flags = FT_LOAD_DEFAULT;
    int load_target = FT_LOAD_TARGET_NORMAL;

    if (!fc_antialias) {
        if (!fc_hinting || fc_hintstyle == FC_HINT_NONE)
            load_flags |= FT_LOAD_NO_HINTING;
        else
            load_target = FT_LOAD_TARGET_MONO;

        load_flags |= FT_LOAD_MONOCHROME;
    }

    else {
        if (!fc_hinting || fc_hintstyle == FC_HINT_NONE)
            load_flags |= FT_LOAD_NO_HINTING;

        else if (fc_hintstyle == FC_HINT_SLIGHT)
            load_target = FT_LOAD_TARGET_LIGHT;

        else if (fc_hintstyle == FC_HINT_MEDIUM)
            ;

        else if (fc_rgba == FC_RGBA_RGB || fc_rgba == FC_RGBA_BGR)
            load_target = FT_LOAD_TARGET_LCD;

        else if (fc_rgba == FC_RGBA_VRGB || fc_rgba == FC_RGBA_VBGR)
            load_target = FT_LOAD_TARGET_LCD_V;
    }

    FcBool fc_embeddedbitmap;
    if (FcPatternGetBool(final_pattern, FC_EMBEDDED_BITMAP, 0, &fc_embeddedbitmap) != FcResultMatch)
        fc_embeddedbitmap = FcTrue;

    if (!fc_embeddedbitmap && outline)
        load_flags |= FT_LOAD_NO_BITMAP;

    FcBool fc_autohint;
    if (FcPatternGetBool(final_pattern, FC_AUTOHINT, 0, &fc_autohint) != FcResultMatch)
        fc_autohint = FcFalse;

    if (fc_autohint)
        load_flags |= FT_LOAD_FORCE_AUTOHINT;

    int render_flags_normal, render_flags_subpixel;
    if (!fc_antialias)
        render_flags_normal = render_flags_subpixel = FT_RENDER_MODE_MONO;

    else {
        if (fc_rgba == FC_RGBA_RGB || fc_rgba == FC_RGBA_BGR) {
            render_flags_subpixel = FT_RENDER_MODE_LCD;
            render_flags_normal = FT_RENDER_MODE_NORMAL;
        }

        else if (fc_rgba == FC_RGBA_VRGB || fc_rgba == FC_RGBA_VBGR) {
            render_flags_subpixel = FT_RENDER_MODE_LCD_V;
            render_flags_normal = FT_RENDER_MODE_NORMAL;
        }

        else
            render_flags_normal = render_flags_subpixel = FT_RENDER_MODE_NORMAL;
    }

    int fc_lcdfilter;
    if (FcPatternGetInteger(final_pattern, FC_LCD_FILTER, 0, &fc_lcdfilter) != FcResultMatch)
        fc_lcdfilter = FC_LCD_DEFAULT;

    switch (fc_lcdfilter) {
    case FC_LCD_NONE:    font->lcd_filter = FT_LCD_FILTER_NONE; break;
    case FC_LCD_DEFAULT: font->lcd_filter = FT_LCD_FILTER_DEFAULT; break;
    case FC_LCD_LIGHT:   font->lcd_filter = FT_LCD_FILTER_LIGHT; break;
    case FC_LCD_LEGACY:  font->lcd_filter = FT_LCD_FILTER_LEGACY; break;
    }

    if (mtx_init(&font->lock, mtx_plain) != thrd_success) {
        LOG_WARN("%s: failed to instantiate mutex", face_file);
        goto err_ft_face_done;
    }

    FcBool fc_embolden;
    if (FcPatternGetBool(final_pattern, FC_EMBOLDEN, 0, &fc_embolden) != FcResultMatch)
        fc_embolden = FcFalse;

    FcMatrix *fc_matrix;
    if (FcPatternGetMatrix(final_pattern, FC_MATRIX, 0, &fc_matrix) == FcResultMatch) {
        FT_Matrix m = {
            .xx = fc_matrix->xx * 0x10000,
            .xy = fc_matrix->xy * 0x10000,
            .yx = fc_matrix->yx * 0x10000,
            .yy = fc_matrix->yy * 0x10000,
        };
        FT_Set_Transform(ft_face, &m, NULL);
    }

    font->name = strdup((char *)face_file);
    FcPatternDestroy(final_pattern);

    font->pattern = NULL;
    font->face = ft_face;
    font->load_flags = load_target | load_flags | FT_LOAD_COLOR;
    font->antialias = fc_antialias;
    font->embolden = fc_embolden;
    font->render_flags_normal = render_flags_normal;
    font->render_flags_subpixel = render_flags_subpixel;
    font->is_fallback = is_fallback;
    font->pixel_size_fixup = pixel_fixup;
    font->bgr = fc_rgba == FC_RGBA_BGR || fc_rgba == FC_RGBA_VBGR;
    font->ref_counter = 1;
    font->fc_idx = font_idx;
    font->charset = charset;

    if (is_fallback) {
        font->fc_pattern = NULL;
        font->fc_fonts = NULL;
        font->fc_loaded_fallbacks = NULL;
        font->glyph_cache = NULL;
    } else {
        font->fc_pattern = !is_fallback ? pattern : NULL;
        font->fc_fonts = !is_fallback ? fonts : NULL;
        font->fc_loaded_fallbacks = calloc(
            fonts->nfont, sizeof(font->fc_loaded_fallbacks[0]));
        font->glyph_cache = calloc(glyph_cache_size, sizeof(font->glyph_cache[0]));
    }

    double max_x_advance = ft_face->size->metrics.max_advance / 64.;
    double max_y_advance = ft_face->size->metrics.height / 64.;
    double height = ft_face->size->metrics.height / 64.;
    double descent = ft_face->size->metrics.descender / 64.;
    double ascent = ft_face->size->metrics.ascender / 64.;

    font->public.height = ceil(height * font->pixel_size_fixup);
    font->public.descent = ceil(-descent * font->pixel_size_fixup);
    font->public.ascent = ceil(ascent * font->pixel_size_fixup);
    font->public.max_advance.x = ceil(max_x_advance * font->pixel_size_fixup);
    font->public.max_advance.y = ceil(max_y_advance * font->pixel_size_fixup);

    /*
     * Some fonts (Noto Sans Mono, for example) provides bad
     * max_advance values for grid-based applications, like terminal
     * emulators.
     *
     * For this reason we also provide the width of a regular space
     * character, to help these applications determine the cell size.
     */
    FT_UInt idx = FT_Get_Char_Index(font->face, L' ');
    if (idx != 0 &&
        (ft_err = FT_Load_Glyph(font->face, idx, font->load_flags)) == 0)
    {
        if (fc_embolden && font->face->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
            FT_GlyphSlot_Embolden(font->face->glyph);

        font->public.space_advance.x = ceil(
            font->face->glyph->advance.x / 64. * font->pixel_size_fixup);
        font->public.space_advance.y = ceil(
            font->face->glyph->advance.y / 64. * font->pixel_size_fixup);
    } else {
        font->public.space_advance.x = -1;
        font->public.space_advance.y = -1;
    }


#if defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG
    LOG_DBG("%s: size=%.2fpt/%.2fpx, dpi=%.2f, fixup-factor: %.4f, "
            "line-height: %dpx, ascent: %dpx, descent: %dpx, x-advance (max/space): %d/%dpx",
            font->name, size, pixel_size, dpi, font->pixel_size_fixup,
            font->public.height, font->public.ascent, font->public.descent,
            font->public.max_advance.x, font->public.space_advance.x);
#else
    LOG_INFO("%s: size=%.2fpt/%dpx, dpi=%d", font->name, size, (int)pixel_size, (int)dpi);
#endif

    underline_strikeout_metrics(font);
    return true;

err_ft_face_done:
    mtx_lock(&ft_lock);
    FT_Done_Face(ft_face);
    mtx_unlock(&ft_lock);

err_pattern_destroy:
    FcPatternDestroy(final_pattern);
    return false;
}

static struct font_priv *
from_name(const char *name, bool is_fallback)
{
    LOG_DBG("instantiating %s%s", name, is_fallback ? " (fallback)" : "");

    /* Fontconfig fails to parse floating point values unless locale
     * is e.g C, or en_US.UTF-8 */
    assert(strcmp(setlocale(LC_NUMERIC, NULL), "C") == 0);
    FcPattern *pattern = FcNameParse((const unsigned char *)name);

    if (pattern == NULL) {
        LOG_ERR("%s: failed to lookup font", name);
        return NULL;
    }

    if (!FcConfigSubstitute(NULL, pattern, FcMatchPattern)) {
        LOG_ERR("%s: failed to do config substitution", name);
        FcPatternDestroy(pattern);
        return NULL;
    }

    FcDefaultSubstitute(pattern);

    FcResult result;
    FcFontSet *fonts = FcFontSort(NULL, pattern, FcTrue, NULL, &result);
    if (result != FcResultMatch) {
        LOG_ERR("%s: failed to match font", name);
        FcPatternDestroy(pattern);
        return NULL;
    }

    struct font_priv *font = malloc(sizeof(*font));

    if (!from_font_set(pattern, fonts, 0, font, is_fallback)) {
        free(font);
        FcFontSetDestroy(fonts);
        FcPatternDestroy(pattern);
        return NULL;
    }

    if (is_fallback) {
        FcFontSetDestroy(fonts);
        FcPatternDestroy(pattern);
    }

    font->pattern = strdup(name);
    return font;
}

static uint64_t
sdbm_hash(const char *s)
{
    uint64_t hash = 0;

    for (; *s != '\0'; s++) {
        int c = *s;
        hash = c + (hash << 6) + (hash << 16) - hash;
    }

    return hash;
}

static uint64_t
font_hash(size_t count, const char *names[static count], const char *attributes)
{
    uint64_t hash = 0;
    for (size_t i = 0; i < count; i++)
        hash ^= sdbm_hash(names[i]);

    if (attributes != NULL)
        hash ^= sdbm_hash(attributes);

    return hash;
}

struct fcft_font *
fcft_from_name(size_t count, const char *names[static count],
               const char *attributes)
{
    log_version_information();
    if (count == 0)
        return false;

    uint64_t hash = font_hash(count, names, attributes);
    struct fcft_font_cache_entry *cache_entry = NULL;

    mtx_lock(&font_cache_lock);
    tll_foreach(font_cache, it) {
        if (it->item.hash != hash)
            continue;

        struct fcft_font_cache_entry *e = &it->item;

        if (e->font != (void *)(uintptr_t)-1) {
            /* Font has already been fully initialized */

            if (e->font != NULL) {
                mtx_lock(&e->font->lock);
                e->font->ref_counter++;
                mtx_unlock(&e->font->lock);
            }

            mtx_unlock(&font_cache_lock);
            return e->font != NULL ? &e->font->public : NULL;
        }

        /*
         * Cache entry is only a reservation - font hasn't yet
         * been initialized.
         */

        /* Let instantiating thread know (yet) another thread
         * wants a reference to it */
        e->waiters++;

        while (e->font == (void *)(uintptr_t)-1)
            cnd_wait(&e->cond, &font_cache_lock);
        mtx_unlock(&font_cache_lock);
        return e->font == NULL ? NULL : &e->font->public;
    }

    /* Pre-allocate entry */
    tll_push_back(
        font_cache,
        ((struct fcft_font_cache_entry){.hash = hash, .font = (void *)(uintptr_t)-1}));

    cache_entry = &tll_back(font_cache);

    if (cnd_init(&cache_entry->cond) != thrd_success) {
        LOG_ERR("%s: failed to instantiate font cache condition variable", names[0]);
        tll_pop_back(font_cache);
        mtx_unlock(&font_cache_lock);
        return false;
    }
    mtx_unlock(&font_cache_lock);

    struct font_priv *font = NULL;

    bool have_attrs = attributes != NULL && strlen(attributes) > 0;
    size_t attr_len = have_attrs ? strlen(attributes) + 1 : 0;

    bool first = true;
    for (size_t i = 0; i < count; i++) {
        const char *base_name = names[i];

        char name[strlen(base_name) + attr_len + 1];
        strcpy(name, base_name);
        if (have_attrs) {
            strcat(name, ":");
            strcat(name, attributes);
        }

        if (first) {
            first = false;

            font = from_name(name, false);
            if (font == NULL)
                break;

            continue;
        }

        assert(font != NULL);
        tll_push_back(
            font->fallbacks, ((struct font_fallback){.pattern = strdup(name)}));
    }

    mtx_lock(&font_cache_lock);
    cache_entry->font = font;
    cache_entry->font->ref_counter += cache_entry->waiters;
    cnd_broadcast(&cache_entry->cond);
    mtx_unlock(&font_cache_lock);

    assert(font == NULL || (void *)&font->public == (void *)font);
    return font != NULL ? &font->public : NULL;
}

struct fcft_font *
fcft_clone(const struct fcft_font *_font)
{
    if (_font == NULL)
        return NULL;

    struct font_priv *font = (struct font_priv *)_font;

    mtx_lock(&font->lock);
    {
        assert(font->ref_counter >= 1);
        font->ref_counter++;
    }
    mtx_unlock(&font->lock);

    return &font->public;
}

static char *
pattern_from_font_with_adjusted_size(const struct font_priv *font, double amount)
{
    /* Get current point size */
    double size;
    if (FcPatternGetDouble(font->fc_pattern, FC_SIZE, 0, &size) != FcResultMatch) {
        LOG_ERR("%s: failed to get size", font->name);
        return NULL;
    }

    /* Adjust it */
    size += amount;

    if (size < 1)
        return NULL;

    /* Get original string pattern, and remove "(pixel)size=" */
    char *pattern = strdup(font->pattern);
    size_t len = strlen(pattern);

    for (size_t i = 0; i < 2; i++) {
        while (true) {
            char *s = strstr(pattern, i == 0 ? ":pixelsize=" : ":size=");

            if (s != NULL) {
                const char *e = strchr(s + 1, ':');

                size_t count = e == NULL ? &pattern[len] - s : e - s;
                memmove(s, s + count, len - (s - pattern) - count);

                len -= count;
                pattern[len] = '\0';
            } else
                break;
        }
    }

    /* Strip trailing ':' */
    if (pattern[len - 1] == ':')
        pattern[--len] = '\0';

    /* Append ":size=" */
    char new_size[32];
    snprintf(new_size, sizeof(new_size), ":size=%.2f", size);
    pattern = realloc(pattern, strlen(pattern) + strlen(new_size) + 1);
    strcat(pattern, new_size);

    LOG_DBG("adjust: amount=%f, pattern \"%s\" -> \"%s\"",
            amount, font->pattern, pattern);
    return pattern;
}

struct fcft_font *
fcft_size_adjust(const struct fcft_font *_font, double amount)
{
    const struct font_priv *font = (const struct font_priv *)_font;
    struct font_priv *mfont = (struct font_priv *)font;
    assert(!font->is_fallback);

    mtx_lock(&mfont->lock);

    char *pattern = pattern_from_font_with_adjusted_size(font, amount);
    if (pattern == NULL) {
        mtx_unlock(&mfont->lock);
        return NULL;
    }

    tll(char *) fallback_patterns = tll_init();

    tll_foreach(font->fallbacks, it) {
        char *pat = NULL;

        if (it->item.font == NULL) {
            struct font_priv *f = from_name(it->item.pattern, false);
            if (f != NULL) {
                pat = pattern_from_font_with_adjusted_size(f, amount);
                fcft_destroy(&f->public);
            }
        } else
            pat = pattern_from_font_with_adjusted_size(it->item.font, amount);

        if (pat != NULL)
            tll_push_back(fallback_patterns, pat);
    }

    /* Done with the old font */
    mtx_unlock(&mfont->lock);

    uint64_t hash = 0;
    hash ^= sdbm_hash(pattern);
    tll_foreach(fallback_patterns, it)
        hash ^= sdbm_hash(it->item);

    struct fcft_font_cache_entry *cache_entry = NULL;

    mtx_lock(&font_cache_lock);
    tll_foreach(font_cache, it) {
        if (it->item.hash != hash)
            continue;

        struct fcft_font_cache_entry *e = &it->item;

        if (e->font != (void *)(uintptr_t)-1) {
            /* Font has already been fully initialized */

            if (e->font != NULL) {
                mtx_lock(&e->font->lock);
                e->font->ref_counter++;
                mtx_unlock(&e->font->lock);
            }

            mtx_unlock(&font_cache_lock);
            return e->font != NULL ? &e->font->public : NULL;
        }

        /*
         * Cache entry is only a reservation - font hasn't yet
         * been initialized.
         */

        /* Let instantiating thread know (yet) another thread
         * wants a reference to it */
        e->waiters++;

        while (e->font == (void *)(uintptr_t)-1)
            cnd_wait(&e->cond, &font_cache_lock);
        mtx_unlock(&font_cache_lock);
        return e->font == NULL ? NULL : &e->font->public;
    }

    /* Pre-allocate entry */
    tll_push_back(
        font_cache,
        ((struct fcft_font_cache_entry){.hash = hash, .font = (void *)(uintptr_t)-1}));

    cache_entry = &tll_back(font_cache);

    if (cnd_init(&cache_entry->cond) != thrd_success) {
        LOG_ERR("%s: failed to instantiate font cache condition variable", font->name);
        tll_pop_back(font_cache);
        mtx_unlock(&font_cache_lock);
        return false;
    }
    mtx_unlock(&font_cache_lock);

    /* Instantiate new font */
    struct font_priv *new_font = from_name(pattern, false);
    free(pattern);

    if (new_font != NULL) {
        /* Fallback patterns */
        tll_foreach(fallback_patterns, it) {
            tll_push_back(
                new_font->fallbacks,
                ((struct font_fallback){.pattern = it->item}));
        }
    }

    tll_free(fallback_patterns);

    mtx_lock(&font_cache_lock);
    cache_entry->font = new_font;
    cache_entry->font->ref_counter += cache_entry->waiters;
    cnd_broadcast(&cache_entry->cond);
    mtx_unlock(&font_cache_lock);

    return new_font != NULL ? &new_font->public : NULL;
}

static size_t
hash_index(wchar_t wc)
{
    return wc % glyph_cache_size;
}

static bool
glyph_for_wchar(const struct font_priv *font, wchar_t wc,
                enum fcft_subpixel subpixel, struct glyph_priv *glyph)
{
    *glyph = (struct glyph_priv){
        .public = {
            .wc = wc,
        },
        .valid = false,
    };

    if (!FcCharSetHasChar(font->charset, wc)) {
        /* No glyph in this font, try fallback fonts */

        tll_foreach(font->fallbacks, it) {
            if (it->item.font == NULL) {
                it->item.font = from_name(it->item.pattern, true);
                if (it->item.font == NULL)
                    continue;
            }

            if (glyph_for_wchar(it->item.font, wc, subpixel, glyph)) {
                LOG_DBG("%C: used fallback: %s", wc, it->item.font->name);
                return true;
            }
        }

        if (font->is_fallback)
            return false;

        /* Try fontconfig fallback fonts */

        assert(font->fc_pattern != NULL);
        assert(font->fc_fonts != NULL);
        assert(font->fc_loaded_fallbacks != NULL);
        assert(font->fc_idx == 0);

        for (int i = 1; i < font->fc_fonts->nfont; i++) {
            if (font->fc_loaded_fallbacks[i] == NULL) {
                /* Load font */
                struct font_priv *fallback = malloc(sizeof(*fallback));
                if (!from_font_set(font->fc_pattern, font->fc_fonts, i,
                                   fallback, true))
                {
                    free(fallback);
                    continue;
                }

                LOG_DBG("loaded new fontconfig fallback font");
                assert(fallback->fc_idx == i);
                font->fc_loaded_fallbacks[i] = fallback;
            }

            assert(font->fc_loaded_fallbacks[i] != NULL);

            if (glyph_for_wchar(font->fc_loaded_fallbacks[i], wc,
                                subpixel, glyph))
            {
                LOG_DBG("%C: used fontconfig fallback: %s",
                        wc, font->fc_loaded_fallbacks[i]->name);
                return true;
            }
        }

        LOG_DBG("%C: no glyph found (in neither the main font, "
                "nor any fallback fonts)", wc);
    }

    FT_Error err;

    FT_UInt idx = FT_Get_Char_Index(font->face, wc);
    if ((err = FT_Load_Glyph(font->face, idx, font->load_flags)) != 0) {
        LOG_ERR("%s: failed to load glyph #%d: %s",
                font->name, idx, ft_error_string(err));
        goto err;
    }

    if (font->embolden && font->face->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
        FT_GlyphSlot_Embolden(font->face->glyph);

    int render_flags;
    bool bgr;

    if (font->antialias) {
        switch (subpixel) {
        case FCFT_SUBPIXEL_NONE:
            render_flags = font->render_flags_normal;
            bgr = false;
            break;

        case FCFT_SUBPIXEL_HORIZONTAL_RGB:
        case FCFT_SUBPIXEL_HORIZONTAL_BGR:
            render_flags = FT_RENDER_MODE_LCD;
            bgr = subpixel == FCFT_SUBPIXEL_HORIZONTAL_BGR;
            break;

        case FCFT_SUBPIXEL_VERTICAL_RGB:
        case FCFT_SUBPIXEL_VERTICAL_BGR:
            render_flags = FT_RENDER_MODE_LCD_V;
            bgr = subpixel == FCFT_SUBPIXEL_VERTICAL_BGR;
            break;

        case FCFT_SUBPIXEL_DEFAULT:
        default:
            render_flags = font->render_flags_subpixel;
            bgr = font->bgr;
            break;
        }
    } else {
        render_flags = font->render_flags_normal;
        bgr = false;
    }

    /* This is disabled in default Freetype libs, and the locking
     * required to handle isn't worth the effort... */
#if 0
    /*
     * LCD filter is per library instance. Thus we need to re-set it
     * every time...
     *
     * Also note that many freetype builds lack this feature
     * (FT_CONFIG_OPTION_SUBPIXEL_RENDERING must be defined, and isn't
     * by default) */
    if ((render_flags & FT_RENDER_MODE_LCD) ||
        (render_flags & FT_RENDER_MODE_LCD_V))
    {
        mtx_lock(&ft_lock);

        FT_Error err = FT_Library_SetLcdFilter(ft_lib, font->lcd_filter);
        if (err != 0 && err != FT_Err_Unimplemented_Feature) {
            LOG_ERR("failed to set LCD filter: %s", ft_error_string(err));
            mtx_unlock(&ft_lock);
            goto err;
        }

        mtx_unlock(&ft_lock);
    }
#endif

    if ((err = FT_Render_Glyph(font->face->glyph, render_flags)) != 0) {
        LOG_ERR("%s: failed to render glyph: %s", font->name, ft_error_string(err));
        goto err;
    }

    if (font->face->glyph->format != FT_GLYPH_FORMAT_BITMAP) {
        LOG_ERR("%s: rasterized glyph is not a bitmap", font->name);
        goto err;
    }

    FT_Bitmap *bitmap = &font->face->glyph->bitmap;
    pixman_format_code_t pix_format;
    int width;
    int rows;

    switch (bitmap->pixel_mode) {
    case FT_PIXEL_MODE_MONO:
        pix_format = PIXMAN_a1;
        width = bitmap->width;
        rows = bitmap->rows;
        break;

    case FT_PIXEL_MODE_GRAY:
        pix_format = PIXMAN_a8;
        width = bitmap->width;
        rows = bitmap->rows;
        break;

    case FT_PIXEL_MODE_LCD:
        pix_format = PIXMAN_x8r8g8b8;
        width = bitmap->width / 3;
        rows = bitmap->rows;
        break;

    case FT_PIXEL_MODE_LCD_V:
        pix_format = PIXMAN_x8r8g8b8;
        width = bitmap->width;
        rows = bitmap->rows / 3;
        break;

    case FT_PIXEL_MODE_BGRA:
        pix_format = PIXMAN_a8r8g8b8;
        width = bitmap->width;
        rows = bitmap->rows;
        break;

    default:
        LOG_ERR("unimplemented: FT pixel mode: %d", bitmap->pixel_mode);
        goto err;
        break;
    }

    int stride = stride_for_format_and_width(pix_format, width);
    assert(stride >= bitmap->pitch);

    uint8_t *data = malloc(rows * stride);

    /* Convert FT bitmap to pixman image */
    switch (bitmap->pixel_mode) {
    case FT_PIXEL_MODE_MONO:
        for (size_t r = 0; r < bitmap->rows; r++) {
            for (size_t c = 0; c < (bitmap->width + 7) / 8; c++) {
                uint8_t v = bitmap->buffer[r * bitmap->pitch + c];
                uint8_t reversed = 0;
                for (size_t i = 0; i < min(8, bitmap->width - c * 8); i++)
                    reversed |= ((v >> (7 - i)) & 1) << i;

                data[r * stride + c] = reversed;
            }
        }
        break;

    case FT_PIXEL_MODE_GRAY:
        for (size_t r = 0; r < bitmap->rows; r++) {
            for (size_t c = 0; c < bitmap->width; c++)
                data[r * stride + c] = bitmap->buffer[r * bitmap->pitch + c];
        }
        break;

    case FT_PIXEL_MODE_BGRA:
        assert(stride == bitmap->pitch);
        memcpy(data, bitmap->buffer, bitmap->rows * bitmap->pitch);
        break;

    case FT_PIXEL_MODE_LCD:
        for (size_t r = 0; r < bitmap->rows; r++) {
            for (size_t c = 0; c < bitmap->width; c += 3) {
                unsigned char _r = bitmap->buffer[r * bitmap->pitch + c + (bgr ? 0 : 2)];
                unsigned char _g = bitmap->buffer[r * bitmap->pitch + c + 1];
                unsigned char _b = bitmap->buffer[r * bitmap->pitch + c + (bgr ? 2 : 0)];

                uint32_t *p = (uint32_t *)&data[r * stride + 4 * (c / 3)];
                *p = _r << 16 | _g << 8 | _b;
            }
        }
        break;

    case FT_PIXEL_MODE_LCD_V:
        for (size_t r = 0; r < bitmap->rows; r += 3) {
            for (size_t c = 0; c < bitmap->width; c++) {
                unsigned char _r = bitmap->buffer[(r + (bgr ? 0 : 2)) * bitmap->pitch + c];
                unsigned char _g = bitmap->buffer[(r + 1) * bitmap->pitch + c];
                unsigned char _b = bitmap->buffer[(r + (bgr ? 2 : 0)) * bitmap->pitch + c];

                uint32_t *p = (uint32_t *)&data[r / 3 * stride + 4 * c];
                *p =  _r << 16 | _g << 8 | _b;
            }
        }
        break;

    default:
        abort();
        break;
    }

    pixman_image_t *pix = pixman_image_create_bits_no_clear(
        pix_format, width, rows, (uint32_t *)data, stride);

    if (pix == NULL) {
        free(data);
        goto err;
    }

    pixman_image_set_component_alpha(
        pix,
        bitmap->pixel_mode == FT_PIXEL_MODE_LCD ||
        bitmap->pixel_mode == FT_PIXEL_MODE_LCD_V);

    if (font->pixel_size_fixup != 1.) {
        struct pixman_f_transform scale;
        pixman_f_transform_init_scale(
            &scale,
            1. / font->pixel_size_fixup,
            1. / font->pixel_size_fixup);

        struct pixman_transform _scale;
        pixman_transform_from_pixman_f_transform(&_scale, &scale);
        pixman_image_set_transform(pix, &_scale);

        pixman_image_set_filter(pix, PIXMAN_FILTER_BEST, NULL, 0);
    }

    int cols = wcwidth(wc);
    if (cols < 0)
        cols = 0;

    *glyph = (struct glyph_priv){
        .public = {
            .wc = wc,
            .cols = cols,
            .pix = pix,
            .x = font->face->glyph->bitmap_left * font->pixel_size_fixup,
            .y = font->face->glyph->bitmap_top * font->pixel_size_fixup,
            .advance = {
                .x = ceil(font->face->glyph->advance.x / 64. * font->pixel_size_fixup),
                .y = ceil(font->face->glyph->advance.y / 64. * font->pixel_size_fixup),
            },
            .width = width / (1. / font->pixel_size_fixup),
            .height = rows / (1. / font->pixel_size_fixup),
        },
        .subpixel = subpixel,
        .valid = true,
    };

    return true;

err:
    assert(!glyph->valid);
    return false;
}

const struct fcft_glyph *
fcft_glyph_rasterize(struct fcft_font *_font, wchar_t wc,
                     enum fcft_subpixel subpixel)
{
    struct font_priv *font = (struct font_priv *)_font;
    mtx_lock(&font->lock);

    assert(font->glyph_cache != NULL);
    size_t hash_idx = hash_index(wc);
    hash_entry_t *hash_entry = font->glyph_cache[hash_idx];

    if (hash_entry != NULL) {
        tll_foreach(*hash_entry, it) {
            if (it->item.public.wc == wc &&
                it->item.subpixel == subpixel)
            {
                mtx_unlock(&font->lock);
                return it->item.valid ? &it->item.public : NULL;
            }
        }
    }

    struct glyph_priv glyph;
    bool got_glyph = glyph_for_wchar(font, wc, subpixel, &glyph);

    if (hash_entry == NULL) {
        hash_entry = calloc(1, sizeof(*hash_entry));

        assert(font->glyph_cache[hash_idx] == NULL);
        font->glyph_cache[hash_idx] = hash_entry;
    }

    assert(hash_entry != NULL);
    tll_push_back(*hash_entry, glyph);

    mtx_unlock(&font->lock);
    return got_glyph ? &tll_back(*hash_entry).public : NULL;
}

void
fcft_destroy(struct fcft_font *_font)
{
    if (_font == NULL)
        return;

    struct font_priv *font = (struct font_priv *)_font;

    bool in_cache = false;
    mtx_lock(&font_cache_lock);
    tll_foreach(font_cache, it) {
        if (it->item.font == font) {

            in_cache = true;

            mtx_lock(&font->lock);
            if (--font->ref_counter > 0) {
                mtx_unlock(&font->lock);
                mtx_unlock(&font_cache_lock);
                return;
            }
            mtx_unlock(&font->lock);

            cnd_destroy(&it->item.cond);
            tll_remove(font_cache, it);
            break;
        }
    };
    mtx_unlock(&font_cache_lock);

    if (!in_cache) {
        mtx_lock(&font->lock);
        if (--font->ref_counter > 0) {
            mtx_unlock(&font->lock);
            return;
        }
        mtx_unlock(&font->lock);
    }


    free(font->name);
    free(font->pattern);

    tll_foreach(font->fallbacks, it) {
        if (it->item.font != NULL)
            fcft_destroy(&it->item.font->public);
        free(it->item.pattern);
    }
    tll_free(font->fallbacks);

    if (font->face != NULL) {
        mtx_lock(&ft_lock);
        FT_Done_Face(font->face);
        mtx_unlock(&ft_lock);
    }

    mtx_destroy(&font->lock);

    if (font->fc_fonts != NULL) {
        assert(font->fc_loaded_fallbacks != NULL);

        for (size_t i = 0; i < font->fc_fonts->nfont; i++)
            if (font->fc_loaded_fallbacks[i] != NULL)
                fcft_destroy(&font->fc_loaded_fallbacks[i]->public);

        free(font->fc_loaded_fallbacks);
    }

    if (font->fc_pattern != NULL)
        FcPatternDestroy(font->fc_pattern);
    if (font->fc_fonts != NULL)
        FcFontSetDestroy(font->fc_fonts);


    for (size_t i = 0; i < glyph_cache_size && font->glyph_cache != NULL; i++) {
        if (font->glyph_cache[i] == NULL)
            continue;

        tll_foreach(*font->glyph_cache[i], it) {
            if (!it->item.valid)
                continue;

            void *image = pixman_image_get_data(it->item.public.pix);
            pixman_image_unref(it->item.public.pix);
            free(image);
        }

        tll_free(*font->glyph_cache[i]);
        free(font->glyph_cache[i]);
    }
    free(font->glyph_cache);
    free(font);
}

bool
fcft_kerning(struct fcft_font *_font, wchar_t left, wchar_t right,
             long *restrict x, long *restrict y)
{
    struct font_priv *font = (struct font_priv *)_font;

    if (x != NULL)
        *x = 0;
    if (y != NULL)
        *y = 0;

    if (!FT_HAS_KERNING(font->face))
        return false;

    FT_UInt left_idx = FT_Get_Char_Index(font->face, left);
    if (left_idx == 0)
        return false;

    FT_UInt right_idx = FT_Get_Char_Index(font->face, right);
    if (right_idx == 0)
        return false;

    FT_Vector kerning;
    FT_Error err = FT_Get_Kerning(
        font->face, left_idx, right_idx, FT_KERNING_DEFAULT, &kerning);

    if (err != 0) {
        LOG_WARN("%s: failed to get kerning for %C -> %C: %s",
                 font->name, left, right, ft_error_string(err));
        return false;
    }

    if (x != NULL)
        *x = kerning.x / 64. * font->pixel_size_fixup;
    if (y != NULL)
        *y = kerning.y / 64. * font->pixel_size_fixup;

    LOG_DBG("%s: kerning: %C -> %C: x=%ld 26.6, y=%ld 26.6",
            font->name, left, right, kerning.x, kerning.y);
    return true;
}

wchar_t
fcft_precompose(const struct fcft_font *_font, wchar_t base, wchar_t comb,
                bool *base_is_from_primary,
                bool *comb_is_from_primary,
                bool *composed_is_from_primary)
{
    static_assert(2 * sizeof(wchar_t) <= sizeof(uint64_t),
                  "two wchars does not fit in an uint64_t");

    const struct font_priv *font = (const struct font_priv *)_font;

    if (font != NULL) {
        if (base_is_from_primary != NULL)
            *base_is_from_primary = FcCharSetHasChar(font->charset, base);
        if (comb_is_from_primary != NULL)
            *comb_is_from_primary = FcCharSetHasChar(font->charset, comb);
    }

    const uint64_t match = (uint64_t)base << 32 | comb;

    ssize_t start = 0;
    ssize_t end = (sizeof(precompose_table) / sizeof(precompose_table[0])) - 1;

    while (start <= end) {
        size_t middle = (start + end) / 2;

        const uint64_t maybe =
            (uint64_t)precompose_table[middle].base << 32 | precompose_table[middle].comb;

        if (maybe < match)
            start = middle + 1;
        else if (maybe > match)
            end = middle - 1;
        else {
            wchar_t composed = precompose_table[middle].replacement;
            if (font != NULL && composed_is_from_primary != NULL) {
                *composed_is_from_primary = FcCharSetHasChar(
                    font->charset, composed);
            }
            return composed;
        }
    }

    if (composed_is_from_primary != NULL)
        *composed_is_from_primary = false;
    return (wchar_t)-1;
}
