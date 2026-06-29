#include "atm_cache.h"
#include "atm_raster.h"
#include <stdlib.h>
#include <string.h>

void atm_cache_init(atm_cache_t *cache, int max_size_kb) {
    memset(cache, 0, sizeof(*cache));
    cache->max_bytes = max_size_kb * 1024;
}

void atm_cache_destroy(atm_cache_t *cache) {
    atm_cache_entry_t *e = cache->head;
    while (e) {
        atm_cache_entry_t *next = e->next;
        atm_bitmap_free(&e->bitmap);
        free(e);
        e = next;
    }
    memset(cache, 0, sizeof(*cache));
}

atm_cache_entry_t *atm_cache_lookup(atm_cache_t *cache,
                                    int font_id, int glyph_id,
                                    atm_fixed_t size) {
    atm_cache_entry_t *e = cache->head;
    while (e) {
        if (e->font_id == font_id && e->glyph_id == glyph_id && e->size == size) {
            /* Promote to MRU */
            if (e != cache->head) {
                if (e == cache->tail) cache->tail = e->prev;
                if (e->prev) e->prev->next = e->next;
                if (e->next) e->next->prev = e->prev;
                e->next = cache->head;
                e->prev = NULL;
                if (cache->head) cache->head->prev = e;
                cache->head = e;
            }
            cache->hits++;
            return e;
        }
        e = e->next;
    }
    cache->misses++;
    return NULL;
}

atm_cache_entry_t *atm_cache_insert(atm_cache_t *cache,
                                    int font_id, int glyph_id,
                                    atm_fixed_t size,
                                    const atm_glyph_bitmap_t *bitmap) {
    int needed = (int)(sizeof(atm_cache_entry_t) + bitmap->pitch * bitmap->height);
    atm_cache_evict(cache, needed);

    if (needed > cache->max_bytes) return NULL;

    atm_cache_entry_t *e = calloc(1, sizeof(atm_cache_entry_t));
    if (!e) return NULL;

    e->font_id = font_id;
    e->glyph_id = glyph_id;
    e->size = size;
    atm_bitmap_alloc(&e->bitmap, bitmap->width, bitmap->height);
    if (e->bitmap.buf && bitmap->buf)
        memcpy(e->bitmap.buf, bitmap->buf, bitmap->pitch * bitmap->height);
    e->bitmap.x_offset = bitmap->x_offset;
    e->bitmap.y_offset = bitmap->y_offset;
    e->bitmap.advance_x = bitmap->advance_x;

    e->next = cache->head;
    e->prev = NULL;
    if (cache->head) cache->head->prev = e;
    cache->head = e;
    if (!cache->tail) cache->tail = e;

    cache->used_bytes += needed;
    cache->num_entries++;
    return e;
}

void atm_cache_evict(atm_cache_t *cache, int needed_bytes) {
    while (cache->used_bytes + needed_bytes > cache->max_bytes && cache->tail) {
        atm_cache_entry_t *victim = cache->tail;
        int freed = (int)(sizeof(atm_cache_entry_t) +
                          victim->bitmap.pitch * victim->bitmap.height);

        cache->tail = victim->prev;
        if (cache->tail) cache->tail->next = NULL;
        if (cache->head == victim) cache->head = NULL;

        atm_bitmap_free(&victim->bitmap);
        free(victim);
        cache->used_bytes -= freed;
        cache->num_entries--;
    }
}

void atm_cache_invalidate_font(atm_cache_t *cache, int font_id) {
    atm_cache_entry_t *e = cache->head;
    while (e) {
        atm_cache_entry_t *next = e->next;
        if (e->font_id == font_id) {
            /* Remove from list */
            if (e->prev) e->prev->next = e->next;
            if (e->next) e->next->prev = e->prev;
            if (cache->head == e) cache->head = e->next;
            if (cache->tail == e) cache->tail = e->prev;
            int freed = (int)(sizeof(atm_cache_entry_t) +
                              e->bitmap.pitch * e->bitmap.height);
            cache->used_bytes -= freed;
            cache->num_entries--;
            atm_bitmap_free(&e->bitmap);
            free(e);
        }
        e = next;
    }
}

int atm_cache_hit_rate(const atm_cache_t *cache) {
    int total = cache->hits + cache->misses;
    if (total == 0) return 100;
    return (cache->hits * 100) / total;
}
