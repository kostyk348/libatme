#include "atm_engine.h"
#include <stdio.h>
#include <string.h>

/* Путь к .pfb больше не прибит к машине автора: аргумент командной строки,
   переменная LIBATME_PFB или пропуск теста (шрифта в репозитории нет). */
#include <stdlib.h>

int main(int argc, char **argv) {
    const char *font_path = (argc > 1) ? argv[1] : getenv("LIBATME_PFB");
    if (!font_path || !*font_path) {
        printf("test_render: путь к .pfb не задан (аргумент или LIBATME_PFB) — пропуск\n");
        return 0;
    }

    atm_engine_t engine;
    atm_config_t config = {
        .flags = ATM_FLAG_ANTIALIAS | ATM_FLAG_CACHE_ENABLED,
        .antialias_level = 2,
        .cache_size_kb = 256,
        .gamma = 1.8f,
    };
    atm_engine_init(&engine, &config);

    char path[512];
    snprintf(path, sizeof(path), "%s", font_path);

    int fid = atm_engine_load_font(&engine, path, NULL);
    if (fid < 0) {
        printf("FAIL: could not load font\n");
        atm_engine_shutdown(&engine);
        return 1;
    }

    atm_font_info_t info;
    if (atm_engine_get_font_info(&engine, fid, &info) == 0) {
        printf("Font: %s (%s)\n", info.family_name, info.postscript_name);
        printf("  Glyphs: %d, Subrs: %d\n",
               info.num_glyphs,
               engine.fonts[0]->num_subrs);
        printf("  BBox: %d %d %d %d\n",
               info.font_bbox.xMin, info.font_bbox.yMin,
               info.font_bbox.xMax, info.font_bbox.yMax);
    }

    /* Try rendering glyph 'A' (65) at various sizes */
    for (int pt = 12; pt <= 72; pt *= 2) {
        atm_glyph_bitmap_t bm;
        int ret = atm_engine_render_glyph(&engine, fid, 'A',
                                          ATM_INT_TO_FIXED(pt), &bm);
        if (ret == 0) {
            printf("  %2dpt: %dx%d, advance=%d, offset=(%d,%d)\n",
                   pt, bm.width, bm.height,
                   bm.advance_x, bm.x_offset, bm.y_offset);
            atm_bitmap_free(&bm);
        } else {
            printf("  %2dpt: FAIL (charstring decode error)\n", pt);
        }
    }

    /* Test some glyphs */
    const char *test = "ABCabc123";
    int total = 0;
    for (const char *p = test; *p; p++) {
        atm_glyph_bitmap_t bm;
        if (atm_engine_render_glyph(&engine, fid, *p,
                                    ATM_INT_TO_FIXED(48), &bm) == 0) {
            total++;
            atm_bitmap_free(&bm);
        }
    }
    printf("  Rendered %d/%d test glyphs\n", total, (int)strlen(test));

    atm_engine_print_stats(&engine);
    atm_engine_shutdown(&engine);
    return 0;
}
