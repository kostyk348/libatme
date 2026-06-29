#ifndef ATM_OUTLINE_H
#define ATM_OUTLINE_H

#include "atm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Type 1 CharString interpreter ───
 * Decodes PostScript Type 1 charstring programs into path elements.
 * subrs/subrs_len can be NULL if no subroutines.
 * Returns number of path elements, or <0 on error.
 * Caller must free outline->elements with free(). */
int atm_decode_charstring(const uint8_t *cs_data, int cs_len,
                          atm_glyph_outline_t *outline);
int atm_decode_charstring_subrs(const uint8_t *cs_data, int cs_len,
                                atm_glyph_outline_t *outline,
                                const uint8_t *const *subrs,
                                const int *subrs_lens, int num_subrs);

/* ─── Scale outline to pixel grid ─── */
void atm_outline_scale(atm_glyph_outline_t *outline, atm_fixed_t scale);

/* ─── Apply hint substitution (simplified) ─── */
void atm_outline_apply_hints(atm_glyph_outline_t *outline);

/* ─── Free outline elements ─── */
void atm_outline_free(atm_glyph_outline_t *outline);

#ifdef __cplusplus
}
#endif

#endif /* ATM_OUTLINE_H */
