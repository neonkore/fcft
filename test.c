#include <stdlib.h>
#include <check.h>
#include <fcft/fcft.h>

static struct fcft_font *font = NULL;

static void
setup(void)
{
    font = fcft_from_name(1, (const char *[]){"Serif"}, NULL);
    ck_assert_ptr_nonnull(font);
}

static void
teardown(void)
{
    fcft_destroy(font);
    font = NULL;
}

START_TEST(test_capabilities)
{
    enum fcft_capabilities caps = fcft_capabilities();

#if defined(FCFT_HAVE_HARFBUZZ)
    ck_assert(caps & FCFT_CAPABILITY_GRAPHEME_SHAPING);
    caps &= ~FCFT_CAPABILITY_GRAPHEME_SHAPING;
#endif

    ck_assert_int_eq(caps, 0);
}
END_TEST

START_TEST(test_from_name)
{
    ck_assert_int_gt(font->height, 0);
    ck_assert_int_gt(font->max_advance.x, 0);
    ck_assert_int_gt(font->underline.thickness, 0);
    ck_assert_int_gt(font->strikeout.thickness, 0);
}
END_TEST

START_TEST(test_glyph_rasterize)
{
    const struct fcft_glyph *glyph = fcft_glyph_rasterize(
        font, L'A', FCFT_SUBPIXEL_NONE);
    ck_assert_ptr_nonnull(glyph);
    ck_assert_ptr_nonnull(glyph->pix);
    ck_assert_int_eq(glyph->wc, L'A');
    ck_assert_int_eq(glyph->cols, 1);
    ck_assert_int_gt(glyph->width, 0);
    ck_assert_int_gt(glyph->height, 0);
    ck_assert_int_gt(glyph->advance.x, 0);
}
END_TEST

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
START_TEST(test_size_adjust)
{
    struct fcft_font *larger = fcft_size_adjust(font, 50.0);
    ck_assert_ptr_nonnull(larger);
    ck_assert_int_gt(larger->height, font->height);
    ck_assert_int_gt(larger->max_advance.x, font->max_advance.x);
    fcft_destroy(larger);
}
END_TEST
#pragma GCC diagnostic pop

START_TEST(test_precompose)
{
    wchar_t ret = fcft_precompose(font, L'a', L'\U00000301', NULL, NULL, NULL);

    /* All western fonts _should_ have this pre-composed character */
    ck_assert_int_eq(ret, L'á');

    /* Can't verify *_is_from_primary since we don't know which font
     * we're using */

    ret = fcft_precompose(font, L'X', L'Y', NULL, NULL, NULL);
    ck_assert_int_eq(ret, (wchar_t)-1);
}
END_TEST

START_TEST(test_set_scaling_filter)
{
    ck_assert(fcft_set_scaling_filter(FCFT_SCALING_FILTER_NONE));
    ck_assert(fcft_set_scaling_filter(FCFT_SCALING_FILTER_NEAREST));
    ck_assert(fcft_set_scaling_filter(FCFT_SCALING_FILTER_BILINEAR));
    ck_assert(fcft_set_scaling_filter(FCFT_SCALING_FILTER_CUBIC));
    ck_assert(fcft_set_scaling_filter(FCFT_SCALING_FILTER_LANCZOS3));

    ck_assert(!fcft_set_scaling_filter(FCFT_SCALING_FILTER_LANCZOS3 + 120));
}
END_TEST

#if defined(FCFT_HAVE_HARFBUZZ)
START_TEST(test_emoji_zwj)
{
    const wchar_t *const emoji = L"🤚🏿";
    const struct fcft_grapheme *grapheme = fcft_grapheme_rasterize(
        font, wcslen(emoji), emoji, FCFT_SUBPIXEL_DEFAULT);
    ck_assert_ptr_nonnull(grapheme);
    ck_assert_int_eq(grapheme->count, 1);

    /* Verify grapheme was cached */
    const struct fcft_grapheme *grapheme2 = fcft_grapheme_rasterize(
        font, wcslen(emoji), emoji, FCFT_SUBPIXEL_DEFAULT);
    ck_assert_ptr_eq(grapheme, grapheme2);
}
END_TEST
#endif

Suite *
fcft_suite(void)
{
    Suite *suite = suite_create("fcft");

    TCase *core = tcase_create("core");
    tcase_add_checked_fixture(core, &setup, &teardown);
    tcase_add_test(core, test_capabilities);
    tcase_add_test(core, test_from_name);
    tcase_add_test(core, test_glyph_rasterize);
    tcase_add_test(core, test_size_adjust);
    tcase_add_test(core, test_precompose);
    tcase_add_test(core, test_set_scaling_filter);

#if defined(FCFT_HAVE_HARFBUZZ)
    tcase_add_test(core, test_emoji_zwj);
#endif

    /* Slow systems, like the Pinebook Pro, with a *lot* of fonts, *will* be slow */
    tcase_set_timeout(core, 60);

    suite_add_tcase(suite, core);
    return suite;
}

int
main(int argc, const char *const *argv)
{
    Suite *suite = fcft_suite();
    SRunner *runner = srunner_create(suite);

    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);

    srunner_free(runner);
    return failed;
}
