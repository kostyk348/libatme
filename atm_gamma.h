#ifndef ATM_GAMMA_H
#define ATM_GAMMA_H

#include "atm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Gamma correction lookup table ───
 * DIBEngineGammaWorkaround=On in ATM.CNF
 * Applies gamma correction to anti-aliased coverage values
 * for proper brightness on CRT/LCD displays. */

typedef struct {
    uint8_t table[256];  /* coverage → gamma-corrected coverage */
    float gamma;
} atm_gamma_t;

/* ─── Initialize gamma table ───
 *  gamma = 1.0 → linear (no correction)
 *  gamma < 1.0 → brighten midtones
 *  gamma > 1.0 → darken midtones (typical: 1.8-2.2) */
void atm_gamma_init(atm_gamma_t *lut, float gamma);

/* ─── Apply gamma correction to bitmap ─── */
void atm_gamma_apply(const atm_gamma_t *lut, atm_glyph_bitmap_t *bitmap);

/* ─── Apply gamma to single coverage value ─── */
static inline uint8_t atm_gamma_correct(const atm_gamma_t *lut, uint8_t v) {
    return lut->table[v];
}

#ifdef __cplusplus
}
#endif

#endif /* ATM_GAMMA_H */
