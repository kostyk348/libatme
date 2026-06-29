#ifndef ATM_CACHE_H
#define ATM_CACHE_H

#include "atm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Glyph cache (LRU, like ATM's FontCache=256) ─── */

typedef struct atm_cache_entry_t {
    int font_id;
    int glyph_id;
    atm_fixed_t size;         /* requested size */
    atm_glyph_bitmap_t bitmap;
    struct atm_cache_entry_t *next;
    struct atm_cache_entry_t *prev;
} atm_cache_entry_t;

typedef struct {
    int max_bytes;           /* max cache size (FontCache in bytes) */
    int used_bytes;          /* current usage */
    atm_cache_entry_t *head; /* MRU end */
    atm_cache_entry_t *tail; /* LRU end */
    int num_entries;
    int hits;
    int misses;
} atm_cache_t;

/* ─── Cache operations ─── */
void atm_cache_init(atm_cache_t *cache, int max_size_kb);
void atm_cache_destroy(atm_cache_t *cache);

/* ─── Lookup glyph in cache ───
 * Returns entry if found (MRU promoted) or NULL. */
atm_cache_entry_t *atm_cache_lookup(atm_cache_t *cache,
                                    int font_id, int glyph_id,
                                    atm_fixed_t size);

/* ─── Insert glyph into cache (becomes MRU) ─── */
atm_cache_entry_t *atm_cache_insert(atm_cache_t *cache,
                                    int font_id, int glyph_id,
                                    atm_fixed_t size,
                                    const atm_glyph_bitmap_t *bitmap);

/* ─── Evict LRU entries until under limit ─── */
void atm_cache_evict(atm_cache_t *cache, int needed_bytes);

/* ─── Invalidate all entries for a font ─── */
void atm_cache_invalidate_font(atm_cache_t *cache, int font_id);

/* ─── Stats ─── */
int atm_cache_hit_rate(const atm_cache_t *cache);

#ifdef __cplusplus
}
#endif

#endif /* ATM_CACHE_H */
