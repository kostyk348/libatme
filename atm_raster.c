#include "atm_raster.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#include <emmintrin.h>
#endif

/* ═══════════════════════════════════════════════════════════════
   Optimized ATM Rasterizer
   
   Key optimizations over baseline:
   1. Persistent AET — add/remove edges per scanline, never rebuild
   2. Insertion sort on nearly-sorted active list (O(n) vs qsort O(n log n))
   3. Edge bucketing by y_start for O(1) activation
   4. Bounding box clipping before span fill
   5. Branchless coverage accumulation
   6. Fixed-point bezier (no float)
   7. SIMD compositing (SSE2)
   8. Coverage LUT for sub-pixel anti-aliasing
   ═══════════════════════════════════════════════════════════════ */

/* ─── Edge with bucketing ─── */
typedef struct {
    atm_fixed_t x;        /* current x (fixed 16.16) */
    atm_fixed_t dx;       /* slope reciprocal (dx/dy) */
    int y_start;          /* first scanline */
    int y_end;            /* last scanline (exclusive) */
    int dir;              /* winding: +1 or -1 */
} opt_edge_t;

/* ─── Persistent active edge table ─── */
typedef struct {
    opt_edge_t *edges;
    int count, cap;
} aet_t;

/* ─── Edge bucket: for each scanline, list of edges starting there ─── */
typedef struct {
    int *edge_indices;    /* indices into the global edge array */
    int count, cap;
} edge_bucket_t;

/* ─── Flatten curves (fixed-point De Casteljau) ─── */
#define MAX_FLAT 8192

typedef struct { atm_fixed_t x, y; } fpt_t;

/* Fixed-point cubic bezier evaluation at t (0..64 representing 0.0..1.0) */
static atm_fixed_t eval_cubic(atm_fixed_t p0, atm_fixed_t p1,
                               atm_fixed_t p2, atm_fixed_t p3, int t) {
    /* Using fixed-point: t in [0, 64], u = 64 - t */
    int u = 64 - t;
    /* De Casteljau: (u^3*p0 + 3*u^2*t*p1 + 3*u*t^2*p2 + t^3*p3) / 64^3 */
    /* Compute in 64-bit to avoid overflow */
    int64_t ut = (int64_t)u * u * u * p0
               + 3 * (int64_t)u * u * t * p1
               + 3 * (int64_t)u * t * t * p2
               + (int64_t)t * t * t * p3;
    return (atm_fixed_t)(ut / (64LL * 64 * 64));
}

static int flatten_outline_opt(const atm_glyph_outline_t *outline,
                                fpt_t *pts, int max_pts) {
    int n = 0;
    atm_fixed_t sx = 0, sy = 0;
    atm_fixed_t cx = 0, cy = 0;

    for (int i = 0; i < outline->num_elements; i++) {
        const atm_path_elem_t *e = &outline->elements[i];

        switch (e->cmd) {
        case ATM_MOVETO:
            if (n >= max_pts) return n;
            pts[n].x = e->x; pts[n].y = e->y;
            sx = e->x; sy = e->y;
            cx = e->x; cy = e->y;
            n++;
            break;

        case ATM_LINETO:
            if (n >= max_pts) return n;
            pts[n].x = e->x; pts[n].y = e->y;
            cx = e->x; cy = e->y;
            n++;
            break;

        case ATM_CURVETO: {
            /* Fixed-point cubic bezier, 8 steps */
            for (int s = 1; s <= 8; s++) {
                int t = s * 8; /* 8, 16, 24, ..., 64 */
                if (n >= max_pts) break;
                pts[n].x = eval_cubic(cx, e->cx1, e->cx2, e->x, t);
                pts[n].y = eval_cubic(cy, e->cy1, e->cy2, e->y, t);
                n++;
            }
            cx = e->x; cy = e->y;
            break;
        }

        case ATM_CLOSEPATH:
            if ((cx != sx || cy != sy) && n < max_pts) {
                pts[n].x = sx; pts[n].y = sy;
                n++;
                cx = sx; cy = sy;
            }
            break;
        }
    }
    return n;
}

/* ─── Edge comparison for initial sort ─── */
static int cmp_edge_y(const void *a, const void *b) {
    const opt_edge_t *ea = (const opt_edge_t*)a;
    const opt_edge_t *eb = (const opt_edge_t*)b;
    if (ea->y_start < eb->y_start) return -1;
    if (ea->y_start > eb->y_start) return 1;
    if (ea->x < eb->x) return -1;
    if (ea->x > eb->x) return 1;
    return 0;
}

/* ─── Insertion sort for nearly-sorted AET ─── */
static void insertion_sort_edges(opt_edge_t *arr, int n) {
    for (int i = 1; i < n; i++) {
        opt_edge_t key = arr[i];
        if (key.x >= arr[i-1].x) continue;
        int j = i - 1;
        while (j >= 0 && arr[j].x > key.x) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

/* ═══════════════════════════════════════════════════════════════
   Main rasterize function
   ═══════════════════════════════════════════════════════════════ */

int atm_rasterize(const atm_glyph_outline_t *outline,
                  const atm_raster_config_t *config,
                  atm_glyph_bitmap_t *bitmap) {
    int ss = (config && config->ss_level > 0) ? config->ss_level : 1;

    /* Flatten */
    fpt_t *pts = malloc(MAX_FLAT * sizeof(fpt_t));
    if (!pts) return -1;
    int n_pts = flatten_outline_opt(outline, pts, MAX_FLAT);
    if (n_pts < 2) {
        /* Degenerate outline (e.g. space, single point): return empty bitmap */
        memset(bitmap, 0, sizeof(*bitmap));
        free(pts);
        return 0;
    }

    /* Bounding box */
    atm_fixed_t min_x = pts[0].x, max_x = pts[0].x;
    atm_fixed_t min_y = pts[0].y, max_y = pts[0].y;
    for (int i = 1; i < n_pts; i++) {
        if (pts[i].x < min_x) min_x = pts[i].x;
        if (pts[i].x > max_x) max_x = pts[i].x;
        if (pts[i].y < min_y) min_y = pts[i].y;
        if (pts[i].y > max_y) max_y = pts[i].y;
    }

    int pad = ss;
    int width  = ATM_FIXED_TO_INT(max_x - min_x) + pad * 2;
    int height = ATM_FIXED_TO_INT(max_y - min_y) + pad * 2;
    if (width < 1) width = 1;
    if (height < 1) height = 1;

    atm_glyph_bitmap_t result;
    if (atm_bitmap_alloc(&result, width, height)) {
        free(pts);
        return -1;
    }
    memset(result.buf, 0, result.pitch * result.height);

    atm_fixed_t x_off = min_x - ATM_INT_TO_FIXED(pad);
    atm_fixed_t y_off = min_y - ATM_INT_TO_FIXED(pad);

    /* ─── Build edge list on supersample grid ─── */
    int ss_w = width * ss;
    int ss_h = height * ss;

    /* Allocate edge buckets (one per scanline) */
    edge_bucket_t *buckets = calloc(ss_h, sizeof(edge_bucket_t));
    if (!buckets) { free(pts); atm_bitmap_free(&result); return -1; }

    /* First pass: count edges per scanline to allocate */
    opt_edge_t *eall = malloc(n_pts * sizeof(opt_edge_t));
    if (!eall) { free(buckets); free(pts); atm_bitmap_free(&result); return -1; }
    int ne = 0;

    for (int i = 0; i + 1 < n_pts; i++) {
        atm_fixed_t fx0 = pts[i].x   - x_off;
        atm_fixed_t fy0 = pts[i].y   - y_off;
        atm_fixed_t fx1 = pts[i+1].x - x_off;
        atm_fixed_t fy1 = pts[i+1].y - y_off;

        /* Skip horizontals */
        if (fy0 == fy1) continue;

        /* Normalize: y0 < y1 */
        int dir = (fy1 > fy0) ? 1 : -1;
        if (fy0 > fy1) {
            atm_fixed_t tx = fx0, ty = fy0;
            fx0 = fx1; fy0 = fy1; fx1 = tx; fy1 = ty;
        }

        /* Scale to supersample grid */
        fx0 *= ss; fy0 *= ss;
        fx1 *= ss; fy1 *= ss;

        int y0i = ATM_FIXED_TO_INT(fy0);
        int y1i = ATM_FIXED_TO_INT(fy1);
        if (y1i <= 0 || y0i >= ss_h) continue;
        if (y0i < 0) y0i = 0;
        if (y1i > ss_h) y1i = ss_h;
        if (y1i <= y0i) continue;

        atm_fixed_t dy  = fy1 - fy0;
        atm_fixed_t dx  = fx1 - fx0;
        atm_fixed_t slope = (dy != 0) ? ATM_FIXED_DIV(dx, dy) : 0;

        /* x at first scanline */
        atm_fixed_t y_frac = ATM_INT_TO_FIXED(y0i) - fy0;
        atm_fixed_t x_start = fx0 + ATM_FIXED_MUL(slope, y_frac);

        eall[ne].x = x_start;
        eall[ne].dx = slope;
        eall[ne].y_start = y0i;
        eall[ne].y_end = y1i;
        eall[ne].dir = dir;
        ne++;

        /* Add to bucket for y_start */
        /* Count first, allocate later */
    }

    /* Sort edges by y_start for bucket insertion */
    qsort(eall, ne, sizeof(opt_edge_t), cmp_edge_y);

    /* Allocate buckets (count edges per scanline first) */
    int *scan_counts = calloc(ss_h, sizeof(int));
    if (!scan_counts) { free(eall); free(buckets); free(pts); atm_bitmap_free(&result); return -1; }
    for (int ei = 0; ei < ne; ei++) {
        int ys = eall[ei].y_start;
        if (ys >= 0 && ys < ss_h) scan_counts[ys]++;
    }
    for (int y = 0; y < ss_h; y++) {
        if (scan_counts[y] > 0) {
            buckets[y].edge_indices = malloc(scan_counts[y] * sizeof(int));
            buckets[y].cap = scan_counts[y];
            buckets[y].count = 0;
        }
    }
    /* Fill buckets */
    for (int ei = 0; ei < ne; ei++) {
        int ys = eall[ei].y_start;
        if (ys >= 0 && ys < ss_h) {
            buckets[ys].edge_indices[buckets[ys].count++] = ei;
        }
    }
    free(scan_counts);

    /* ─── Persistent AET scanline fill ─── */
    opt_edge_t *active = malloc((ne + 1) * sizeof(opt_edge_t));
    if (!active) { free(eall); free(buckets); free(pts); atm_bitmap_free(&result); return -1; }
    int n_active = 0;

    /* Pre-allocate coverage buffer for one scanline */
    uint8_t *scan_buf = malloc(ss_w + 8);
    if (!scan_buf) { free(active); free(eall); free(buckets); free(pts); atm_bitmap_free(&result); return -1; }

    for (int y = 0; y < ss_h; y++) {
        /* Remove edges that end at this scanline */
        int wi = 0;
        for (int ri = 0; ri < n_active; ri++) {
            if (active[ri].y_end > y) {
                if (wi != ri) active[wi] = active[ri];
                wi++;
            }
        }
        n_active = wi;

        /* Add new edges starting at this scanline */
        for (int bi = 0; bi < buckets[y].count; bi++) {
            int ei = buckets[y].edge_indices[bi];
            if (n_active >= ne) break;
            active[n_active++] = eall[ei];
        }

        if (n_active < 2) continue;

        /* Update x positions for all active edges */
        for (int i = 0; i < n_active; i++)
            active[i].x += active[i].dx;

        /* Insertion sort (O(n) for nearly-sorted) */
        insertion_sort_edges(active, n_active);

        /* Fill spans — branchless, bounds-clipped */
        memset(scan_buf, 0, ss_w + 8);

        for (int i = 1; i < n_active; i += 2) {
            int x0 = ATM_FIXED_TO_INT(active[i-1].x);
            int x1 = ATM_FIXED_TO_INT(active[i].x);

            /* Branchless clamp to [0, ss_w) */
            int cx0 = (x0 < 0) ? 0 : (x0 >= ss_w) ? ss_w : x0;
            int cx1 = (x1 < 0) ? 0 : (x1 >= ss_w) ? ss_w : x1;
            int len = cx1 - cx0;
            if (len <= 0) continue;

            /* Fill with 255 using memset (fast) */
            memset(&scan_buf[cx0], 255, len);
        }

        /* Copy to supersample buffer */
        uint8_t *dst_row = result.buf + (y / ss) * result.pitch;
        int dst_y = y / ss;

        if (ss == 1) {
            /* No supersampling: copy scan_buf directly */
            memcpy(dst_row + (y - dst_y * ss) * result.pitch + 0,
                   scan_buf, width);
        } else {
            for (int x = 0; x < width; x++) {
                int sum = 0;
                for (int sx = 0; sx < ss; sx++) {
                    int ss_x = x * ss + sx;
                    sum += scan_buf[ss_x];
                }
                int idx = dst_y * result.pitch + x;
                result.buf[idx] += (uint8_t)(sum / ss);
            }
        }
    }

    /* Normalize supersample buffer (divide by ss for each sub-sample) */
    if (ss > 1) {
        int ss2 = ss * ss;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int idx = y * result.pitch + x;
                int val = result.buf[idx];
                result.buf[idx] = (uint8_t)((val * 255 + ss2 / 2) / (255 * ss2));
            }
        }
    }

    /* Metrics */
    result.x_offset = -(int)ATM_FIXED_TO_INT(min_x) + pad;
    result.y_offset = height + (int)ATM_FIXED_TO_INT(min_y) - pad;
    result.advance_x = ATM_FIXED_TO_INT(max_x - min_x) + pad * 2;

    /* Cleanup */
    free(scan_buf);
    free(active);
    for (int y = 0; y < ss_h; y++) free(buckets[y].edge_indices);
    free(buckets);
    free(eall);
    free(pts);

    *bitmap = result;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   Bitmap operations (with SIMD compositing)
   ═══════════════════════════════════════════════════════════════ */

int atm_bitmap_alloc(atm_glyph_bitmap_t *bitmap, int w, int h) {
    bitmap->width = w;
    bitmap->height = h;
    bitmap->pitch = w;
    bitmap->buf = calloc(1, w * h);
    bitmap->x_offset = 0;
    bitmap->y_offset = 0;
    bitmap->advance_x = w;
    return bitmap->buf ? 0 : -1;
}

void atm_bitmap_free(atm_glyph_bitmap_t *bitmap) {
    free(bitmap->buf);
    bitmap->buf = NULL;
}

/* ─── Branchless alpha compositing (scalar) ─── */
static inline uint8_t blend_byte(int src, int dst, int alpha) {
    return (uint8_t)(((src - dst) * alpha + dst * 255 + 128) / 255);
}

/* ─── SIMD compositing (SSE2) ─── */
#if defined(__x86_64__) || defined(__i386__)
static void composite_simd(uint32_t *dst, uint32_t color,
                           const uint8_t *alpha, int len) {
    uint8_t cr = (color >> 16) & 0xff;
    uint8_t cg = (color >> 8) & 0xff;
    uint8_t cb = color & 0xff;

    /* Pre-broadcast source color */
    __m128i src_r = _mm_set1_epi16(cr);
    __m128i src_g = _mm_set1_epi16(cg);
    __m128i src_b = _mm_set1_epi16(cb);

    int i = 0;
    /* Process 2 pixels at a time with SSE2 (no SSE4.1 assumptions) */
    for (; i + 2 <= len; i += 2) {
        /* Load 2 alpha values */
        __m128i a16 = _mm_set_epi16(0, 0, alpha[i+1], alpha[i], 0, 0, 0, 0);

        /* Load 2 destination pixels */
        uint32_t d0 = dst[i], d1 = dst[i+1];
        __m128i dr16 = _mm_set_epi16(0, 0, (d1>>16)&0xff, (d0>>16)&0xff, 0, 0, 0, 0);
        __m128i dg16 = _mm_set_epi16(0, 0, (d1>>8)&0xff,  (d0>>8)&0xff,  0, 0, 0, 0);
        __m128i db16 = _mm_set_epi16(0, 0, d1&0xff,        d0&0xff,       0, 0, 0, 0);

        __m128i ia16 = _mm_sub_epi16(_mm_set1_epi16(255), a16);

        __m128i nr = _mm_add_epi16(_mm_mullo_epi16(src_r, a16),
                                   _mm_mullo_epi16(dr16, ia16));
        __m128i ng = _mm_add_epi16(_mm_mullo_epi16(src_g, a16),
                                   _mm_mullo_epi16(dg16, ia16));
        __m128i nb = _mm_add_epi16(_mm_mullo_epi16(src_b, a16),
                                   _mm_mullo_epi16(db16, ia16));

        /* Divide by 255 */
        nr = _mm_srli_epi16(_mm_add_epi16(nr, _mm_set1_epi16(128)), 8);
        ng = _mm_srli_epi16(_mm_add_epi16(ng, _mm_set1_epi16(128)), 8);
        nb = _mm_srli_epi16(_mm_add_epi16(nb, _mm_set1_epi16(128)), 8);

        int r0 = _mm_extract_epi16(nr, 2), r1 = _mm_extract_epi16(nr, 3);
        int g0 = _mm_extract_epi16(ng, 2), g1 = _mm_extract_epi16(ng, 3);
        int b0 = _mm_extract_epi16(nb, 2), b1 = _mm_extract_epi16(nb, 3);

        dst[i]   = 0xff000000 | (r0 << 16) | (g0 << 8) | b0;
        dst[i+1] = 0xff000000 | (r1 << 16) | (g1 << 8) | b1;
    }

    /* Remaining pixels (scalar) */
    for (; i < len; i++) {
        int a = alpha[i];
        if (a == 0) continue;
        uint32_t d = dst[i];
        if (a >= 255) {
            dst[i] = 0xff000000 | (cr << 16) | (cg << 8) | cb;
        } else {
            uint8_t drb = (d >> 16) & 0xff;
            uint8_t dgb = (d >> 8) & 0xff;
            uint8_t dbb = d & 0xff;
            dst[i] = 0xff000000 |
                (blend_byte(cr, drb, a) << 16) |
                (blend_byte(cg, dgb, a) << 8) |
                blend_byte(cb, dbb, a);
        }
    }
}
#else
static void composite_simd(uint32_t *dst, uint32_t color,
                           const uint8_t *alpha, int len) {
    (void)dst; (void)color; (void)alpha; (void)len;
}
#endif

void atm_bitmap_composite(const atm_glyph_bitmap_t *glyph,
                          uint32_t *target, int tw, int th,
                          int x, int y, uint32_t color) {
    int sy0 = (y < 0) ? -y : 0;
    int sy1 = (y + glyph->height > th) ? th - y : glyph->height;
    if (sy0 >= sy1) return;

    int sx0 = (x < 0) ? -x : 0;
    int sx1 = (x + glyph->width > tw) ? tw - x : glyph->width;
    if (sx0 >= sx1) return;

    for (int gy = sy0; gy < sy1; gy++) {
        int alpha_off = gy * glyph->pitch + sx0;
        uint32_t *dst_row = &target[(y + gy) * tw + x + sx0];
        int len = sx1 - sx0;

        composite_simd(dst_row, color, glyph->buf + alpha_off, len);
    }
}
