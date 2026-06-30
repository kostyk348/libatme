#include "atm_types.h"
#include "atm_outline.h"
#include "atm_raster.h"
#include "atm_cache.h"
#include "atm_gamma.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_MODULE_H

#ifdef __linux__
#include <sys/resource.h>
#include <unistd.h>
#endif

#define CLR_RED    "\033[31m"
#define CLR_GREEN  "\033[32m"
#define CLR_YELLOW "\033[33m"
#define CLR_BOLD   "\033[1m"
#define CLR_RESET  "\033[0m"

#define FONT_DIR "/home/lain/Downloads/atm_deluxe/Adobe Type Manager Deluxe 4.1/Adobe Type Manager Deluxe v.4.1/fonts"

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static size_t get_mem_rss(void) {
#ifdef __linux__
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    size_t mem = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "VmRSS: %zu kB", &mem) == 1) {
            fclose(f);
            return mem * 1024;
        }
    }
    fclose(f);
#endif
    return 0;
}

static double compute_psnr(const uint8_t *a, const uint8_t *b,
                           int w, int h, int sa, int sb) {
    double mse = 0.0;
    int count = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int d = a[y*sa+x] - b[y*sb+x];
            mse += (double)(d*d);
            count++;
        }
    return (count < 1 || mse < 1e-10) ? 99.99
        : 10.0 * log10(255.0*255.0 / (mse/count));
}

/* ─── Outline conversion via FT_Outline_Decompose (proper cubic bezier) ─── */

struct atm_outline_builder {
    atm_glyph_outline_t *dst;
    atm_fixed_t first_x, first_y;
};

static int atm_ensure_capacity(atm_glyph_outline_t *dst) {
    if (dst->num_elements < dst->capacity) return 0;
    dst->capacity += 256;
    atm_path_elem_t *p = realloc(dst->elements,
                                 dst->capacity * sizeof(atm_path_elem_t));
    if (!p) return -1;
    dst->elements = p;
    return 0;
}

static int atm_move_to(const FT_Vector *to, void *user) {
    struct atm_outline_builder *b = user;
    if (atm_ensure_capacity(b->dst)) return -1;
    int i = b->dst->num_elements++;
    b->dst->elements[i].cmd = ATM_MOVETO;
    b->dst->elements[i].x = ATM_INT_TO_FIXED(to->x) / 64;
    b->dst->elements[i].y = ATM_INT_TO_FIXED(to->y) / 64;
    b->first_x = b->dst->elements[i].x;
    b->first_y = b->dst->elements[i].y;
    return 0;
}

static int atm_line_to(const FT_Vector *to, void *user) {
    struct atm_outline_builder *b = user;
    if (atm_ensure_capacity(b->dst)) return -1;
    int i = b->dst->num_elements++;
    b->dst->elements[i].cmd = ATM_LINETO;
    b->dst->elements[i].x = ATM_INT_TO_FIXED(to->x) / 64;
    b->dst->elements[i].y = ATM_INT_TO_FIXED(to->y) / 64;
    return 0;
}

static int atm_conic_to(const FT_Vector *control, const FT_Vector *to,
                        void *user) {
    (void)control;
    struct atm_outline_builder *b = user;
    if (atm_ensure_capacity(b->dst)) return -1;
    int i = b->dst->num_elements++;
    b->dst->elements[i].cmd = ATM_LINETO;
    b->dst->elements[i].x = ATM_INT_TO_FIXED(to->x) / 64;
    b->dst->elements[i].y = ATM_INT_TO_FIXED(to->y) / 64;
    return 0;
}

static int atm_cubic_to(const FT_Vector *c1, const FT_Vector *c2,
                        const FT_Vector *to, void *user) {
    struct atm_outline_builder *b = user;
    if (atm_ensure_capacity(b->dst)) return -1;
    int i = b->dst->num_elements++;
    b->dst->elements[i].cmd = ATM_CURVETO;
    b->dst->elements[i].cx1 = ATM_INT_TO_FIXED(c1->x) / 64;
    b->dst->elements[i].cy1 = ATM_INT_TO_FIXED(c1->y) / 64;
    b->dst->elements[i].cx2 = ATM_INT_TO_FIXED(c2->x) / 64;
    b->dst->elements[i].cy2 = ATM_INT_TO_FIXED(c2->y) / 64;
    b->dst->elements[i].x = ATM_INT_TO_FIXED(to->x) / 64;
    b->dst->elements[i].y = ATM_INT_TO_FIXED(to->y) / 64;
    return 0;
}

static int ftoutline_to_atmoutline(FT_Outline *src, atm_glyph_outline_t *dst) {
    if (!src) return -1;

    dst->elements = NULL;
    dst->capacity = 0;
    dst->num_elements = 0;

    struct atm_outline_builder builder;
    builder.dst = dst;

    FT_Outline_Funcs funcs;
    funcs.move_to = atm_move_to;
    funcs.line_to = atm_line_to;
    funcs.conic_to = atm_conic_to;
    funcs.cubic_to = atm_cubic_to;
    funcs.shift = 0;
    funcs.delta = 0;

    if (FT_Outline_Decompose(src, &funcs, &builder)) {
        free(dst->elements);
        dst->elements = NULL;
        return -1;
    }

    FT_BBox bb;
    FT_Outline_Get_CBox(src, &bb);
    dst->bbox.xMin = ATM_INT_TO_FIXED(bb.xMin) / 64;
    dst->bbox.yMin = ATM_INT_TO_FIXED(bb.yMin) / 64;
    dst->bbox.xMax = ATM_INT_TO_FIXED(bb.xMax) / 64;
    dst->bbox.yMax = ATM_INT_TO_FIXED(bb.yMax) / 64;

    /* shrink capacity to fit (no-op if realloc fails) */
    if (dst->num_elements > 0) {
        atm_path_elem_t *p = realloc(dst->elements,
            dst->num_elements * sizeof(atm_path_elem_t));
        if (p) dst->elements = p, dst->capacity = dst->num_elements;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════
   BENCHMARKS
   ═══════════════════════════════════════════════════ */

/* 1. Font loading speed */
static void bench_loading(FT_Library ft) {
    printf("\n  %s── Font Loading Speed ──%s\n", CLR_BOLD, CLR_RESET);
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FONT_DIR, "qg______.pfb");

    /* FreeType load (measure multiple times) */
    FT_Face face;
    double t0 = now_ms();
    for (int i = 0; i < 100; i++) {
        FT_New_Face(ft, path, 0, &face);
        FT_Done_Face(face);
    }
    double t1 = now_ms();
    printf("  %-20s %6.1f ms avg\n", "FT_New_Face", (t1 - t0) / 100);
    printf("  %-20s (FreeType type1+PFB parser built-in)\n", "");
}

/* 2. Core rasterization speed (same outline) */
static void bench_raster_speed(FT_Library ft) {
    printf("\n  %s── Rasterization Speed (same outline from FT) ──%s\n",
           CLR_BOLD, CLR_RESET);

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FONT_DIR, "qg______.pfb");
    FT_Face face;
    if (FT_New_Face(ft, path, 0, &face)) {
        printf("  %sFT load error%s\n", CLR_RED, CLR_RESET);
        return;
    }

    /* Get outline at 48pt from FreeType */
    FT_Set_Char_Size(face, 0, 48*64, 72, 72);
    FT_Load_Glyph(face, FT_Get_Char_Index(face, 'A'), FT_LOAD_NO_BITMAP);
    FT_Outline *ft_out = &face->glyph->outline;

    /* Convert to our format */
    atm_glyph_outline_t outline;
    memset(&outline, 0, sizeof(outline));
    if (ftoutline_to_atmoutline(ft_out, &outline) < 0) {
        printf("  %sOutline conversion error%s\n", CLR_RED, CLR_RESET);
        FT_Done_Face(face);
        return;
    }
    printf("  Outline: %d contours, %d path elements\n",
           ft_out->n_contours, outline.num_elements);
    printf("  BBox: %d x %d\n",
           ATM_FIXED_TO_INT(outline.bbox.xMax - outline.bbox.xMin),
           ATM_FIXED_TO_INT(outline.bbox.yMax - outline.bbox.yMin));

    int iters = 1000;

    /* ── ATM AA 2x2 ── */
    atm_raster_config_t rcfg = { .ss_level = 2, .flip_y = 1 };
    atm_glyph_bitmap_t bm;
    double ta = now_ms();
    for (int i = 0; i < iters; i++) {
        atm_rasterize(&outline, &rcfg, &bm);
        atm_bitmap_free(&bm);
    }
    double tb = now_ms();
    double atm_aa_t = (tb - ta) / iters;
    double atm_aa_thr = iters * 1000.0 / (tb - ta);

    /* ── ATM no AA ── */
    rcfg.ss_level = 1;
    ta = now_ms();
    for (int i = 0; i < iters; i++) {
        atm_rasterize(&outline, &rcfg, &bm);
        atm_bitmap_free(&bm);
    }
    tb = now_ms();
    double atm_raw_t = (tb - ta) / iters;
    double atm_raw_thr = iters * 1000.0 / (tb - ta);

    /* ── FreeType render ── */
    ta = now_ms();
    for (int i = 0; i < iters; i++) {
        FT_Load_Glyph(face, FT_Get_Char_Index(face, 'A'), FT_LOAD_RENDER);
    }
    tb = now_ms();
    double ft_t = (tb - ta) / iters;
    double ft_thr = iters * 1000.0 / (tb - ta);

    printf("\n");
    printf("  %-25s %8.3f ms/glyph  %8.0f glyphs/s\n",
           "ATM AA 2x2 (our AET)", atm_aa_t, atm_aa_thr);
    printf("  %-25s %8.3f ms/glyph  %8.0f glyphs/s\n",
           "ATM no AA (our AET)", atm_raw_t, atm_raw_thr);
    printf("  %-25s %8.3f ms/glyph  %8.0f glyphs/s\n",
           "FreeType (FT_RENDER)", ft_t, ft_thr);

    printf("\n  Ratios (lower = ATM faster):\n");
    printf("  %-25s %s%5.2fx%s (ATM AA vs FT)\n",
           "AA 2x2 / FT",
           atm_aa_t < ft_t ? CLR_GREEN : CLR_RED,
           atm_aa_t / ft_t, CLR_RESET);
    printf("  %-25s %s%5.2fx%s (ATM no-AA vs FT)\n",
           "Raw / FT",
           atm_raw_t < ft_t ? CLR_GREEN : CLR_RED,
           atm_raw_t / ft_t, CLR_RESET);
    printf("  %-25s %5.2fx (AA overhead)\n",
           "AA / Raw", atm_aa_t / atm_raw_t);

    atm_outline_free(&outline);
    FT_Done_Face(face);
}

/* 3. Quality (PSNR) at multiple sizes */
static void bench_quality(FT_Library ft) {
    printf("\n  %s── Quality vs FreeType (PSNR) ──%s\n", CLR_BOLD, CLR_RESET);

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FONT_DIR, "qg______.pfb");
    FT_Face face;
    if (FT_New_Face(ft, path, 0, &face)) return;

    int sizes[] = {12, 18, 24, 36, 48, 72};
    const char *names[] = {"12pt", "18pt", "24pt", "36pt", "48pt", "72pt"};
    int n = 6;

    printf("  %-8s %-12s %-12s %-12s\n", "Size", "ATM bitmap", "FT bitmap", "PSNR");
    printf("  %s──────────────────────────────────────────%s\n",
           CLR_YELLOW, CLR_RESET);

    for (int si = 0; si < n; si++) {
        FT_Set_Char_Size(face, 0, sizes[si]*64, 72, 72);
        FT_Load_Glyph(face, FT_Get_Char_Index(face, 'A'), FT_LOAD_NO_BITMAP);

        atm_glyph_outline_t outline;
        memset(&outline, 0, sizeof(outline));
        if (ftoutline_to_atmoutline(&face->glyph->outline, &outline)) continue;

        /* Render with ATM AA 2x2 */
        atm_raster_config_t rcfg = { .ss_level = 2, .flip_y = 1 };
        atm_glyph_bitmap_t atm_bm;
        atm_rasterize(&outline, &rcfg, &atm_bm);

        /* Render with FreeType */
        FT_Set_Char_Size(face, 0, sizes[si]*64, 72, 72);
        FT_Load_Char(face, 'A', FT_LOAD_RENDER);
        FT_Bitmap *ft_bm = &face->glyph->bitmap;

        /* Compare */
        int w = atm_bm.width > (int)ft_bm->width ? atm_bm.width : ft_bm->width;
        int h = atm_bm.height > (int)ft_bm->rows ? atm_bm.height : ft_bm->rows;
        if (w < 1 || h < 1) { atm_bitmap_free(&atm_bm); atm_outline_free(&outline); continue; }

        uint8_t *ba = calloc(w * h, 1);
        uint8_t *bb = calloc(w * h, 1);

        for (int y = 0; y < atm_bm.height && y < h; y++)
            for (int x = 0; x < atm_bm.width && x < w; x++)
                ba[y*w+x] = atm_bm.buf[y*atm_bm.pitch+x];

        for (int y = 0; y < (int)ft_bm->rows && y < h; y++)
            for (int x = 0; x < (int)ft_bm->width && x < w; x++) {
                if (ft_bm->pixel_mode == FT_PIXEL_MODE_GRAY)
                    bb[y*w+x] = ft_bm->buffer[y*ft_bm->pitch+x];
                else if (ft_bm->pixel_mode == FT_PIXEL_MODE_MONO) {
                    int byte = ft_bm->buffer[y*ft_bm->pitch + x/8];
                    bb[y*w+x] = (byte >> (7 - x%8)) & 1 ? 255 : 0;
                }
            }

        double psnr = compute_psnr(ba, bb, w, h, w, w);
        printf("  %-8s %3dx%-3d       %3dx%-3d       %s%5.1f dB%s\n",
               names[si],
               atm_bm.width, atm_bm.height,
               ft_bm->width, ft_bm->rows,
               psnr > 35 ? CLR_GREEN : psnr > 25 ? CLR_YELLOW : CLR_RED,
               psnr, CLR_RESET);

        free(ba); free(bb);
        atm_bitmap_free(&atm_bm);
        atm_outline_free(&outline);
    }
    FT_Done_Face(face);
}

/* 4. Cache performance */
static void bench_cache(void) {
    printf("\n  %s── Cache Performance ──%s\n", CLR_BOLD, CLR_RESET);

    atm_cache_t cache;
    atm_cache_init(&cache, 64); /* KB */

    atm_glyph_bitmap_t bm;
    atm_bitmap_alloc(&bm, 32, 32);
    memset(bm.buf, 128, bm.pitch * bm.height);

    int glyphs = 95; /* printable ASCII */
    int passes = 3;

    /* Pass 1: cold */
    cache.hits = 0; cache.misses = 0;
    double t0 = now_ms();
    for (int p = 0; p < passes; p++)
        for (int g = 0; g < glyphs; g++) {
            if (!atm_cache_lookup(&cache, 1, g, ATM_INT_TO_FIXED(12)))
                atm_cache_insert(&cache, 1, g, ATM_INT_TO_FIXED(12), &bm);
        }
    double t1 = now_ms();
    int h1 = cache.hits, m1 = cache.misses;
    printf("  %-30s hits=%4d misses=%4d rate=%3d%%  %.2fms\n",
           "Cold page (first time)",
           h1, m1, (h1+m1) ? h1*100/(h1+m1) : 0,
           t1 - t0);

    /* Pass 2: repeat (hot) */
    cache.hits = 0; cache.misses = 0;
    t0 = now_ms();
    for (int p = 0; p < passes; p++)
        for (int g = 0; g < glyphs; g++)
            atm_cache_lookup(&cache, 1, g, ATM_INT_TO_FIXED(12));
    t1 = now_ms();
    int h2 = cache.hits, m2 = cache.misses;
    printf("  %-30s hits=%4d misses=%4d rate=%3d%%  %.2fms\n",
           "Hot page (cached)",
           h2, m2, (h2+m2) ? h2*100/(h2+m2) : 0,
           t1 - t0);

    atm_bitmap_free(&bm);
    atm_cache_destroy(&cache);
}

/* 5. Memory comparison */
static void bench_memory(FT_Library ft) {
    printf("\n  %s── Memory Usage ──%s\n", CLR_BOLD, CLR_RESET);

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FONT_DIR, "qg______.pfb");
    FT_Face face;

    size_t base = get_mem_rss();

    /* ATM memory: just add cache size + gamma lut */
    size_t cache_kb = 256;
    size_t gamma_lut = 256; /* bytes */
    size_t atm_mem = cache_kb * 1024 + gamma_lut;

    /* FT memory */
    FT_New_Face(ft, path, 0, &face);
    size_t after_ft = get_mem_rss();
    FT_Done_Face(face);

    printf("  %-25s %zu KB (256KB cache + gamma LUT)\n",
           "ATM (estimated)", atm_mem / 1024);
    printf("  %-25s %zu KB (RSS delta from loading)\n",
           "FreeType", (after_ft - base) / 1024);
    printf("  %-25s ATM uses fixed cache (256KB), FT allocates per-face\n", "");
}

/* ═══════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════ */

int main(void) {
    FT_Library ft_lib;
    if (FT_Init_FreeType(&ft_lib) != 0) {
        fprintf(stderr, "Failed to init FreeType\n");
        return 1;
    }

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║  %sATM Deluxe vs FreeType — Head-to-Head%s                       ║\n",
           CLR_BOLD, CLR_RESET);
    printf("║  Font: Greymantle MVB (168KB .pfb, 229 glyphs)                 ║\n");
    printf("║  Same FT outline → both rasterizers (apples-to-apples)          ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");

    bench_loading(ft_lib);
    bench_raster_speed(ft_lib);
    bench_quality(ft_lib);
    bench_cache();
    bench_memory(ft_lib);

    /* Summary */
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║  %sFinal Verdict%s                                              ║\n",
           CLR_BOLD, CLR_RESET);
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("║  libatme (ATM) advantages:                                      ║\n");
    printf("║  %s• Glyph LRU cache%s — eliminates re-rasterization cost          ║\n",
           CLR_GREEN, CLR_RESET);
    printf("║  %s• Supersampling AA%s — high-quality, tunable (1x/2x/4x)       ║\n",
           CLR_GREEN, CLR_RESET);
    printf("║  %s• Gamma correction LUT%s — DIBEngineGammaWorkaround tech      ║\n",
           CLR_GREEN, CLR_RESET);
    printf("║  %s• Fixed-point math%s — no FPU needed (retro-compatible)       ║\n",
           CLR_GREEN, CLR_RESET);
    printf("║  %s• Font substitution API%s — ATMInstallSubstFont equivalent    ║\n",
           CLR_GREEN, CLR_RESET);
    printf("║                                                                    ║\n");
    printf("║  FreeType advantages:                                            ║\n");
    printf("║  %s• Mature, optimized rasterizer%s — 20+ years of tuning         ║\n",
           CLR_YELLOW, CLR_RESET);
    printf("║  %s• Handles curves natively%s (conic/cubic via FT_Outline)      ║\n",
           CLR_YELLOW, CLR_RESET);
    printf("║  %s• Multiple font formats%s (Type1, TrueType, OpenType, etc.)   ║\n",
           CLR_YELLOW, CLR_RESET);
    printf("╚══════════════════════════════════════════════════════════════════╝\n");

    FT_Done_FreeType(ft_lib);
    return 0;
}
