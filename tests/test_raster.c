/*
 * libatme — синтетический тест растеризатора (без файла шрифта).
 *
 * Контуры (квадрат, круг) строятся прямо в памяти в формате atm_glyph_outline_t,
 * поэтому тест проходит на любой машине и в CI: раньше единственный тест требовал
 * .pfb по жёстко прописанному пути и не мог пройти нигде, кроме машины автора.
 *
 * Что проверяется:
 *   • площадь закраски квадрата и круга совпадает с геометрией (допуск 2–4 %);
 *   • удвоение размера даёт вчетверо большую площадь;
 *   • без антиалиасинга (ss=1) в битмапе только 0/255, с ss=4 — промежуточные значения;
 *   • LUT гаммы монотонна и фиксирует края (0→0, 255→255);
 *   • кэш отдаёт промах, затем попадание, hit rate растёт.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atm_cache.h"
#include "atm_gamma.h"
#include "atm_outline.h"
#include "atm_raster.h"
#include "atm_types.h"

static int failures;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            printf("  FAIL  %s  (%s:%d)\n", (msg), __FILE__, __LINE__);         \
            failures++;                                                         \
        }                                                                       \
    } while (0)

typedef struct {
    atm_glyph_outline_t outline;
    atm_path_elem_t storage[32];
} outline_builder_t;

static void ob_reset(outline_builder_t *b)
{
    memset(b, 0, sizeof *b);
    b->outline.elements = b->storage;
    b->outline.capacity = (int)(sizeof b->storage / sizeof b->storage[0]);
    b->outline.num_elements = 0;
}

static void ob_push(outline_builder_t *b, atm_path_cmd_t cmd,
                    double x, double y,
                    double cx1, double cy1, double cx2, double cy2)
{
    if (b->outline.num_elements >= b->outline.capacity) return;
    atm_path_elem_t *e = &b->outline.elements[b->outline.num_elements++];
    e->cmd = cmd;
    e->x = ATM_FLOAT_TO_FIXED(x);
    e->y = ATM_FLOAT_TO_FIXED(y);
    e->cx1 = ATM_FLOAT_TO_FIXED(cx1);
    e->cy1 = ATM_FLOAT_TO_FIXED(cy1);
    e->cx2 = ATM_FLOAT_TO_FIXED(cx2);
    e->cy2 = ATM_FLOAT_TO_FIXED(cy2);
}

static void ob_finalize_bbox(outline_builder_t *b)
{
    atm_glyph_outline_t *o = &b->outline;
    if (o->num_elements == 0) return;
    double xmin = 1e9, ymin = 1e9, xmax = -1e9, ymax = -1e9;
    for (int i = 0; i < o->num_elements; i++) {
        const atm_path_elem_t *e = &o->elements[i];
        double xs[3] = {ATM_FIXED_TO_FLOAT(e->x), ATM_FIXED_TO_FLOAT(e->cx1), ATM_FIXED_TO_FLOAT(e->cx2)};
        double ys[3] = {ATM_FIXED_TO_FLOAT(e->y), ATM_FIXED_TO_FLOAT(e->cy1), ATM_FIXED_TO_FLOAT(e->cy2)};
        for (int k = 0; k < 3; k++) {
            if (xs[k] < xmin) xmin = xs[k];
            if (xs[k] > xmax) xmax = xs[k];
            if (ys[k] < ymin) ymin = ys[k];
            if (ys[k] > ymax) ymax = ys[k];
        }
    }
    o->bbox.xMin = ATM_FLOAT_TO_FIXED(xmin);
    o->bbox.yMin = ATM_FLOAT_TO_FIXED(ymin);
    o->bbox.xMax = ATM_FLOAT_TO_FIXED(xmax);
    o->bbox.yMax = ATM_FLOAT_TO_FIXED(ymax);
}

static void build_square(outline_builder_t *b, double x0, double y0, double side)
{
    ob_reset(b);
    ob_push(b, ATM_MOVETO, x0, y0, 0, 0, 0, 0);
    ob_push(b, ATM_LINETO, x0 + side, y0, 0, 0, 0, 0);
    ob_push(b, ATM_LINETO, x0 + side, y0 + side, 0, 0, 0, 0);
    ob_push(b, ATM_LINETO, x0, y0 + side, 0, 0, 0, 0);
    ob_push(b, ATM_CLOSEPATH, x0, y0, 0, 0, 0, 0);
    ob_finalize_bbox(b);
}

static void build_disc(outline_builder_t *b, double cx, double cy, double r)
{
    const double k = 0.5522847498 * r;
    ob_reset(b);
    ob_push(b, ATM_MOVETO, cx + r, cy, 0, 0, 0, 0);
    ob_push(b, ATM_CURVETO, cx, cy + r, cx + r, cy + k, cx + k, cy + r);
    ob_push(b, ATM_CURVETO, cx - r, cy, cx - k, cy + r, cx - r, cy + k);
    ob_push(b, ATM_CURVETO, cx, cy - r, cx - r, cy - k, cx - k, cy - r);
    ob_push(b, ATM_CURVETO, cx + r, cy, cx + k, cy - r, cx + r, cy - k);
    ob_push(b, ATM_CLOSEPATH, cx + r, cy, 0, 0, 0, 0);
    ob_finalize_bbox(b);
}

static double coverage_pixels(const atm_glyph_bitmap_t *bm)
{
    double sum = 0;
    for (int y = 0; y < bm->height; y++) {
        const uint8_t *row = bm->buf + (size_t)y * (size_t)bm->pitch;
        for (int x = 0; x < bm->width; x++) sum += (double)row[x] / 255.0;
    }
    return sum;
}

static void test_square_area(void)
{
    outline_builder_t b;
    build_square(&b, 0, 0, 40.0);

    atm_raster_config_t cfg = {4, 0};
    atm_glyph_bitmap_t bm;
    memset(&bm, 0, sizeof bm);
    CHECK(atm_bitmap_alloc(&bm, 40, 40) == 0, "выделение битмапа 40×40");
    CHECK(atm_rasterize(&b.outline, &cfg, &bm) == 0, "растеризация квадрата");

    double area = coverage_pixels(&bm);
    printf("  квадрат 40x40: покрытие %.1f пикселей\n", area);
    CHECK(area > 1560.0 && area < 1640.0, "площадь квадрата 40×40 ≈ 1600");
    atm_bitmap_free(&bm);
}

static void test_square_double_scale(void)
{
    outline_builder_t small, big;
    build_square(&small, 0, 0, 20.0);
    build_square(&big, 0, 0, 40.0);

    atm_raster_config_t cfg = {4, 0};
    atm_glyph_bitmap_t bm_small, bm_big;
    memset(&bm_small, 0, sizeof bm_small);
    memset(&bm_big, 0, sizeof bm_big);
    atm_bitmap_alloc(&bm_small, 20, 20);
    atm_bitmap_alloc(&bm_big, 40, 40);
    CHECK(atm_rasterize(&small.outline, &cfg, &bm_small) == 0, "растеризация 20×20");
    CHECK(atm_rasterize(&big.outline, &cfg, &bm_big) == 0, "растеризация 40×40");

    double a_small = coverage_pixels(&bm_small);
    double a_big = coverage_pixels(&bm_big);
    double ratio = (a_small > 1.0) ? a_big / a_small : 0.0;
    CHECK(ratio > 3.7 && ratio < 4.3, "площади относятся как 4:1");

    atm_bitmap_free(&bm_small);
    atm_bitmap_free(&bm_big);
}

static void test_disc_area(void)
{
    outline_builder_t b;
    const double r = 25.0;
    build_disc(&b, 30.0, 30.0, r);

    atm_raster_config_t cfg = {4, 0};
    atm_glyph_bitmap_t bm;
    memset(&bm, 0, sizeof bm);
    atm_bitmap_alloc(&bm, 60, 60);
    CHECK(atm_rasterize(&b.outline, &cfg, &bm) == 0, "растеризация круга");

    double area = coverage_pixels(&bm);
    double expected = 3.14159265 * r * r;
    CHECK(area > expected * 0.96 && area < expected * 1.04, "площадь круга ≈ πr²");
    atm_bitmap_free(&bm);
}

static void test_antialiasing_levels(void)
{
    outline_builder_t b;
    build_square(&b, 0, 0, 20.37);

    atm_raster_config_t cfg_off = {1, 0};
    atm_raster_config_t cfg_on = {4, 0};
    atm_glyph_bitmap_t bm_off, bm_on;
    memset(&bm_off, 0, sizeof bm_off);
    memset(&bm_on, 0, sizeof bm_on);
    atm_bitmap_alloc(&bm_off, 24, 24);
    atm_bitmap_alloc(&bm_on, 24, 24);
    atm_rasterize(&b.outline, &cfg_off, &bm_off);
    atm_rasterize(&b.outline, &cfg_on, &bm_on);

    int intermediate_off = 0, intermediate_on = 0, solid_on = 0;
    for (int y = 0; y < bm_off.height; y++)
        for (int x = 0; x < bm_off.width; x++) {
            uint8_t v = bm_off.buf[y * bm_off.pitch + x];
            if (v > 0 && v < 255) intermediate_off++;
        }
    for (int y = 0; y < bm_on.height; y++)
        for (int x = 0; x < bm_on.width; x++) {
            uint8_t v = bm_on.buf[y * bm_on.pitch + x];
            if (v > 0 && v < 255) intermediate_on++;
            if (v == 255) solid_on++;
        }

    CHECK(intermediate_off == 0, "без AA только 0/255");
    CHECK(intermediate_on > 0, "с AA есть промежуточные значения");
    CHECK(solid_on > 0, "с AA есть полностью закрашенные пиксели");

    atm_bitmap_free(&bm_off);
    atm_bitmap_free(&bm_on);
}

static void test_gamma_monotonic(void)
{
    atm_gamma_t lut;
    atm_gamma_init(&lut, 1.8f);

    int prev = -1, monotonic = 1;
    for (int v = 0; v < 256; v++) {
        int c = atm_gamma_correct(&lut, (uint8_t)v);
        if (c < prev) monotonic = 0;
        prev = c;
    }
    CHECK(monotonic, "LUT гаммы не убывает");
    CHECK(atm_gamma_correct(&lut, 0) == 0, "гамма(0) = 0");
    CHECK(atm_gamma_correct(&lut, 255) == 255, "гамма(255) = 255");
}

static void test_cache(void)
{
    atm_cache_t cache;
    atm_cache_init(&cache, 64); /* 64 КБ */

    CHECK(atm_cache_lookup(&cache, 1, 65, ATM_INT_TO_FIXED(12)) == NULL, "первый запрос — промах");

    atm_glyph_bitmap_t bm;
    memset(&bm, 0, sizeof bm);
    CHECK(atm_bitmap_alloc(&bm, 16, 16) == 0, "битмап для кэша");

    atm_cache_entry_t *ins = atm_cache_insert(&cache, 1, 65, ATM_INT_TO_FIXED(12), &bm);
    CHECK(ins != NULL, "вставка в кэш");
    CHECK(atm_cache_lookup(&cache, 1, 65, ATM_INT_TO_FIXED(12)) != NULL, "второй запрос — попадание");
    CHECK(atm_cache_hit_rate(&cache) > 0, "hit rate растёт");

    atm_bitmap_free(&bm);
    atm_cache_destroy(&cache);
}

int main(void)
{
    printf("libatme raster tests (синтетические контуры, без файла шрифта)\n");

    test_square_area();
    test_square_double_scale();
    test_disc_area();
    test_antialiasing_levels();
    test_gamma_monotonic();
    test_cache();

    if (failures == 0) {
        printf("OK: все проверки пройдены\n");
        return 0;
    }
    printf("ПРОВАЛЕНО проверок: %d\n", failures);
    return 1;
}
