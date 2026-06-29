#ifndef ATM_RASTER_H
#define ATM_RASTER_H

#include "atm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Anti-aliased glyph rasterizer ───
 * Converts a glyph outline to an 8-bit coverage bitmap.
 * This is the core ATM technology: gray-scale anti-aliased font rendering.
 *
 * Uses a configurable supersampling grid (2x2 or 4x4) for anti-aliasing. */

typedef struct {
    int ss_level;     /* supersample level: 1=no AA, 2=2x2, 4=4x4 */
    int flip_y;       /* 1=flip Y (screen coords vs font coords) */
} atm_raster_config_t;

/* ─── Rasterize glyph outline to alpha bitmap ───
 *  outline   - glyph path data
 *  config    - rasterization parameters (anti-alias level)
 *  bitmap    - output bitmap (caller allocates via atm_bitmap_alloc)
 * Returns 0 on success, <0 on error. */
int atm_rasterize(const atm_glyph_outline_t *outline,
                  const atm_raster_config_t *config,
                  atm_glyph_bitmap_t *bitmap);

/* ─── Allocate glyph bitmap ─── */
int atm_bitmap_alloc(atm_glyph_bitmap_t *bitmap, int w, int h);

/* ─── Free glyph bitmap ─── */
void atm_bitmap_free(atm_glyph_bitmap_t *bitmap);

/* ─── Composite glyph bitmap onto target ───
 * Places the glyph at (x,y) on the target buffer (32-bit RGBA). */
void atm_bitmap_composite(const atm_glyph_bitmap_t *glyph,
                          uint32_t *target, int target_w, int target_h,
                          int x, int y, uint32_t color);

#ifdef __cplusplus
}
#endif

#endif /* ATM_RASTER_H */
