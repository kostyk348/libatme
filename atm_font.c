#include "atm_font.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ─── PFB parsing ─── */

int atm_pfb_load(const char *path, atm_pfb_t *pfb) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *buf = (uint8_t*)malloc(fsize);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, fsize, f);
    fclose(f);

    memset(pfb, 0, sizeof(*pfb));
    size_t pos = 0;

    while (pos + 6 <= (size_t)fsize) {
        if (buf[pos] != 0x80) break;
        int seg_type = buf[pos + 1];
        uint32_t seg_len = *(uint32_t*)(buf + pos + 2);
        pos += 6;

        if (pos + seg_len > (size_t)fsize) break;

        atm_pfb_seg_t *seg = realloc(pfb->segments,
            (pfb->num_segments + 1) * sizeof(atm_pfb_seg_t));
        if (!seg) { free(buf); return -1; }
        pfb->segments = seg;

        atm_pfb_seg_t *s = &pfb->segments[pfb->num_segments];
        s->type = seg_type;
        s->data = (uint8_t*)malloc(seg_len);
        s->len = seg_len;
        if (s->data) memcpy(s->data, buf + pos, seg_len);
        pfb->num_segments++;

        if (seg_type == PFB_SEG_EOF) break;
        pos += seg_len;
    }

    free(buf);
    return 0;
}

void atm_pfb_free(atm_pfb_t *pfb) {
    for (int i = 0; i < pfb->num_segments; i++)
        free(pfb->segments[i].data);
    free(pfb->segments);
    pfb->segments = NULL;
    pfb->num_segments = 0;
}

/* ─── PFM parsing ─── */

int atm_pfm_load(const char *path, atm_pfm_t *pfm) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    memset(pfm, 0, sizeof(*pfm));
    fread(&pfm->header, 1, sizeof(pfm->header), f);
    fclose(f);
    return 0;
}

/* ─── Type 1 font parsing helpers ─── */

static char *find_dict_entry(const char *ascii, int len, const char *key) {
    (void)len;
    /* Key can be followed by space, newline, \r, {, [, or ( */
    const char *delims = " \n\r{([";
    for (int di = 0; delims[di]; di++) {
        char pattern[256];
        snprintf(pattern, sizeof(pattern), "/%s%c", key, delims[di]);
        char *p = strstr(ascii, pattern);
        if (p) return p;
    }
    return NULL;
}

static char *extract_string(const char *start) {
    while (*start && *start != '(') start++;
    if (!*start) return NULL;
    start++;
    const char *end = strchr(start, ')');
    if (!end) return NULL;
    int len = (int)(end - start);
    char *s = (char*)malloc(len + 1);
    if (!s) return NULL;
    memcpy(s, start, len);
    s[len] = 0;
    return s;
}

static int extract_int(const char **pp) {
    const char *start = *pp;
    /* Skip key/value prefix until first delimiter */
    while (*start && *start != ' ' && *start != '{' && *start != '[' && *start != '(')
        start++;
    if (*start) start++;
    while (*start == ' ') start++;
    int val = atoi(start);
    /* Advance past the parsed integer */
    const char *p = start;
    if (*p == '-') p++;
    while (*p >= '0' && *p <= '9') p++;
    *pp = p;
    return val;
}

static atm_fixed_t extract_fixed(const char **pp) {
    const char *start = *pp;
    while (*start && *start != ' ' && *start != '{' && *start != '[' && *start != '(')
        start++;
    if (*start) start++;
    while (*start == ' ') start++;
    float v = atof(start);
    return ATM_FLOAT_TO_FIXED(v);
}

/* ═══════════════════════════════════════════════════
   eexec Decryption (cipher-based key update)
   ═══════════════════════════════════════════════════ */

static uint8_t *eexec_decrypt(const uint8_t *data, int len, int *out_len, uint16_t key) {
    uint8_t *out = malloc(len);
    if (!out) return NULL;

    for (int i = 0; i < len; i++) {
        uint8_t cipher = data[i];
        uint8_t plain = cipher ^ (key >> 8);
        out[i] = plain;
        key = (uint16_t)(((uint16_t)cipher + key) * 52845 + 22719);
    }
    *out_len = len;
    return out;
}

/* ═══════════════════════════════════════════════════
   Parse Subrs and CharStrings from decrypted binary
   Format: dup <index> <len> -| <len bytes> |-
   Or:     dup <index> <len> RD <len bytes>
   
   Strategy: find section markers (/Subrs, CharStrings)
   and limit searching to those regions.
   ═══════════════════════════════════════════════════ */

/* Length-bounded memmem replacement (standalone, no POSIX deps) */
static const char *memfind(const char *haystack, int hs_len,
                           const char *needle, int n_len) {
    if (n_len == 0) return haystack;
    if (n_len > hs_len) return NULL;
    for (int i = 0; i <= hs_len - n_len; i++) {
        if (memcmp(haystack + i, needle, n_len) == 0)
            return haystack + i;
    }
    return NULL;
}

/* Find "end" as a whole word (not inside "endash" etc) */
static const char *find_end_word(const char *p, int len) {
    for (int i = 0; i <= len - 3; i++) {
        if (p[i] == 'e' && p[i+1] == 'n' && p[i+2] == 'd') {
            /* Check it's a word boundary: preceded by whitespace/{{/[( and followed by whitespace/]/) */
            int prev_ok = (i == 0) || p[i-1] <= ' ' || p[i-1] == '{' || p[i-1] == '[';
            int next_ok = (i+3 >= len) || p[i+3] <= ' ' || p[i+3] == '}' || p[i+3] == ']';
            if (prev_ok && next_ok)
                return p + i;
        }
    }
    return NULL;
}

static int find_section(const char *txt, int txt_len,
                        const char *marker, int *start, int *end) {
    int mlen = (int)strlen(marker);
    const char *s = memfind(txt, txt_len, marker, mlen);
    if (!s) return -1;
    *start = (int)(s - txt);
    /* Find next "end" as a whole word after marker */
    const char *e = s + mlen;
    int rem = txt_len - (int)(e - txt);
    const char *endmark = find_end_word(e, rem);
    if (!endmark) *end = txt_len;
    else *end = (int)(endmark - txt) + 3;
    return 0;
}

/* ─── Byte-level parser helpers (null-safe) ─── */

/* Skip whitespace */
static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t'))
        p++;
    return p;
}

/* Parse decimal number, advance p */
static int parse_int(const char **pp, const char *end) {
    const char *p = skip_ws(*pp, end);
    int neg = 0, val = 0;
    if (p < end && *p == '-') { neg = 1; p++; }
    while (p < end && *p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    *pp = p;
    return neg ? -val : val;
}

/* Check if string matches at position p */
static int match_str(const char *p, const char *end, const char *s) {
    int len = (int)strlen(s);
    if (p + len > end) return 0;
    return memcmp(p, s, len) == 0;
}

/* ─── Parse "dup <idx> <len> -| <bytes>" ─── */
/* Returns the position after the data, or NULL on failure.
 * *idx, *length_out are set if found. */
static const char *parse_dup_at(const char *p, const char *end,
                                int *idx, int *length_out,
                                const char **data_start_out) {
    if (!match_str(p, end, "dup ")) return NULL;
    const char *q = p + 4;
    int id = parse_int(&q, end);
    int len = parse_int(&q, end);

    /* Expect -| or RD */
    q = skip_ws(q, end);
    if (q + 1 >= end) return NULL;
    int sep_ok = (q[0] == '-' && q[1] == '|') || (q[0] == 'R' && q[1] == 'D');
    if (!sep_ok) return NULL;
    q += 2;

    /* Data starts here */
    const char *dstart = skip_ws(q, end);
    if (dstart + len > end) return NULL;

    *idx = id;
    *length_out = len;
    *data_start_out = dstart;
    return dstart + len;
}

/* ─── Parse dup entries (Subrs) ─── */
static int parse_dup_entries(const char *txt, int txt_start, int txt_end,
                             uint8_t ***out_data, int **out_lens,
                             int *max_index, int lenIV) {
    int cap = 512;
    uint8_t **data = calloc(cap, sizeof(uint8_t*));
    int *lens = calloc(cap, sizeof(int));
    if (!data || !lens) { free(data); free(lens); return 0; }

    const char *p = txt + txt_start;
    const char *end = txt + txt_end;
    int top = 0;

    while (p < end) {
        int idx, length;
        const char *dstart;
        const char *next = parse_dup_at(p, end, &idx, &length, &dstart);
        if (!next) { p++; continue; }

        if (length > 0 && length < 65536) {
            /* Decrypt with charstring key (4330), then strip lenIV bytes */
            int dec_len;
            uint8_t *decrypted = eexec_decrypt((const uint8_t*)dstart, length, &dec_len, 4330);
            if (!decrypted) { p = next; continue; }
            int skip = (dec_len > lenIV) ? lenIV : 0;
            int actual_len = dec_len - skip;
            if (idx >= cap) {
                cap = idx + 512;
                data = realloc(data, cap * sizeof(uint8_t*));
                lens = realloc(lens, cap * sizeof(int));
            }
            data[idx] = malloc(actual_len);
            if (data[idx]) {
                memcpy(data[idx], decrypted + skip, actual_len);
                lens[idx] = actual_len;
                if (idx + 1 > top) top = idx + 1;
            }
            free(decrypted);
        }

        p = next;
        /* Skip |- if present */
        if (p + 1 < end && p[0] == '|' && p[1] == '-') p += 2;
    }

    *out_data = data;
    *out_lens = lens;
    *max_index = top;
    return top;
}

/* ─── Parse glyph entries (CharStrings: /<name> <len> -|-| <bytes>) ─── */
static int parse_glyph_entries(const char *txt, int txt_start, int txt_end,
                               uint8_t ***out_data, int **out_lens,
                               int **out_ids, char ***out_names,
                               int lenIV) {
    int cap = 256, n = 0;
    uint8_t **data = calloc(cap, sizeof(uint8_t*));
    int *lens = calloc(cap, sizeof(int));
    int *ids = calloc(cap, sizeof(int));
    char **names = calloc(cap, sizeof(char*));
    if (!data || !lens || !ids || !names) {
        free(data); free(lens); free(ids); free(names);
        return 0;
    }

    const char *p = txt + txt_start;
    const char *end = txt + txt_end;

    while (p < end) {
        if (*p != '/') { p++; continue; }

        const char *name_start = p + 1;
        const char *name_end = name_start;
        while (name_end < end && *name_end != ' ' && *name_end != '\n' &&
               *name_end != '\r' && *name_end != '\t')
            name_end++;
        if (name_end >= end) break;

        int name_len = (int)(name_end - name_start);
        const char *q = skip_ws(name_end, end);
        int length = parse_int(&q, end);

        if (length <= 0 || length >= 65536) { p = name_end; continue; }

        q = skip_ws(q, end);
        if (q + 1 >= end) break;
        int sep_ok = (q[0] == '-' && q[1] == '|') || (q[0] == 'R' && q[1] == 'D');
        if (!sep_ok) { p = q + 1; continue; }
        q += 2;

        const char *dstart = skip_ws(q, end);
        if (dstart + length > end) break;

        /* Decrypt with charstring key (4330), then strip lenIV bytes */
        int dec_len;
        uint8_t *decrypted = eexec_decrypt((const uint8_t*)dstart, length, &dec_len, 4330);
        if (!decrypted) { p = dstart + length; continue; }
        int skip = (dec_len > lenIV) ? lenIV : 0;
        int actual_len = dec_len - skip;
        if (n >= cap) {
            cap = n + 256;
            data = realloc(data, cap * sizeof(uint8_t*));
            lens = realloc(lens, cap * sizeof(int));
            ids = realloc(ids, cap * sizeof(int));
            names = realloc(names, cap * sizeof(char*));
        }
        data[n] = malloc(actual_len);
        names[n] = malloc(name_len + 1);
        if (data[n] && names[n]) {
            memcpy(data[n], decrypted + skip, actual_len);
            memcpy(names[n], name_start, name_len);
            names[n][name_len] = 0;
            lens[n] = actual_len;
            ids[n] = n;
            n++;
        }
        free(decrypted);

        p = dstart + length;
        if (p + 1 < end && p[0] == '|' && p[1] == '-') p += 2;
    }

    *out_data = data;
    *out_lens = lens;
    *out_ids = ids;
    *out_names = names;
    return n;
}

/* ═══════════════════════════════════════════════════
   Main font open
   ═══════════════════════════════════════════════════ */

int atm_font_open(const char *pfb_path, const char *pfm_path,
                  atm_font_t *font) {
    memset(font, 0, sizeof(*font));

    if (atm_pfb_load(pfb_path, &font->pfb) != 0)
        return -1;

    if (pfm_path && atm_pfm_load(pfm_path, &font->pfm) != 0) {
        /* PFM is optional for parsing */
    }

    /* Find ASCII segment and extract font info */
    for (int i = 0; i < font->pfb.num_segments; i++) {
        if (font->pfb.segments[i].type == PFB_SEG_ASCII) {
            const char *ascii = (const char*)font->pfb.segments[i].data;
            int ascii_len = font->pfb.segments[i].len;
            font->ascii_data = (char*)malloc(ascii_len + 1);
            if (font->ascii_data) {
                memcpy(font->ascii_data, ascii, ascii_len);
                font->ascii_data[ascii_len] = 0;
                font->ascii_len = ascii_len;
            }
            break;
        }
    }

    /* Parse font info from ASCII segment */
    if (font->ascii_data) {
        char *p;

        /* FontName */
        p = find_dict_entry(font->ascii_data, font->ascii_len, "FontName");
        if (p) {
            char *name = extract_string(p);
            if (name) {
                strncpy(font->info.postscript_name, name, 255);
                free(name);
            }
        }

        /* FullName */
        p = find_dict_entry(font->ascii_data, font->ascii_len, "FullName");
        if (p) {
            char *name = extract_string(p);
            if (name) {
                strncpy(font->info.family_name, name, 255);
                free(name);
            }
        }

        /* FamilyName */
        if (!font->info.family_name[0]) {
            p = find_dict_entry(font->ascii_data, font->ascii_len, "FamilyName");
            if (p) {
                char *name = extract_string(p);
                if (name) {
                    strncpy(font->info.family_name, name, 255);
                    free(name);
                }
            }
        }

        /* FontBBox */
        p = find_dict_entry(font->ascii_data, font->ascii_len, "FontBBox");
        if (p) {
            font->info.font_bbox.xMin = (int16_t)extract_int((const char**)&p);
            font->info.font_bbox.yMin = (int16_t)extract_int((const char**)&p);
            font->info.font_bbox.xMax = (int16_t)extract_int((const char**)&p);
            font->info.font_bbox.yMax = (int16_t)extract_int((const char**)&p);
        }

        /* ItalicAngle, etc */
        p = find_dict_entry(font->ascii_data, font->ascii_len, "ItalicAngle");
        if (p) font->info.italic_angle = extract_fixed((const char**)&p);

        p = find_dict_entry(font->ascii_data, font->ascii_len, "isFixedPitch");
        if (p) font->info.is_fixed_pitch = extract_int((const char**)&p) != 0;

        p = find_dict_entry(font->ascii_data, font->ascii_len, "UnderlinePosition");
        if (p) font->info.underline_pos = extract_fixed((const char**)&p);

        p = find_dict_entry(font->ascii_data, font->ascii_len, "UnderlineThickness");
        if (p) font->info.underline_thickness = extract_fixed((const char**)&p);

        p = find_dict_entry(font->ascii_data, font->ascii_len, "UnitsPerEm");
        font->info.units_per_em = p ? ATM_INT_TO_FIXED(extract_int((const char**)&p))
                                    : ATM_INT_TO_FIXED(1000);

        /* Count glyphs (rough) */
        font->info.num_glyphs = 0;
        p = find_dict_entry(font->ascii_data, font->ascii_len, "CharStrings");
        if (p) {
            /* Move past "CharStrings N dict" */
            int n = 0;
            sscanf(p, "/CharStrings %d dict", &n);
            font->info.num_glyphs = n > 0 ? n : 256;
        }

        /* Parse /lenIV from ASCII section (may be here or in eexec Private) */
        p = find_dict_entry(font->ascii_data, font->ascii_len, "lenIV");
        if (p) font->lenIV = extract_int((const char**)&p);
    }

    /* ═══ eexec decryption + Subrs + CharStrings extraction ═══ */

    for (int i = 0; i < font->pfb.num_segments; i++) {
        if (font->pfb.segments[i].type != PFB_SEG_BINARY) continue;

        uint8_t *bin = font->pfb.segments[i].data;
        int bin_len = font->pfb.segments[i].len;

        /* Decrypt eexec-encrypted data */
        int dec_len;
        uint8_t *dec = eexec_decrypt(bin, bin_len, &dec_len, 55665);
        if (!dec) return -1;

        /* Decrypted data is PostScript text with embedded binary charstrings.
         * Make it null-terminated for string parsing. */
        char *txt = (char*)realloc(dec, dec_len + 1);
        if (!txt) { free(dec); return -1; }
        txt[dec_len] = 0;

        /* Parse /lenIV from Private dictionary (default 4) */
        font->lenIV = 4;
        {
            char *iv = strstr(txt, "/lenIV");
            if (iv) {
                iv += 6;
                while (iv < txt + dec_len && (*iv == ' ' || *iv == '\n' || *iv == '\t')) iv++;
                if (iv < txt + dec_len) font->lenIV = atoi(iv);
            }
        }

        /* Find section boundaries for Subrs and CharStrings */
        int subrs_start = 0, subrs_end = 0;
        int cs_start = 0, cs_end = 0;
        find_section(txt, dec_len, "/Subrs", &subrs_start, &subrs_end);
        find_section(txt, dec_len, "CharStrings", &cs_start, &cs_end);
        /* Parse Subrs */
        font->subrs_data = NULL;
        font->subrs_lens = NULL;
        font->num_subrs = 0;
        if (subrs_start < subrs_end) {
            font->num_subrs = parse_dup_entries(txt, subrs_start, subrs_end,
                                                  &font->subrs_data,
                                                  &font->subrs_lens,
                                                  &font->num_subrs,
                                                  font->lenIV);
        }

        /* Parse CharStrings */
        font->glyph_data = NULL;
        font->glyph_lens = NULL;
        font->glyph_ids = NULL;
        font->glyph_names = NULL;
        font->num_glyphs = 0;
        if (cs_start < cs_end) {
            int parsed = parse_glyph_entries(txt, cs_start, cs_end,
                                              &font->glyph_data,
                                              &font->glyph_lens,
                                              &font->glyph_ids,
                                              &font->glyph_names,
                                              font->lenIV);
            font->num_glyphs = parsed;
            font->info.num_glyphs = parsed;
        }

        /* Widths array (not stored in Type 1 charstrings, use PFM if available) */
        font->widths = NULL;

        free(txt);
        break; /* Only first binary segment */
    }

    return 0;
}

void atm_font_close(atm_font_t *font) {
    free(font->ascii_data);
    font->ascii_data = NULL;

    for (int i = 0; i < font->num_glyphs; i++) {
        free(font->glyph_data[i]);
        free(font->glyph_names[i]);
    }
    free(font->glyph_data);
    free(font->glyph_lens);
    free(font->glyph_ids);
    free(font->glyph_names);
    free(font->widths);
    font->glyph_data = NULL;
    font->glyph_lens = NULL;
    font->glyph_ids = NULL;
    font->glyph_names = NULL;
    font->num_glyphs = 0;

    for (int i = 0; i < font->num_subrs; i++)
        free(font->subrs_data[i]);
    free(font->subrs_data);
    free(font->subrs_lens);
    font->subrs_data = NULL;
    font->subrs_lens = NULL;
    font->num_subrs = 0;

    atm_pfb_free(&font->pfb);
}

/* ─── Adobe StandardEncoding (name→glyph_index map) ─── */
int atm_font_glyph_index(atm_font_t *font, int charcode) {
    if (!font->glyph_names || font->num_glyphs < 1) return -1;

    /* StandardEncoding glyph names for codes 32-126 (ASCII printable) */
    static const char *standard_names[256] = {
        [32]="space",
        [33]="exclam", [34]="quotedbl", [35]="numbersign",
        [36]="dollar", [37]="percent", [38]="ampersand",
        [39]="quoteright", [40]="parenleft", [41]="parenright",
        [42]="asterisk", [43]="plus", [44]="comma",
        [45]="hyphen", [46]="period", [47]="slash",
        [48]="zero", [49]="one", [50]="two", [51]="three",
        [52]="four", [53]="five", [54]="six", [55]="seven",
        [56]="eight", [57]="nine",
        [58]="colon", [59]="semicolon", [60]="less", [61]="equal",
        [62]="greater", [63]="question", [64]="at",
        [65]="A", [66]="B", [67]="C", [68]="D", [69]="E",
        [70]="F", [71]="G", [72]="H", [73]="I", [74]="J",
        [75]="K", [76]="L", [77]="M", [78]="N", [79]="O",
        [80]="P", [81]="Q", [82]="R", [83]="S", [84]="T",
        [85]="U", [86]="V", [87]="W", [88]="X", [89]="Y",
        [90]="Z",
        [91]="bracketleft", [92]="backslash", [93]="bracketright",
        [94]="asciicircum", [95]="underscore",
        [96]="quoteleft",
        [97]="a", [98]="b", [99]="c", [100]="d", [101]="e",
        [102]="f", [103]="g", [104]="h", [105]="i", [106]="j",
        [107]="k", [108]="l", [109]="m", [110]="n", [111]="o",
        [112]="p", [113]="q", [114]="r", [115]="s", [116]="t",
        [117]="u", [118]="v", [119]="w", [120]="x", [121]="y",
        [122]="z",
        [123]="braceleft", [124]="bar", [125]="braceright",
        [126]="asciitilde",
    };

    const char *name = NULL;
    if (charcode >= 0 && charcode < 256)
        name = standard_names[charcode];
    if (!name) return -1;

    /* Linear scan through glyph names (typically small: 229 entries) */
    for (int i = 0; i < font->num_glyphs; i++) {
        if (font->glyph_names[i] && strcmp(font->glyph_names[i], name) == 0)
            return i;
    }
    return -1;
}
