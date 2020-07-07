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
#include FT_MODULE_H
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

static FT_Error ft_lib_err;
static FT_Library ft_lib;
static mtx_t ft_lock;

struct glyph_priv {
    struct fcft_glyph public;

    enum fcft_subpixel subpixel;
    bool valid;
};

struct font_instance {
    char *path;
    FT_Face face;
    int load_flags;

    bool antialias;
    bool embolden;
    int render_flags_normal;
    int render_flags_subpixel;

    FT_LcdFilter lcd_filter;

    double pixel_size_fixup; /* Scale factor - should only be used with ARGB32 glyphs */
    bool bgr;  /* True for FC_RGBA_BGR and FC_RGBA_VBGR */

    struct fcft_font metrics;
};

struct fallback {
    FcPattern *pattern;
    FcCharSet *charset;
    struct font_instance *font;

    /* User-requested size(s) - i.e. sizes from *base* pattern */
    double req_pt_size;
    double req_px_size;
};

struct font_priv {
    /* Must be first */
    struct fcft_font public;

    mtx_t lock;
    struct {
        struct glyph_priv **table;
        size_t size;
        size_t count;
    } cache;

    tll(struct fallback) fallbacks;
    size_t ref_counter;
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

static void __attribute__((constructor))
init(void)
{
    FcInit();
    ft_lib_err = FT_Init_FreeType(&ft_lib);
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

    if (ft_lib_err == FT_Err_Ok)
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

static void
instance_destroy(struct font_instance *inst)
{
    if (inst == NULL)
        return;

    mtx_lock(&ft_lock);
    FT_Done_Face(inst->face);
    mtx_unlock(&ft_lock);

    free(inst->path);
    free(inst);
}

static void
fallback_destroy(struct fallback *fallback)
{
    FcPatternDestroy(fallback->pattern);
    FcCharSetDestroy(fallback->charset);
    instance_destroy(fallback->font);
}

static void
underline_strikeout_metrics(FT_Face ft_face, struct fcft_font *font)
{
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
    font->underline.position = trunc(underline_position + underline_thickness / 2.);
    font->underline.thickness = round(max(1., underline_thickness));

    LOG_DBG("underline: pos=%f, thick=%f -> pos=%f, pos=%d, thick=%d",
            underline_position, underline_thickness,
            underline_position + underline_thickness / 2.,
            font->underline.position, font->underline.thickness);

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

    font->strikeout.position = trunc(strikeout_position + strikeout_thickness / 2.);
    font->strikeout.thickness = round(max(1., strikeout_thickness));

    LOG_DBG("strikeout: pos=%f, thick=%f -> pos=%f, pos=%d, thick=%d",
            strikeout_position, strikeout_thickness,
            strikeout_position + strikeout_thickness / 2.,
            font->strikeout.position, font->strikeout.thickness);
}

static FcPattern *
base_pattern_from_name(const char *name, FcFontSet **set)
{
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
    *set = FcFontSort(NULL, pattern, FcTrue, NULL, &result);
    if (result != FcResultMatch) {
        LOG_ERR("%s: failed to match font", name);
        FcPatternDestroy(pattern);
        return NULL;
    }

    return pattern;
}

static FcPattern *
pattern_from_font_set(FcPattern *base_pattern, FcFontSet *set, size_t idx)
{
    FcPattern *pattern = FcFontRenderPrepare(NULL, base_pattern, set->fonts[idx]);
    if (pattern == NULL) {
        LOG_ERR("failed to prepare 'final' pattern");
        return NULL;
    }

    return pattern;
}

static bool
instantiate_pattern(FcPattern *pattern, double req_pt_size, double req_px_size,
                    struct font_instance *font)
{
    FcChar8 *face_file = NULL;
    if (FcPatternGetString(pattern, FC_FT_FACE, 0, &face_file) != FcResultMatch &&
        FcPatternGetString(pattern, FC_FILE, 0, &face_file) != FcResultMatch)
    {
        LOG_ERR("no face file path in pattern");
        return false;
    }

    double dpi;
    if (FcPatternGetDouble(pattern, FC_DPI, 0, &dpi) != FcResultMatch)
        dpi = 75;

    double size;
    if (FcPatternGetDouble(pattern, FC_SIZE, 0, &size) != FcResultMatch)
        LOG_WARN("%s: failed to get size", face_file);

    double pixel_size;
    if (FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &pixel_size) != FcResultMatch) {
        LOG_ERR("%s: failed to get pixel size", face_file);
        return false;
    }

    int face_index;
    if (FcPatternGetInteger(pattern, FC_INDEX, 0, &face_index) != FcResultMatch) {
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
        return false;
    }

    if ((ft_err = FT_Set_Pixel_Sizes(ft_face, 0, pixel_size)) != 0) {
        LOG_ERR("%s: failed to set character size: %s",
                face_file, ft_error_string(ft_err));
        goto err_done_face;
    }

    FcBool scalable;
    if (FcPatternGetBool(pattern, FC_SCALABLE, 0, &scalable) != FcResultMatch)
        scalable = FcTrue;

    FcBool outline;
    if (FcPatternGetBool(pattern, FC_OUTLINE, 0, &outline) != FcResultMatch)
        outline = FcTrue;

    double pixel_fixup = 1.;
    if (FcPatternGetDouble(pattern, "pixelsizefixupfactor", 0, &pixel_fixup) != FcResultMatch) {
        /*
         * Force a fixup factor on scalable bitmap fonts (typically
         * emoji fonts). The fixup factor is
         *   requested-pixel-size / actual-pixels-size
         */
        if (scalable && !outline) {
            if (req_px_size < 0.)
                req_px_size = req_pt_size * dpi / 72.;

            pixel_fixup = req_px_size / ft_face->size->metrics.y_ppem;
            LOG_DBG("estimated pixel fixup factor to %f (from pixel size: %f)",
                    pixel_fixup, req_px_size);
        } else
            pixel_fixup = 1.;
    }

#if 0
    LOG_DBG("FIXED SIZES: %d", ft_face->num_fixed_sizes);
    for (int i = 0; i < ft_face->num_fixed_sizes; i++)
        LOG_DBG("  #%d: height=%d, y_ppem=%f", i, ft_face->available_sizes[i].height, ft_face->available_sizes[i].y_ppem / 64.);
#endif

    FcBool fc_hinting;
    if (FcPatternGetBool(pattern, FC_HINTING,0,  &fc_hinting) != FcResultMatch)
        fc_hinting = FcTrue;

    FcBool fc_antialias;
    if (FcPatternGetBool(pattern, FC_ANTIALIAS, 0, &fc_antialias) != FcResultMatch)
        fc_antialias = FcTrue;

    int fc_hintstyle;
    if (FcPatternGetInteger(pattern, FC_HINT_STYLE, 0, &fc_hintstyle) != FcResultMatch)
        fc_hintstyle = FC_HINT_SLIGHT;

    int fc_rgba;
    if (FcPatternGetInteger(pattern, FC_RGBA, 0, &fc_rgba) != FcResultMatch)
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
    if (FcPatternGetBool(pattern, FC_EMBEDDED_BITMAP, 0, &fc_embeddedbitmap) != FcResultMatch)
        fc_embeddedbitmap = FcTrue;

    if (!fc_embeddedbitmap && outline)
        load_flags |= FT_LOAD_NO_BITMAP;

    FcBool fc_autohint;
    if (FcPatternGetBool(pattern, FC_AUTOHINT, 0, &fc_autohint) != FcResultMatch)
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
    if (FcPatternGetInteger(pattern, FC_LCD_FILTER, 0, &fc_lcdfilter) != FcResultMatch)
        fc_lcdfilter = FC_LCD_DEFAULT;

    switch (fc_lcdfilter) {
    case FC_LCD_NONE:    font->lcd_filter = FT_LCD_FILTER_NONE; break;
    case FC_LCD_DEFAULT: font->lcd_filter = FT_LCD_FILTER_DEFAULT; break;
    case FC_LCD_LIGHT:   font->lcd_filter = FT_LCD_FILTER_LIGHT; break;
    case FC_LCD_LEGACY:  font->lcd_filter = FT_LCD_FILTER_LEGACY; break;
    }

    FcBool fc_embolden;
    if (FcPatternGetBool(pattern, FC_EMBOLDEN, 0, &fc_embolden) != FcResultMatch)
        fc_embolden = FcFalse;

    FcMatrix *fc_matrix;
    if (FcPatternGetMatrix(pattern, FC_MATRIX, 0, &fc_matrix) == FcResultMatch) {
        FT_Matrix m = {
            .xx = fc_matrix->xx * 0x10000,
            .xy = fc_matrix->xy * 0x10000,
            .yx = fc_matrix->yx * 0x10000,
            .yy = fc_matrix->yy * 0x10000,
        };
        FT_Set_Transform(ft_face, &m, NULL);
    }

    font->path = strdup((char *)face_file);
    font->face = ft_face;
    font->load_flags = load_target | load_flags | FT_LOAD_COLOR;
    font->antialias = fc_antialias;
    font->embolden = fc_embolden;
    font->render_flags_normal = render_flags_normal;
    font->render_flags_subpixel = render_flags_subpixel;
    font->pixel_size_fixup = pixel_fixup;
    font->bgr = fc_rgba == FC_RGBA_BGR || fc_rgba == FC_RGBA_VBGR;

    double max_x_advance = ft_face->size->metrics.max_advance / 64.;
    double max_y_advance = ft_face->size->metrics.height / 64.;
    double height = ft_face->size->metrics.height / 64.;
    double descent = ft_face->size->metrics.descender / 64.;
    double ascent = ft_face->size->metrics.ascender / 64.;

    font->metrics.height = ceil(height * font->pixel_size_fixup);
    font->metrics.descent = ceil(-descent * font->pixel_size_fixup);
    font->metrics.ascent = ceil(ascent * font->pixel_size_fixup);
    font->metrics.max_advance.x = ceil(max_x_advance * font->pixel_size_fixup);
    font->metrics.max_advance.y = ceil(max_y_advance * font->pixel_size_fixup);

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
        (ft_err = FT_Load_Glyph(font->face, idx, font->load_flags | FT_LOAD_BITMAP_METRICS_ONLY)) == 0)
    {
        if (fc_embolden && font->face->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
            FT_GlyphSlot_Embolden(font->face->glyph);

        font->metrics.space_advance.x = ceil(
            font->face->glyph->advance.x / 64. * font->pixel_size_fixup);
        font->metrics.space_advance.y = ceil(
            font->face->glyph->advance.y / 64. * font->pixel_size_fixup);
    } else {
        font->metrics.space_advance.x = -1;
        font->metrics.space_advance.y = -1;
    }

    underline_strikeout_metrics(ft_face, &font->metrics);

#if defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG
    LOG_DBG("%s: size=%.2fpt/%.2fpx, dpi=%.2f, fixup-factor: %.4f, "
            "line-height: %dpx, ascent: %dpx, descent: %dpx, x-advance (max/space): %d/%dpx",
            font->path, size, pixel_size, dpi, font->pixel_size_fixup,
            font->metrics.height, font->metrics.ascent, font->metrics.descent,
            font->metrics.max_advance.x, font->metrics.space_advance.x);
#else
    LOG_INFO("%s: size=%.2fpt/%dpx, dpi=%d", font->path, size, (int)pixel_size, (int)dpi);
#endif

    return true;

err_done_face:
    mtx_lock(&ft_lock);
    FT_Done_Face(ft_face);
    mtx_unlock(&ft_lock);
    return false;
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
    if (ft_lib_err != FT_Err_Ok) {
        LOG_ERR("failed to initialize FreeType: %s", ft_error_string(ft_lib_err));
        return NULL;
    }

    if (count == 0)
        return NULL;

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

    tll(struct fallback) fc_fallbacks = tll_init();

    bool first = true;
    for (size_t i = 0; i < count; i++) {
        const char *base_name = names[i];

        char name[strlen(base_name) + attr_len + 1];
        strcpy(name, base_name);
        if (have_attrs) {
            strcat(name, ":");
            strcat(name, attributes);
        }

        FcFontSet *set;
        FcPattern *base_pattern = base_pattern_from_name(name, &set);
        if (base_pattern == NULL)
            break;

        FcPattern *pattern = pattern_from_font_set(base_pattern, set, 0);
        if (pattern == NULL)
            break;

        FcCharSet *charset;
        if (FcPatternGetCharSet(base_pattern, FC_CHARSET, 0, &charset) != FcResultMatch &&
            FcPatternGetCharSet(pattern, FC_CHARSET, 0, &charset) != FcResultMatch)
        {
            LOG_ERR("%s: failed to get charset", name);
            FcPatternDestroy(pattern);
            FcPatternDestroy(base_pattern);
            FcFontSetDestroy(set);
            break;
        }

        double req_px_size = -1., req_pt_size = -1.;
        FcPatternGetDouble(base_pattern, FC_PIXEL_SIZE, 0, &req_px_size);
        FcPatternGetDouble(base_pattern, FC_SIZE, 0, &req_pt_size);

        if (first) {
            first = false;

            mtx_t lock;
            if (mtx_init(&lock, mtx_plain) != thrd_success) {
                LOG_WARN("%s: failed to instantiate mutex", name);
                FcCharSetDestroy(charset);
                FcPatternDestroy(pattern);
                FcPatternDestroy(base_pattern);
                FcFontSetDestroy(set);
                break;
            }

            struct font_instance *primary = malloc(sizeof(*primary));
            if (!instantiate_pattern(pattern, req_pt_size, req_px_size, primary)) {
                free(primary);
                mtx_destroy(&lock);
                FcCharSetDestroy(charset);
                FcPatternDestroy(pattern);
                FcPatternDestroy(base_pattern);
                FcFontSetDestroy(set);
                break;
            }


            font = calloc(1, sizeof(*font));

            font->ref_counter = 1;
            font->lock = lock;
            font->cache.size = 256;
            font->cache.count = 0;
            font->cache.table = calloc(font->cache.size, sizeof(font->cache.table[0]));
            font->public = primary->metrics;

            tll_push_back(font->fallbacks, ((struct fallback){
                        .pattern = pattern,
                        .charset = FcCharSetCopy(charset),
                        .font = primary,
                        .req_px_size = req_px_size,
                        .req_pt_size = req_pt_size}));

            for (size_t i = 1; i < set->nfont; i++) {
                FcPattern *fallback_pattern = pattern_from_font_set(base_pattern, set, i);
                if (fallback_pattern == NULL)
                    continue;

                FcCharSet *fallback_charset;
                if (FcPatternGetCharSet(base_pattern, FC_CHARSET, 0, &fallback_charset) != FcResultMatch &&
                    FcPatternGetCharSet(fallback_pattern, FC_CHARSET, 0, &fallback_charset) != FcResultMatch)
                {
                    LOG_ERR("%s: failed to get charset", name);
                    FcPatternDestroy(fallback_pattern);
                    continue;
                }

                FcPatternGetDouble(base_pattern, FC_PIXEL_SIZE, 0, &req_px_size);
                FcPatternGetDouble(base_pattern, FC_SIZE, 0, &req_pt_size);

                tll_push_back(fc_fallbacks, ((struct fallback){
                            .pattern = fallback_pattern,
                            .charset = FcCharSetCopy(fallback_charset),
                            .req_px_size = req_px_size,
                            .req_pt_size = req_pt_size}));
            }

        } else {
            assert(font != NULL);
            tll_push_back(font->fallbacks, ((struct fallback){
                        .pattern = pattern,
                        .charset = FcCharSetCopy(charset),
                        .req_px_size = req_px_size,
                        .req_pt_size = req_pt_size}));
        }

        FcFontSetDestroy(set);
        FcPatternDestroy(base_pattern);
    }

    /* Append fontconfig-based fallbacks at the end of our fallback list */
    if (font != NULL) {
        tll_foreach(fc_fallbacks, it)
            tll_push_back(font->fallbacks, it->item);
        tll_free(fc_fallbacks);
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

struct fcft_font *
fcft_size_adjust(const struct fcft_font *_font, double amount)
{
    struct font_priv *new = calloc(1, sizeof(*new));
    mtx_init(&new->lock, mtx_plain);

    new->ref_counter = 1;
    new->cache.size = 256;
    new->cache.count = 0;
    new->cache.table = calloc(new->cache.size, sizeof(new->cache.table[0]));

    struct font_priv *font = (struct font_priv *)_font;
    tll_foreach(font->fallbacks, it) {
        FcPattern *pat = it->item.pattern;
        double size = it->item.req_pt_size;

        if (size < 0. &&
            FcPatternGetDouble(pat, FC_SIZE, 0, &size) != FcResultMatch)
        {
            LOG_WARN("failed to get size");
            goto err;
        }

        size += amount;
        if (size < 1.)
            goto err;

        FcPattern *new_base = FcPatternDuplicate(pat);
        FcPatternRemove(new_base, FC_SIZE, 0);
        FcPatternRemove(new_base, FC_PIXEL_SIZE, 0);
        FcPatternRemove(new_base, FC_WEIGHT, 0);
        FcPatternRemove(new_base, FC_WIDTH, 0);
        FcPatternRemove(new_base, FC_FILE, 0);
        FcPatternRemove(new_base, FC_FT_FACE, 0);

        FcPatternAddDouble(new_base, FC_SIZE, size);

        if (!FcConfigSubstitute(NULL, new_base, FcMatchPattern)) {
            FcPatternDestroy(new_base);
            goto err;
        }
        FcDefaultSubstitute(new_base);

        FcResult res;
        FcPattern *new_pat = FcFontMatch(NULL, new_base, &res);

        tll_push_back(new->fallbacks, ((struct fallback){
                    .pattern = new_pat,
                    .charset = FcCharSetCopy(it->item.charset),
                    .req_px_size = -1.,
                    .req_pt_size = size}));

        FcPatternDestroy(new_base);
    }

    assert(tll_length(new->fallbacks) > 0);
    struct fallback *primary = &tll_front(new->fallbacks);

    struct font_instance *inst = malloc(sizeof(*inst));
    if (!instantiate_pattern(
            primary->pattern, primary->req_pt_size, primary->req_px_size, inst))
    {
        free(inst);
        goto err;
    }
    primary->font = inst;

    new->public = inst->metrics;
    return &new->public;

err:
    fcft_destroy(&new->public);
    return NULL;

}

static bool
glyph_for_wchar(const struct font_instance *inst, wchar_t wc,
                enum fcft_subpixel subpixel, struct glyph_priv *glyph)
{
    glyph->public.wc = wc;
    glyph->valid = false;

    FT_Error err;

    FT_UInt idx = FT_Get_Char_Index(inst->face, wc);
    if ((err = FT_Load_Glyph(inst->face, idx, inst->load_flags)) != 0) {
        LOG_ERR("%s: failed to load glyph #%d: %s",
                inst->path, idx, ft_error_string(err));
        goto err;
    }

    if (inst->embolden && inst->face->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
        FT_GlyphSlot_Embolden(inst->face->glyph);

    int render_flags;
    bool bgr;

    if (inst->antialias) {
        switch (subpixel) {
        case FCFT_SUBPIXEL_NONE:
            render_flags = inst->render_flags_normal;
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
            render_flags = inst->render_flags_subpixel;
            bgr = inst->bgr;
            break;
        }
    } else {
        render_flags = inst->render_flags_normal;
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

        FT_Error err = FT_Library_SetLcdFilter(ft_lib, font->primary.lcd_filter);
        if (err != 0 && err != FT_Err_Unimplemented_Feature) {
            LOG_ERR("failed to set LCD filter: %s", ft_error_string(err));
            mtx_unlock(&ft_lock);
            goto err;
        }

        mtx_unlock(&ft_lock);
    }
#endif

    if ((err = FT_Render_Glyph(inst->face->glyph, render_flags)) != 0) {
        LOG_ERR("%s: failed to render glyph: %s", inst->path, ft_error_string(err));
        goto err;
    }

    if (inst->face->glyph->format != FT_GLYPH_FORMAT_BITMAP) {
        LOG_ERR("%s: rasterized glyph is not a bitmap", inst->path);
        goto err;
    }

    const FT_Bitmap *bitmap = &inst->face->glyph->bitmap;
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
        if (stride == bitmap->pitch) {
            memcpy(data, bitmap->buffer, rows * stride);
        } else {
            for (size_t r = 0; r < bitmap->rows; r++) {
                for (size_t c = 0; c < bitmap->width; c++)
                    data[r * stride + c] = bitmap->buffer[r * bitmap->pitch + c];
            }
        }
        break;

    case FT_PIXEL_MODE_BGRA:
        assert(stride == bitmap->pitch);
        memcpy(data, bitmap->buffer, bitmap->rows * bitmap->pitch);
        break;

    case FT_PIXEL_MODE_LCD:
        for (size_t r = 0; r < bitmap->rows; r++) {
            for (size_t c = 0; c < bitmap->width; c += 3) {
                unsigned char _r = bitmap->buffer[r * bitmap->pitch + c + (bgr ? 2 : 0)];
                unsigned char _g = bitmap->buffer[r * bitmap->pitch + c + 1];
                unsigned char _b = bitmap->buffer[r * bitmap->pitch + c + (bgr ? 0 : 2)];

                uint32_t *p = (uint32_t *)&data[r * stride + 4 * (c / 3)];
                *p = _r << 16 | _g << 8 | _b;
            }
        }
        break;

    case FT_PIXEL_MODE_LCD_V:
        for (size_t r = 0; r < bitmap->rows; r += 3) {
            for (size_t c = 0; c < bitmap->width; c++) {
                unsigned char _r = bitmap->buffer[(r + (bgr ? 2 : 0)) * bitmap->pitch + c];
                unsigned char _g = bitmap->buffer[(r + 1) * bitmap->pitch + c];
                unsigned char _b = bitmap->buffer[(r + (bgr ? 0 : 2)) * bitmap->pitch + c];

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

    if (inst->pixel_size_fixup != 1.) {
        struct pixman_f_transform scale;
        pixman_f_transform_init_scale(
            &scale,
            1. / inst->pixel_size_fixup,
            1. / inst->pixel_size_fixup);

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
            .x = inst->face->glyph->bitmap_left * inst->pixel_size_fixup,
            .y = inst->face->glyph->bitmap_top * inst->pixel_size_fixup,
            .advance = {
                .x = inst->face->glyph->advance.x / 64.,
                .y = inst->face->glyph->advance.y / 64.,
            },
            .width = width / (1. / inst->pixel_size_fixup),
            .height = rows / (1. / inst->pixel_size_fixup),
        },
        .subpixel = subpixel,
        .valid = true,
    };

    return true;

err:
    assert(!glyph->valid);
    return false;
}

static size_t
hash_index_for_size(size_t size, size_t v)
{
    return (v * 2654435761) & (size - 1);
}

static size_t
hash_index(const struct font_priv *font, size_t v)
{
    return hash_index_for_size(font->cache.size, v);
}

static size_t
hash_value_for_wc(wchar_t wc, enum fcft_subpixel subpixel)
{
    return subpixel << 29 | wc;
}

static struct glyph_priv **
glyph_hash_lookup(struct font_priv *font, wchar_t wc,
                  enum fcft_subpixel subpixel)
{
    size_t idx = hash_index(font, hash_value_for_wc(wc, subpixel));
    struct glyph_priv **glyph = &font->cache.table[idx];

    while (*glyph != NULL && !((*glyph)->public.wc == wc &&
                               (*glyph)->subpixel == subpixel))
    {
        idx = (idx + 1) & (font->cache.size - 1);
        glyph = &font->cache.table[idx];
    }

    return glyph;
}

static bool
cache_resize(struct font_priv *font)
{
    if (font->cache.count * 100 / font->cache.size < 75)
        return false;

    size_t size = 2 * font->cache.size;
    assert(__builtin_popcount(size) == 1);

    struct glyph_priv **table = calloc(size, sizeof(table[0]));

    for (size_t i = 0; i < font->cache.size; i++) {
        struct glyph_priv *entry = font->cache.table[i];

        if (entry == NULL)
            continue;

        size_t idx = hash_index_for_size(
            size, hash_value_for_wc(entry->public.wc, entry->subpixel));

        while (table[idx] != NULL) {
            assert(!(table[idx]->public.wc == entry->public.wc &&
                     table[idx]->subpixel != entry->subpixel));
            idx = (idx + 1) & (size - 1);
        }

        assert(table[idx] == NULL);
        table[idx] = entry;
        font->cache.table[i] = NULL;
    }

    free(font->cache.table);

    LOG_DBG("resized glyph cache from %zu to %zu", font->cache.size, size);
    font->cache.table = table;
    font->cache.size = size;
    return true;
}

const struct fcft_glyph *
fcft_glyph_rasterize(struct fcft_font *_font, wchar_t wc,
                     enum fcft_subpixel subpixel)
{
    struct font_priv *font = (struct font_priv *)_font;
    mtx_lock(&font->lock);

    assert(font->cache.table != NULL);

    struct glyph_priv **entry = glyph_hash_lookup(font, wc, subpixel);

    if (*entry != NULL) {
        assert((*entry)->public.wc == wc);
        assert((*entry)->subpixel == subpixel);
        mtx_unlock(&font->lock);
        return (*entry)->valid ? &(*entry)->public : NULL;
    }

    if (cache_resize(font))
        entry = glyph_hash_lookup(font, wc, subpixel);

    struct glyph_priv *glyph = malloc(sizeof(*glyph));
    glyph->public.wc = wc;
    glyph->valid = false;

    assert(tll_length(font->fallbacks) > 0);

    bool noone = true;
    bool got_glyph = false;
    tll_foreach(font->fallbacks, it) {
        if (!FcCharSetHasChar(it->item.charset, wc))
            continue;

        if (it->item.font == NULL) {
            struct font_instance *inst = malloc(sizeof(*inst));
            if (!instantiate_pattern(
                    it->item.pattern,
                    it->item.req_pt_size, it->item.req_px_size,
                    inst))
            {
                /* Remove, so that we don't have to keep trying to
                 * instantiate it */
                free(inst);
                fallback_destroy(&it->item);
                tll_remove(font->fallbacks, it);
                continue;
            }

            it->item.font = inst;
        }

        assert(it->item.font != NULL);
        got_glyph = glyph_for_wchar(it->item.font, wc, subpixel, glyph);
        noone = false;
        break;
    }

    if (noone) {
        /*
         * No font claimed this glyph - use the primary font anyway.
         */
        assert(tll_length(font->fallbacks) > 0);
        struct font_instance *inst = tll_front(font->fallbacks).font;

        assert(inst != NULL);
        got_glyph = glyph_for_wchar(inst, wc, subpixel, glyph);
    }

    assert(*entry == NULL);
    *entry = glyph;
    font->cache.count++;

    mtx_unlock(&font->lock);
    return got_glyph ? &glyph->public : NULL;
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


    tll_foreach(font->fallbacks, it)
        fallback_destroy(&it->item);

    tll_free(font->fallbacks);
    mtx_destroy(&font->lock);

    for (size_t i = 0; i < font->cache.size && font->cache.table != NULL; i++) {
        struct glyph_priv *entry = font->cache.table[i];

        if (entry == NULL)
            continue;

        if (entry->valid) {
            void *image = pixman_image_get_data(entry->public.pix);
            pixman_image_unref(entry->public.pix);
            free(image);
        }

        free(entry);
    }

    free(font->cache.table);
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

    assert(tll_length(font->fallbacks) > 0);
    const struct font_instance *primary = tll_front(font->fallbacks).font;

    if (!FT_HAS_KERNING(primary->face))
        return false;

    FT_UInt left_idx = FT_Get_Char_Index(primary->face, left);
    if (left_idx == 0)
        return false;

    FT_UInt right_idx = FT_Get_Char_Index(primary->face, right);
    if (right_idx == 0)
        return false;

    FT_Vector kerning;
    FT_Error err = FT_Get_Kerning(
        primary->face, left_idx, right_idx, FT_KERNING_DEFAULT, &kerning);

    if (err != 0) {
        LOG_WARN("%s: failed to get kerning for %C -> %C: %s",
                 primary->path, left, right, ft_error_string(err));
        return false;
    }

    if (x != NULL)
        *x = kerning.x / 64. * primary->pixel_size_fixup;
    if (y != NULL)
        *y = kerning.y / 64. * primary->pixel_size_fixup;

    LOG_DBG("%s: kerning: %C -> %C: x=%ld 26.6, y=%ld 26.6",
            primary->path, left, right, kerning.x, kerning.y);
    return true;
}

wchar_t
fcft_precompose(const struct fcft_font *_font, wchar_t base, wchar_t comb,
                bool *base_is_from_primary,
                bool *comb_is_from_primary,
                bool *composed_is_from_primary)
{
    _Static_assert(2 * sizeof(wchar_t) <= sizeof(uint64_t),
                  "two wchars does not fit in an uint64_t");

    const struct font_priv *font = (const struct font_priv *)_font;

    assert(tll_length(font->fallbacks) > 0);
    const struct fallback *primary = &tll_front(font->fallbacks);

    if (font != NULL) {
        if (base_is_from_primary != NULL)
            *base_is_from_primary = FcCharSetHasChar(primary->charset, base);
        if (comb_is_from_primary != NULL)
            *comb_is_from_primary = FcCharSetHasChar(primary->charset, comb);
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
                    primary->charset, composed);
            }
            return composed;
        }
    }

    if (composed_is_from_primary != NULL)
        *composed_is_from_primary = false;
    return (wchar_t)-1;
}
