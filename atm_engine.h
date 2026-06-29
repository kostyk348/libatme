#ifndef ATM_ENGINE_H
#define ATM_ENGINE_H

#include "atm_types.h"
#include "atm_font.h"
#include "atm_outline.h"
#include "atm_raster.h"
#include "atm_cache.h"
#include "atm_gamma.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Main ATM Engine ─── */

#define ATM_MAX_FONTS 32

typedef struct {
    atm_config_t config;
    atm_cache_t cache;
    atm_gamma_t gamma;
    int next_font_id;
    atm_font_t *fonts[ATM_MAX_FONTS];
} atm_engine_t;

/* ─── Engine lifecycle (mirrors ATMClient/ATMFinish) ─── */
int  atm_engine_init(atm_engine_t *engine, const atm_config_t *config);
void atm_engine_shutdown(atm_engine_t *engine);

/* ─── Font management ─── */
int  atm_engine_load_font(atm_engine_t *engine, const char *pfb_path,
                          const char *pfm_path);
void atm_engine_unload_font(atm_engine_t *engine, int font_id);
int  atm_engine_get_font_info(atm_engine_t *engine, int font_id,
                              atm_font_info_t *info);

/* ─── Glyph outline retrieval (ATMGetOutline equivalent) ───
 * Returns outline at specified size, or <0 on error. */
int  atm_engine_get_outline(atm_engine_t *engine, int font_id,
                            int glyph_id, atm_fixed_t size,
                            atm_glyph_outline_t *outline);

/* ─── Text rendering (ATMXYShowText equivalent) ───
 * Renders text string to a bitmap at the given position. */
int  atm_engine_render_text(atm_engine_t *engine, int font_id,
                            const char *text, atm_fixed_t size,
                            uint32_t *target, int target_w, int target_h,
                            int x, int y, uint32_t color);

/* ─── Render single glyph with caching ─── */
int  atm_engine_render_glyph(atm_engine_t *engine, int font_id,
                             int glyph_id, atm_fixed_t size,
                             atm_glyph_bitmap_t *bitmap);

/* ─── Font substitution (ATMInstallSubstFont equivalent) ─── */
int  atm_engine_set_substitution(atm_engine_t *engine,
                                 const char *from_font,
                                 const char *to_font);

/* ─── Configuration ─── */
void atm_engine_set_flag(atm_engine_t *engine, atm_flags_t flag, int on);
void atm_engine_set_gamma(atm_engine_t *engine, float gamma);

/* ─── Stats ─── */
void atm_engine_print_stats(const atm_engine_t *engine);

#ifdef __cplusplus
}
#endif

#endif /* ATM_ENGINE_H */
