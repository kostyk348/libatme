#ifndef ATM_TYPES_H
#define ATM_TYPES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Fixed-point types (like ATM's FUnit) ─── */
typedef int32_t atm_fixed_t;
#define ATM_FIXED_SHIFT 16
#define ATM_FIXED_SCALE (1 << ATM_FIXED_SHIFT)
#define ATM_FLOAT_TO_FIXED(f) ((atm_fixed_t)((f) * ATM_FIXED_SCALE))
#define ATM_FIXED_TO_FLOAT(x) ((float)(x) / ATM_FIXED_SCALE)
#define ATM_INT_TO_FIXED(i) ((atm_fixed_t)((unsigned)(i) << ATM_FIXED_SHIFT))
#define ATM_FIXED_TO_INT(x) ((int)((x) >> ATM_FIXED_SHIFT))
#define ATM_FIXED_MUL(a,b) (((int64_t)(a) * (b)) >> ATM_FIXED_SHIFT)
#define ATM_FIXED_DIV(a,b) (((int64_t)(a) << ATM_FIXED_SHIFT) / (b))

/* ─── Point / Vector ─── */
typedef struct { atm_fixed_t x, y; } atm_point_t;

/* ─── Bounding box ─── */
typedef struct { int16_t xMin, yMin, xMax, yMax; } atm_bbox_s16_t;
typedef struct { atm_fixed_t xMin, yMin, xMax, yMax; } atm_bbox_t;

/* ─── Color ─── */
typedef struct { uint8_t r, g, b, a; } atm_color_t;

/* ─── Outline commands (PostScript Type 1 charstring operators) ─── */
typedef enum {
    ATM_MOVETO = 0,
    ATM_LINETO = 1,
    ATM_CURVETO = 2,   /* cubic bezier */
    ATM_CLOSEPATH = 3,
} atm_path_cmd_t;

typedef struct {
    atm_path_cmd_t cmd;
    atm_fixed_t x, y;       /* endpoint */
    atm_fixed_t cx1, cy1;   /* first control point (for CURVETO) */
    atm_fixed_t cx2, cy2;   /* second control point (for CURVETO) */
} atm_path_elem_t;

/* ─── Glyph outline ─── */
typedef struct {
    int num_elements;
    int capacity;
    atm_path_elem_t *elements;
    atm_bbox_t bbox;
    int glyph_index;
    atm_fixed_t advance_width;
} atm_glyph_outline_t;

/* ─── Anti-aliased glyph bitmap ─── */
typedef struct {
    int width;
    int height;
    int pitch;        /* bytes per row */
    uint8_t *buf;     /* alpha buffer (8-bit per pixel coverage) */
    int x_offset;     /* bearing x */
    int y_offset;     /* bearing y */
    int advance_x;    /* advance width in fixed point */
} atm_glyph_bitmap_t;

/* ─── Font info (NEWTEXTMETRIC-style) ─── */
typedef struct {
    char family_name[256];
    char postscript_name[256];
    char menu_name[256];
    atm_fixed_t units_per_em;
    atm_fixed_t ascender;
    atm_fixed_t descender;
    atm_fixed_t line_gap;
    atm_fixed_t cap_height;
    atm_fixed_t x_height;
    atm_fixed_t max_advance_width;
    atm_bbox_s16_t font_bbox;
    int num_glyphs;
    int is_fixed_pitch;
    int weight;
    atm_fixed_t italic_angle;
    atm_fixed_t underline_pos;
    atm_fixed_t underline_thickness;
} atm_font_info_t;

/* ─── Engine flags (from ATM.CNF) ─── */
typedef enum {
    ATM_FLAG_ANTIALIAS       = 1 << 0,
    ATM_FLAG_GAMMA_CORRECT   = 1 << 1,
    ATM_FLAG_CACHE_ENABLED   = 1 << 2,
    ATM_FLAG_AUTO_ACTIVATE   = 1 << 3,
    ATM_FLAG_SUBSTITUTION    = 1 << 4,
    ATM_FLAG_BITMAP_FONTS    = 1 << 5,
    ATM_FLAG_DOWNLOAD_FONTS  = 1 << 6,
    ATM_FLAG_GDI_FONTS       = 1 << 7,
} atm_flags_t;

/* ─── Engine configuration ─── */
typedef struct {
    uint32_t flags;
    int cache_size_kb;       /* FontCache in KB (default 256) */
    float gamma;             /* gamma value for DIBEngineGammaWorkaround */
    int antialias_level;     /* 0=off, 1=2x2, 2=4x4 supersample */
} atm_config_t;

/* ─── PFB segment types ─── */
#define PFB_SEG_ASCII  1
#define PFB_SEG_BINARY 2
#define PFB_SEG_EOF    3

/* ─── Utility macros ─── */
#define ATM_MIN(a,b) ((a) < (b) ? (a) : (b))
#define ATM_MAX(a,b) ((a) > (b) ? (a) : (b))
#define ATM_CLAMP(x,lo,hi) ATM_MIN(ATM_MAX((x),(lo)),(hi))
#define ATM_ABS(x) ((x) < 0 ? -(x) : (x))

#ifdef __cplusplus
}
#endif

#endif /* ATM_TYPES_H */
