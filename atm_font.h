#ifndef ATM_FONT_H
#define ATM_FONT_H

#include "atm_types.h"
#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── PFB segment ─── */
typedef struct {
    int type;           /* 1=ASCII, 2=BINARY, 3=EOF */
    uint8_t *data;
    uint32_t len;
} atm_pfb_seg_t;

/* ─── PFB file ─── */
typedef struct {
    atm_pfb_seg_t *segments;
    int num_segments;
} atm_pfb_t;

/* ─── PFM file (Windows .pfm metrics, partial) ─── */
typedef struct {
    uint8_t header[512];  /* PFM header + extended metrics */
} atm_pfm_t;

/* ─── Loaded font ─── */
typedef struct {
    int font_id;
    atm_pfb_t pfb;
    atm_pfm_t pfm;
    char *ascii_data;
    int ascii_len;
    atm_font_info_t info;
    int *glyph_ids;
    uint8_t **glyph_data;
    int *glyph_lens;
    char **glyph_names;
    int num_glyphs;
    int *widths;
    uint8_t **subrs_data;
    int *subrs_lens;
    int num_subrs;
    int lenIV;  /* Number of random bytes before each charstring (default 4) */
} atm_font_t;

/* ─── PFB operations ─── */
int  atm_pfb_load(const char *path, atm_pfb_t *pfb);
void atm_pfb_free(atm_pfb_t *pfb);

/* ─── PFM operations ─── */
int  atm_pfm_load(const char *path, atm_pfm_t *pfm);

/* ─── Font operations ─── */
int  atm_font_open(const char *pfb_path, const char *pfm_path, atm_font_t *font);
void atm_font_close(atm_font_t *font);

/* ─── Character code → glyph index lookup (StandardEncoding) ─── */
int  atm_font_glyph_index(atm_font_t *font, int charcode);

#ifdef __cplusplus
}
#endif

#endif /* ATM_FONT_H */
