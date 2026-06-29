#include "atm_gamma.h"
#include <math.h>

void atm_gamma_init(atm_gamma_t *lut, float gamma) {
    lut->gamma = gamma;
    for (int i = 0; i < 256; i++) {
        float normalized = i / 255.0f;
        /* Gamma correction: out = in^(1/gamma) */
        float corrected = powf(normalized, 1.0f / gamma);
        if (corrected < 0) corrected = 0;
        if (corrected > 1) corrected = 1;
        lut->table[i] = (uint8_t)(corrected * 255.0f);
    }
}

void atm_gamma_apply(const atm_gamma_t *lut, atm_glyph_bitmap_t *bitmap) {
    for (int y = 0; y < bitmap->height; y++) {
        for (int x = 0; x < bitmap->width; x++) {
            int off = y * bitmap->pitch + x;
            bitmap->buf[off] = lut->table[bitmap->buf[off]];
        }
    }
}
