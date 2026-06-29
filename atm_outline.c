#include "atm_outline.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ─── Type 1 CharString operator codes ─── */
#define CS_HSBW      13
#define CS_ENDCHAR   14
#define CS_HSTEM      1
#define CS_VSTEM      3
#define CS_VSTEM2     2
#define CS_VMOVETO    4
#define CS_RLINETO    5
#define CS_HLINETO    6
#define CS_VLINETO    7
#define CS_RRCURVETO  8
#define CS_CLOSEPATH  9
#define CS_CALLSUBR  10
#define CS_RETURN    11
#define CS_ESCAPE    12
#define CS_RMOVETO   21
#define CS_HMOVETO   22
#define CS_VMOVETO2   4
#define CS_SETCURRENTPOINT 0x0c21
#define CS_CALLGSUBR 0x0c1d
#define CS_CALLOTHERSUBR 0x0c10
#define CS_POP 0x0c11

/* ─── Stack-based interpreter ─── */
#define MAX_STACK 256
#define MAX_CALL 32

typedef struct {
    atm_fixed_t stack[MAX_STACK];
    int sp;
    atm_fixed_t x, y;
    atm_glyph_outline_t *outline;
    int error;
    const uint8_t *const *subrs;
    const int *subrs_lens;
    int num_subrs;
    atm_fixed_t other_stack[MAX_STACK];
    int other_sp;
} cs_state_t;

static void cs_push(cs_state_t *st, atm_fixed_t v) {
    if (st->sp < MAX_STACK) st->stack[st->sp++] = v;
    else st->error = 1;
}

static atm_fixed_t cs_pop(cs_state_t *st) {
    if (st->sp > 0) return st->stack[--st->sp];
    st->error = 1;
    return 0;
}

static void cs_other_push(cs_state_t *st, atm_fixed_t v) {
    if (st->other_sp < MAX_STACK) st->other_stack[st->other_sp++] = v;
    else st->error = 1;
}

static atm_fixed_t cs_other_pop(cs_state_t *st) {
    if (st->other_sp > 0) return st->other_stack[--st->other_sp];
    return 0; /* no error — empty OtherSubr stack is valid */
}

static void cs_add_elem(cs_state_t *st, atm_path_elem_t elem) {
    atm_glyph_outline_t *o = st->outline;
    if (o->num_elements >= o->capacity) {
        o->capacity = o->capacity ? o->capacity * 2 : 128;
        o->elements = realloc(o->elements, o->capacity * sizeof(atm_path_elem_t));
    }
    if (o->elements)
        o->elements[o->num_elements++] = elem;
}

/* ─── Execute a charstring program (used for both main and subrs) ─── */
static void execute_program(cs_state_t *st, const uint8_t *p, const uint8_t *end) {
    while (p < end && !st->error) {
        int b = *p++;

        if (b >= 32) {
            atm_fixed_t val = 0;
            if (b <= 246) {
                val = ATM_INT_TO_FIXED((int)b - 139);
            } else if (b <= 250) {
                if (p >= end) break;
                val = ATM_INT_TO_FIXED(((b - 247) * 256 + *p++ + 108));
            } else if (b <= 254) {
                if (p >= end) break;
                val = ATM_INT_TO_FIXED((-(b - 251) * 256 - *p++ - 108));
            } else if (b == 255) {
                if (p + 4 > end) break;
                int32_t v = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
                val = ATM_INT_TO_FIXED(v);
                p += 4;
            }
            cs_push(st, val);
            continue;
        }

        if (b == CS_ESCAPE) {
            if (p >= end) break;
            int b2 = *p++;
            b = 0x0c00 | b2;
        }

        switch (b) {
        case 30: /* VSTEM3 – 3 vertical stem hints, pop 6 */
        case 31: /* HSTEM3 – 3 horizontal stem hints, pop 6 */
            for (int i = 0; i < 6 && st->sp > 0; i++) cs_pop(st);
            break;
        case CS_HSBW: {
            atm_fixed_t sbx = cs_pop(st);
            atm_fixed_t wx  = cs_pop(st);
            st->x = sbx;
            (void)wx;
            break;
        }
        case CS_HSTEM:
        case CS_VSTEM:
        case CS_VSTEM2: {
            while (st->sp >= 2) {
                cs_pop(st); cs_pop(st);
            }
            break;
        }
        case CS_RMOVETO: {
            atm_fixed_t dy = cs_pop(st);
            atm_fixed_t dx = cs_pop(st);
            st->x += dx; st->y += dy;
            atm_path_elem_t elem = { .cmd = ATM_MOVETO, .x = st->x, .y = st->y };
            cs_add_elem(st, elem);
            break;
        }
        case CS_HMOVETO: {
            atm_fixed_t dx = cs_pop(st);
            st->x += dx;
            atm_path_elem_t elem = { .cmd = ATM_MOVETO, .x = st->x, .y = st->y };
            cs_add_elem(st, elem);
            break;
        }
        case CS_VMOVETO: {
            atm_fixed_t dy = cs_pop(st);
            st->y += dy;
            atm_path_elem_t elem = { .cmd = ATM_MOVETO, .x = st->x, .y = st->y };
            cs_add_elem(st, elem);
            break;
        }
        case CS_RLINETO: {
            int n = st->sp / 2;
            atm_fixed_t pairs[32][2];
            if (n > 32) n = 32;
            for (int i = n - 1; i >= 0; i--) {
                pairs[i][1] = cs_pop(st);
                pairs[i][0] = cs_pop(st);
            }
            for (int i = 0; i < n; i++) {
                st->x += pairs[i][0]; st->y += pairs[i][1];
                atm_path_elem_t elem = { .cmd = ATM_LINETO, .x = st->x, .y = st->y };
                cs_add_elem(st, elem);
            }
            break;
        }
        case CS_HLINETO: {
            while (st->sp > 0) {
                atm_fixed_t dx = cs_pop(st);
                st->x += dx;
                atm_path_elem_t elem = { .cmd = ATM_LINETO, .x = st->x, .y = st->y };
                cs_add_elem(st, elem);
            }
            break;
        }
        case CS_VLINETO: {
            while (st->sp > 0) {
                atm_fixed_t dy = cs_pop(st);
                st->y += dy;
                atm_path_elem_t elem = { .cmd = ATM_LINETO, .x = st->x, .y = st->y };
                cs_add_elem(st, elem);
            }
            break;
        }
        case CS_RRCURVETO: {
            int n = st->sp / 6;
            atm_fixed_t pts[16][6];
            if (n > 16) n = 16;
            for (int i = n - 1; i >= 0; i--) {
                pts[i][5] = cs_pop(st); pts[i][4] = cs_pop(st);
                pts[i][3] = cs_pop(st); pts[i][2] = cs_pop(st);
                pts[i][1] = cs_pop(st); pts[i][0] = cs_pop(st);
            }
            for (int i = 0; i < n; i++) {
                atm_fixed_t cx1 = st->x + pts[i][0], cy1 = st->y + pts[i][1];
                atm_fixed_t cx2 = cx1 + pts[i][2],    cy2 = cy1 + pts[i][3];
                atm_fixed_t ex  = cx2 + pts[i][4],    ey  = cy2 + pts[i][5];
                atm_path_elem_t elem = {
                    .cmd = ATM_CURVETO,
                    .x = ex, .y = ey,
                    .cx1 = cx1, .cy1 = cy1,
                    .cx2 = cx2, .cy2 = cy2,
                };
                cs_add_elem(st, elem);
                st->x = ex; st->y = ey;
            }
            break;
        }
        case CS_CLOSEPATH: {
            atm_path_elem_t elem = { .cmd = ATM_CLOSEPATH };
            cs_add_elem(st, elem);
            break;
        }
        case CS_ENDCHAR:
            return;
        case CS_RETURN:
            return;
        case CS_CALLSUBR: {
            int idx = ATM_FIXED_TO_INT(cs_pop(st));
            if (idx >= 0 && idx < st->num_subrs && st->subrs[idx]) {
                execute_program(st,
                    st->subrs[idx],
                    st->subrs[idx] + st->subrs_lens[idx]);
            } else {
                st->sp = 0;
            }
            break;
        }
        case CS_CALLGSUBR: {
            /* Global subrs - not supported yet, skip */
            st->sp = 0;
            break;
        }
        case CS_SETCURRENTPOINT: {
            st->y = cs_pop(st);
            st->x = cs_pop(st);
            break;
        }
        case CS_CALLOTHERSUBR: {
            /* OtherSubrs – pop subr index, simulate results */
            int subr_idx = ATM_FIXED_TO_INT(cs_pop(st));
            switch (subr_idx) {
            case 0: /* {} – no-op in uddc2 */ break;
            case 1: /* {} – no-op */ break;
            case 2: /* {} – no-op */ break;
            case 3: /* copy-protection: pop 1 arg, push 3 → other_stack */
                if (st->sp > 0) cs_pop(st);
                cs_other_push(st, ATM_INT_TO_FIXED(3));
                break;
            default: break;
            }
            break;
        }
        case CS_POP: {
            /* Move value from OtherSubr result stack → main stack */
            atm_fixed_t v = cs_other_pop(st);
            cs_push(st, v);
            break;
        }
        default:
            /* Unknown operator 0-31: skip it, don't corrupt stack */
            break;
        }
    }
}

/* ─── Decode charstring (no subrs) ─── */
int atm_decode_charstring(const uint8_t *cs_data, int cs_len,
                          atm_glyph_outline_t *outline) {
    return atm_decode_charstring_subrs(cs_data, cs_len, outline,
                                       NULL, NULL, 0);
}

/* ─── Decode charstring with subroutines ─── */
int atm_decode_charstring_subrs(const uint8_t *cs_data, int cs_len,
                                atm_glyph_outline_t *outline,
                                const uint8_t *const *subrs,
                                const int *subrs_lens, int num_subrs) {
    cs_state_t state;
    memset(&state, 0, sizeof(state));
    state.outline = outline;
    state.subrs = subrs;
    state.subrs_lens = subrs_lens;
    state.num_subrs = num_subrs;
    outline->num_elements = 0;
    outline->capacity = 0;
    outline->elements = NULL;

    execute_program(&state, cs_data, cs_data + cs_len);

    outline->glyph_index = 0;
    outline->bbox.xMin = ATM_INT_TO_FIXED(-50);
    outline->bbox.yMin = ATM_INT_TO_FIXED(-50);
    outline->bbox.xMax = ATM_INT_TO_FIXED(500);
    outline->bbox.yMax = ATM_INT_TO_FIXED(500);
    return state.error ? -1 : outline->num_elements;
}

void atm_outline_scale(atm_glyph_outline_t *outline, atm_fixed_t scale) {
    for (int i = 0; i < outline->num_elements; i++) {
        atm_path_elem_t *e = &outline->elements[i];
        e->x  = ATM_FIXED_MUL(e->x, scale);
        e->y  = ATM_FIXED_MUL(e->y, scale);
        e->cx1 = ATM_FIXED_MUL(e->cx1, scale);
        e->cy1 = ATM_FIXED_MUL(e->cy1, scale);
        e->cx2 = ATM_FIXED_MUL(e->cx2, scale);
        e->cy2 = ATM_FIXED_MUL(e->cy2, scale);
    }
}

void atm_outline_apply_hints(atm_glyph_outline_t *outline) {
    (void)outline;
}

void atm_outline_free(atm_glyph_outline_t *outline) {
    free(outline->elements);
    outline->elements = NULL;
}
