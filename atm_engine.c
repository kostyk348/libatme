#include "atm_engine.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int atm_engine_init(atm_engine_t *engine, const atm_config_t *config) {
    memset(engine, 0, sizeof(*engine));
    if (config) {
        engine->config = *config;
    } else {
        engine->config.flags = ATM_FLAG_ANTIALIAS |
                               ATM_FLAG_GAMMA_CORRECT |
                               ATM_FLAG_CACHE_ENABLED;
        engine->config.cache_size_kb = 256;
        engine->config.gamma = 1.8f;
        engine->config.antialias_level = 2;
    }
    atm_cache_init(&engine->cache, engine->config.cache_size_kb);
    atm_gamma_init(&engine->gamma, engine->config.gamma);
    engine->next_font_id = 1;
    return 0;
}

void atm_engine_shutdown(atm_engine_t *engine) {
    for (int i = 0; i < ATM_MAX_FONTS; i++) {
        if (engine->fonts[i]) {
            atm_font_close(engine->fonts[i]);
            free(engine->fonts[i]);
            engine->fonts[i] = NULL;
        }
    }
    atm_cache_destroy(&engine->cache);
    memset(engine, 0, sizeof(*engine));
}

static atm_font_t *find_font(atm_engine_t *engine, int font_id) {
    for (int i = 0; i < ATM_MAX_FONTS; i++)
        if (engine->fonts[i] && engine->fonts[i]->font_id == font_id)
            return engine->fonts[i];
    return NULL;
}

int atm_engine_load_font(atm_engine_t *engine, const char *pfb_path,
                         const char *pfm_path) {
    atm_font_t *font = calloc(1, sizeof(atm_font_t));
    if (!font) return -1;
    if (atm_font_open(pfb_path, pfm_path, font) < 0) {
        free(font);
        return -1;
    }
    font->font_id = engine->next_font_id++;
    for (int i = 0; i < ATM_MAX_FONTS; i++) {
        if (!engine->fonts[i]) {
            engine->fonts[i] = font;
            return font->font_id;
        }
    }
    atm_font_close(font);
    free(font);
    return -1;
}

void atm_engine_unload_font(atm_engine_t *engine, int font_id) {
    atm_cache_invalidate_font(&engine->cache, font_id);
    for (int i = 0; i < ATM_MAX_FONTS; i++) {
        if (engine->fonts[i] && engine->fonts[i]->font_id == font_id) {
            atm_font_close(engine->fonts[i]);
            free(engine->fonts[i]);
            engine->fonts[i] = NULL;
            return;
        }
    }
}

int atm_engine_get_font_info(atm_engine_t *engine, int font_id,
                             atm_font_info_t *info) {
    atm_font_t *font = find_font(engine, font_id);
    if (!font || !info) return -1;
    *info = font->info;
    return 0;
}

static void compute_outline_bbox(atm_glyph_outline_t *outline) {
    if (outline->num_elements < 1) {
        outline->bbox.xMin = 0;
        outline->bbox.yMin = 0;
        outline->bbox.xMax = 0;
        outline->bbox.yMax = 0;
        return;
    }
    atm_fixed_t xmin = outline->elements[0].x;
    atm_fixed_t ymin = outline->elements[0].y;
    atm_fixed_t xmax = xmin, ymax = ymin;
    for (int i = 0; i < outline->num_elements; i++) {
        atm_path_elem_t *e = &outline->elements[i];
        if (e->x < xmin) xmin = e->x;
        if (e->x > xmax) xmax = e->x;
        if (e->y < ymin) ymin = e->y;
        if (e->y > ymax) ymax = e->y;
        if (e->cmd == ATM_CURVETO) {
            if (e->cx1 < xmin) xmin = e->cx1;
            if (e->cx1 > xmax) xmax = e->cx1;
            if (e->cy1 < ymin) ymin = e->cy1;
            if (e->cy1 > ymax) ymax = e->cy1;
            if (e->cx2 < xmin) xmin = e->cx2;
            if (e->cx2 > xmax) xmax = e->cx2;
            if (e->cy2 < ymin) ymin = e->cy2;
            if (e->cy2 > ymax) ymax = e->cy2;
        }
    }
    outline->bbox.xMin = xmin;
    outline->bbox.yMin = ymin;
    outline->bbox.xMax = xmax;
    outline->bbox.yMax = ymax;
}

int atm_engine_get_outline(atm_engine_t *engine, int font_id,
                           int glyph_id, atm_fixed_t size,
                           atm_glyph_outline_t *outline) {
    atm_font_t *font = find_font(engine, font_id);
    if (!font) return -1;

    int idx = atm_font_glyph_index(font, glyph_id);
    if (idx < 0) return -1;

    const uint8_t *const *subrs = (const uint8_t *const *)font->subrs_data;
    if (atm_decode_charstring_subrs(font->glyph_data[idx],
                                     font->glyph_lens[idx],
                                     outline,
                                     subrs,
                                     font->subrs_lens,
                                     font->num_subrs) < 0)
        return -1;

    /* Scale outline to requested size */
    atm_fixed_t units_per_em = font->info.units_per_em;
    if (units_per_em <= 0)
        units_per_em = ATM_INT_TO_FIXED(1000);
    atm_fixed_t scale = ATM_FIXED_DIV(size, units_per_em);
    atm_outline_scale(outline, scale);

    /* Update bbox after scaling */
    compute_outline_bbox(outline);

    outline->advance_width = font->widths ?
        ATM_INT_TO_FIXED(font->widths[glyph_id]) : size;
    return 0;
}

int atm_engine_render_glyph(atm_engine_t *engine, int font_id,
                            int glyph_id, atm_fixed_t size,
                            atm_glyph_bitmap_t *bitmap) {
    if (engine->config.flags & ATM_FLAG_CACHE_ENABLED) {
        atm_cache_entry_t *cached = atm_cache_lookup(
            &engine->cache, font_id, glyph_id, size);
        if (cached) {
            /* Deep copy so caller can free independently */
            atm_bitmap_alloc(bitmap, cached->bitmap.width,
                             cached->bitmap.height);
            if (bitmap->buf && cached->bitmap.buf)
                memcpy(bitmap->buf, cached->bitmap.buf,
                       cached->bitmap.pitch * cached->bitmap.height);
            bitmap->x_offset = cached->bitmap.x_offset;
            bitmap->y_offset = cached->bitmap.y_offset;
            bitmap->advance_x = cached->bitmap.advance_x;
            return 0;
        }
    }

    atm_glyph_outline_t outline;
    memset(&outline, 0, sizeof(outline));
    if (atm_engine_get_outline(engine, font_id, glyph_id, size,
                               &outline) < 0)
        return -1;

    /* Empty outline (space, nbsp) → return empty bitmap with advance width */
    if (outline.num_elements == 0) {
        memset(bitmap, 0, sizeof(*bitmap));
        bitmap->advance_x = ATM_FIXED_TO_INT(outline.advance_width);
        atm_outline_free(&outline);
        return 0;
    }

    atm_raster_config_t rcfg = {
        .ss_level = (engine->config.flags & ATM_FLAG_ANTIALIAS)
                    ? engine->config.antialias_level : 1,
        .flip_y = 1,
    };

    if (atm_rasterize(&outline, &rcfg, bitmap) < 0) {
        atm_outline_free(&outline);
        return -1;
    }

    if (engine->config.flags & ATM_FLAG_GAMMA_CORRECT)
        atm_gamma_apply(&engine->gamma, bitmap);

    bitmap->advance_x = ATM_FIXED_TO_INT(outline.advance_width);

    if (engine->config.flags & ATM_FLAG_CACHE_ENABLED)
        atm_cache_insert(&engine->cache, font_id, glyph_id, size, bitmap);

    atm_outline_free(&outline);
    return 0;
}

int atm_engine_render_text(atm_engine_t *engine, int font_id,
                           const char *text, atm_fixed_t size,
                           uint32_t *target, int target_w, int target_h,
                           int x, int y, uint32_t color) {
    if (!text) return -1;
    int cx = x, cy = y;
    for (const char *p = text; *p; p++) {
        int gid = (unsigned char)*p;
        atm_glyph_bitmap_t bm;
        if (atm_engine_render_glyph(engine, font_id, gid, size, &bm) == 0) {
            atm_bitmap_composite(&bm, target, target_w, target_h,
                                 cx + bm.x_offset, cy - bm.y_offset, color);
            cx += bm.advance_x;
            atm_bitmap_free(&bm);
        }
    }
    return 0;
}

int atm_engine_set_substitution(atm_engine_t *engine,
                                const char *from, const char *to) {
    (void)engine; (void)from; (void)to;
    return 0;
}

void atm_engine_set_flag(atm_engine_t *engine, atm_flags_t flag, int on) {
    if (on) engine->config.flags |= flag;
    else    engine->config.flags &= ~flag;
}

void atm_engine_set_gamma(atm_engine_t *engine, float gamma) {
    engine->config.gamma = gamma;
    atm_gamma_init(&engine->gamma, gamma);
}

void atm_engine_print_stats(const atm_engine_t *engine) {
    printf("╔════════════════════════════╗\n");
    printf("║   ATM Engine Stats         ║\n");
    printf("╠════════════════════════════╣\n");
    printf("║ Cache: %d/%d KB            ║\n",
           engine->cache.used_bytes / 1024,
           engine->cache.max_bytes / 1024);
    printf("║ Entries: %d                ║\n",
           engine->cache.num_entries);
    printf("║ Hit rate: %d%%             ║\n",
           atm_cache_hit_rate(&engine->cache));
    printf("║ Gamma: %.1f                ║\n",
           engine->config.gamma);
    printf("╚════════════════════════════╝\n");
}
