#include "ttx.h"
#include "util.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define MAX_PAGES 3000
#define SUB_HISTORY 200
#define CYRILLIC_RU 0x24

/* ---------- биты ---------- */
static uint8_t REV[256];
static int8_t H84[256];
static int tables_ready;

static int bits(unsigned v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }
static int enc84(int n) {
    int d1 = n & 1, d2 = (n >> 1) & 1, d3 = (n >> 2) & 1, d4 = (n >> 3) & 1;
    int p1 = 1 ^ d1 ^ d3 ^ d4, p2 = 1 ^ d1 ^ d2 ^ d4, p3 = 1 ^ d1 ^ d2 ^ d3;
    int low = p1 | (d1 << 1) | (p2 << 2) | (d2 << 3) | (p3 << 4) | (d3 << 5) | (d4 << 7);
    int p4 = (bits((unsigned)low) + 1) & 1;   /* всего единиц — нечётное */
    return low | (p4 << 6);
}
static void tables(void) {
    if (tables_ready) return;
    for (int b = 0; b < 256; b++) {
        int r = 0;
        for (int i = 0; i < 8; i++) if (b & (1 << i)) r |= 0x80 >> i;
        REV[b] = (uint8_t)r;
        H84[b] = -1;
        for (int n = 0; n < 16; n++) if (bits((unsigned)(b ^ enc84(n))) <= 1) H84[b] = (int8_t)n;
    }
    tables_ready = 1;
}
uint8_t ttx_rev(uint8_t b) { tables(); return REV[b]; }
int ttx_ham84(uint8_t b) { tables(); return H84[b]; }

static int ham2418(const uint8_t *d, int p) {
    int b0 = d[p], b1 = d[p + 1] & 0x7F, b2 = d[p + 2] & 0x7F;
    int lo = ((b0 >> 2) & 1) | (((b0 >> 4) & 1) << 1) | (((b0 >> 5) & 1) << 2) | (((b0 >> 6) & 1) << 3);
    return lo | (b1 << 4) | (b2 << 11);
}

/* ---------- наборы G0 ---------- */
static const int LATIN_NATIONAL[13] = {0x23, 0x24, 0x40, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x7B, 0x7C, 0x7D, 0x7E};
static const char *const LATIN_OPTIONS[7] = {
    "£$@←½→↑#―¼‖¾÷",   /* английский */
    "#$§ÄÖÜ^_°äöüß",   /* немецкий */
    "#¤ÉÄÖÅÜ_éäöåü",   /* шведский / финский */
    "£$é°ç→↑#ùàòèì",   /* итальянский */
    "éïàëêùî#èâôûç",   /* французский */
    "ç$¡áéíóú¿üñèà",   /* испанский / португальский */
    "#ůčťžýířéáěúš",   /* чешский / словацкий */
};
static const char *CYR_UP = "ЮАБЦДЕФГХИЙКЛМНОПЯРСТУЖВЬЪЗШЭЩЧ";   /* 0x40…0x5E */
static const char *CYR_LOW = "юабцдефгхийклмнопярстужвьъзшэщч";  /* 0x60…0x7E */

static uint32_t nth_cp(const char *s, int k) {
    uint32_t c = 0;
    for (int i = 0; i <= k; i++) { c = u8_next(&s); if (!c) return 0x20; }
    return c;
}

uint32_t ttx_glyph(int c, int charset) {
    if (c == 0x7F) return 0x25A0;
    if (charset == 0x24 || charset == 0x25) {   /* кириллица (украинский — по русскому) */
        if (c == 0x26) return 0x44B;            /* ы */
        if (c >= 0x40 && c <= 0x5E) return nth_cp(CYR_UP, c - 0x40);
        if (c == 0x5F) return 0x42B;            /* Ы */
        if (c >= 0x60 && c <= 0x7E) return nth_cp(CYR_LOW, c - 0x60);
        return (uint32_t)c;
    }
    int opt = charset & 7;
    if (opt > 6) opt = 0;
    for (int k = 0; k < 13; k++) if (LATIN_NATIONAL[k] == c) return nth_cp(LATIN_OPTIONS[opt], k);
    return (uint32_t)c;
}

/* ---------- декодер ---------- */
typedef struct { int mag, active, charset, subset; ttx_page p; } builder;
typedef struct { int64_t pts; ttx_page p; } subrec;

struct ttx {
    pthread_mutex_t mu;
    ttx_page *pages; int n, cap;
    builder b[8];
    int mag_charset[8];
    int version, declared, cyr_default;
    int declared_subs[8], ndeclared;
    int sub_number;
    subrec *hist; int hn, hpos;
};

ttx *ttx_new(void) {
    tables();
    ttx *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    pthread_mutex_init(&t->mu, NULL);
    t->hist = calloc(SUB_HISTORY, sizeof *t->hist);
    for (int i = 0; i < 8; i++) t->mag_charset[i] = -1;
    return t;
}
void ttx_free(ttx *t) {
    if (!t) return;
    pthread_mutex_destroy(&t->mu);
    free(t->pages);
    free(t->hist);
    free(t);
}
void ttx_reset(ttx *t) {
    pthread_mutex_lock(&t->mu);
    t->n = 0;
    memset(t->b, 0, sizeof t->b);
    for (int i = 0; i < 8; i++) t->mag_charset[i] = -1;
    t->declared = t->cyr_default = t->ndeclared = t->sub_number = 0;
    t->hn = t->hpos = 0;
    t->version++;
    pthread_mutex_unlock(&t->mu);
}

void ttx_declare(ttx *t, const char *lang, const int *sub, int n) {
    static const char *const cyr[] = {"rus", "bul", "ukr", "bel", "srp", "mkd", "mac", "kaz"};
    pthread_mutex_lock(&t->mu);
    t->declared = 1;
    t->cyr_default = 0;
    for (size_t i = 0; i < sizeof cyr / sizeof cyr[0]; i++) if (lang && !strncasecmp(lang, cyr[i], 3)) t->cyr_default = 1;
    t->ndeclared = 0;
    for (int i = 0; i < n && t->ndeclared < 8; i++) if (sub[i] != 0x100) t->declared_subs[t->ndeclared++] = sub[i];   /* «100» часто ошибочно */
    pthread_mutex_unlock(&t->mu);
}
int ttx_declared(ttx *t) { return t->declared; }
int ttx_version(ttx *t) { return t->version; }
int ttx_count(ttx *t) { return t->n; }

static ttx_page *find(ttx *t, int number, int subcode) {
    for (int i = 0; i < t->n; i++) if (t->pages[i].number == number && t->pages[i].subcode == subcode) return &t->pages[i];
    return NULL;
}

static int accept_subtitle(ttx *t, const ttx_page *p) {
    if (!t->sub_number) {
        int ok = (p->flags & TTXF_SUBTITLE) || p->number == 0x888;
        for (int i = 0; i < t->ndeclared && !ok; i++) if (t->declared_subs[i] == p->number) ok = 1;
        if (!ok) return 0;
        t->sub_number = p->number;
    }
    return p->number == t->sub_number;
}

static void finish(ttx *t, int mag, int64_t pts) {
    builder *b = &t->b[mag];
    if (!b->active) return;
    b->active = 0;
    b->p.charset = (uint8_t)(b->charset >= 0 ? b->charset : t->mag_charset[mag] >= 0 ? t->mag_charset[mag] : t->cyr_default ? CYRILLIC_RU : b->subset);
    ttx_page *dst = find(t, b->p.number, b->p.subcode);
    if (!dst) {
        if (t->n >= MAX_PAGES) t->n = 0;   /* «бегущий» код подстраницы копил бы страницы без конца */
        if (t->n == t->cap) {
            int cap = t->cap ? t->cap * 2 : 128;
            ttx_page *np = realloc(t->pages, (size_t)cap * sizeof *np);
            if (!np) return;
            t->pages = np;
            t->cap = cap;
        }
        dst = &t->pages[t->n++];
    }
    *dst = b->p;
    t->version++;
    if (pts >= 0 && accept_subtitle(t, &b->p)) {
        subrec *r = &t->hist[t->hpos];
        r->pts = pts;
        r->p = b->p;
        t->hpos = (t->hpos + 1) % SUB_HISTORY;
        if (t->hn < SUB_HISTORY) t->hn++;
    }
}

static void header(ttx *t, int mag, const uint8_t *d, int64_t pts) {
    int u = ttx_ham84(d[0]), te = ttx_ham84(d[1]), s1 = ttx_ham84(d[2]), s2 = ttx_ham84(d[3]), s3 = ttx_ham84(d[4]),
        s4 = ttx_ham84(d[5]), c7 = ttx_ham84(d[6]), c11 = ttx_ham84(d[7]);
    if (u < 0 || te < 0 || s1 < 0 || s2 < 0 || s3 < 0 || s4 < 0 || c7 < 0 || c11 < 0) { finish(t, mag, pts); return; }
    int flags = (s2 & 8 ? TTXF_ERASE : 0) | (s4 & 4 ? TTXF_NEWSFLASH : 0) | (s4 & 8 ? TTXF_SUBTITLE : 0) | (c7 & 1 ? TTXF_SUPPRESS_HEADER : 0) |
                (c11 & 1 ? TTXF_SERIAL : 0);
    if (flags & TTXF_SERIAL) for (int m = 0; m < 8; m++) finish(t, m, pts);
    else finish(t, mag, pts);
    int number = ((mag == 0 ? 8 : mag) << 8) | (te << 4) | u;
    if ((number & 0xFF) == 0xFF) return;   /* заполнители */
    builder *b = &t->b[mag];
    memset(b, 0, sizeof *b);
    b->active = 1;
    b->mag = mag;
    b->charset = -1;
    b->subset = (c11 >> 1) & 7;
    b->p.number = (uint16_t)number;
    b->p.subcode = (uint16_t)(s1 | ((s2 & 7) << 4) | (s3 << 8) | ((s4 & 3) << 12));
    b->p.flags = (uint16_t)flags;
    if (!(flags & TTXF_ERASE)) {   /* без бита стирания — дополняет прежнюю */
        ttx_page *old = find(t, number, b->p.subcode);
        if (old) {
            memcpy(b->p.rows, old->rows, sizeof old->rows);
            memcpy(b->p.has, old->has, sizeof old->has);
            memcpy(b->p.links, old->links, sizeof old->links);
            b->charset = old->charset;
        }
    }
    for (int i = 0; i < TTX_COLS; i++) b->p.rows[0][i] = d[i] & 0x7F;
    b->p.has[0] = 1;
}

static void fastext(builder *b, const uint8_t *d) {
    for (int k = 0; k < 6; k++) {
        int q = 1 + k * 6, n[6], bad = 0;
        for (int i = 0; i < 6; i++) if ((n[i] = ttx_ham84(d[q + i])) < 0) bad = 1;
        if (bad) continue;
        int mag = b->mag ^ (((n[3] >> 3) & 1) | ((n[5] >> 1) & 6));
        int num = ((mag == 0 ? 8 : mag) << 8) | (n[1] << 4) | n[0];
        b->p.links[k] = (uint16_t)((num & 0xFF) != 0xFF ? num : 0);
    }
}

static void unit(ttx *t, const uint8_t *p, int64_t pts) {
    int a = ttx_ham84(REV[p[0]]), c = ttx_ham84(REV[p[1]]);
    if (a < 0 || c < 0) return;
    int addr = a | (c << 4), mag = addr & 7, row = addr >> 3;
    uint8_t d[40];
    for (int i = 0; i < 40; i++) d[i] = REV[p[2 + i]];
    builder *b = &t->b[mag];
    if (row == 0) header(t, mag, d, pts);
    else if (row <= 24) { if (b->active) { for (int i = 0; i < 40; i++) b->p.rows[row][i] = d[i] & 0x7F; b->p.has[row] = 1; } }
    else if (row == 27) { if (b->active && ttx_ham84(d[0]) == 0) fastext(b, d); }
    else if (row == 28 || row == 29) {
        int des = ttx_ham84(d[0]);
        if (des != 0 && des != 4) return;
        int cs = (ham2418(d, 1) >> 7) & 0x7F;
        if (row == 29) t->mag_charset[mag] = cs;
        else if (b->active && b->charset < 0) b->charset = cs;
    }
}

void ttx_feed(ttx *t, const uint8_t *d, size_t n, int64_t pts) {
    if (n < 3 || d[0] < 0x10 || d[0] > 0x1F) return;
    pthread_mutex_lock(&t->mu);
    size_t i = 1;
    while (i + 2 <= n) {
        int uid = d[i], ul = d[i + 1];
        size_t u = i + 2;
        i = u + (size_t)ul;
        if (i > n) break;
        if ((uid == 0x02 || uid == 0x03) && ul == 44 && d[u + 1] == 0xE4) unit(t, d + u + 2, pts);
    }
    pthread_mutex_unlock(&t->mu);
}

static int cmp_u16(const void *a, const void *b) { return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b; }

int ttx_get(ttx *t, int number, int sub, ttx_page *out, int *nsub) {
    pthread_mutex_lock(&t->mu);
    uint16_t codes[256];
    int k = 0;
    for (int i = 0; i < t->n && k < 256; i++) if (t->pages[i].number == number) codes[k++] = t->pages[i].subcode;
    if (nsub) *nsub = k;
    int rc = -1;
    if (k) {
        qsort(codes, (size_t)k, sizeof codes[0], cmp_u16);
        int want = sub < 0 || sub >= k ? k - 1 : sub;
        ttx_page *p = find(t, number, codes[want]);
        if (p) { *out = *p; rc = 0; }
    }
    pthread_mutex_unlock(&t->mu);
    return rc;
}

int ttx_neighbour(ttx *t, int number, int dir) {
    pthread_mutex_lock(&t->mu);
    int best = 0, wrap = 0;
    for (int i = 0; i < t->n; i++) {
        int x = t->pages[i].number;
        if (dir > 0) {
            if (x > number && (!best || x < best)) best = x;
            if (!wrap || x < wrap) wrap = x;
        } else {
            if (x < number && x > best) best = x;
            if (x > wrap) wrap = x;
        }
    }
    pthread_mutex_unlock(&t->mu);
    return best ? best : wrap;
}

int ttx_subtitle_at(ttx *t, int64_t pts, ttx_page *out) {
    pthread_mutex_lock(&t->mu);
    int rc = -1;
    int64_t bt = INT64_MIN;
    for (int i = 0; i < t->hn; i++) {
        subrec *r = &t->hist[i];
        if (r->pts <= pts && r->pts > bt) { bt = r->pts; *out = r->p; rc = 0; }
    }
    pthread_mutex_unlock(&t->mu);
    return rc;
}
int ttx_has_subtitles(ttx *t) { return t->hn > 0; }
int ttx_subtitle_number(ttx *t) { return t->sub_number; }
int ttx_subtitles_known(ttx *t) { return t->sub_number || t->ndeclared > 0; }
int ttx_numbers(ttx *t, uint16_t *out, int max) {
    pthread_mutex_lock(&t->mu);
    int n = 0;
    for (int i = 0; i < t->n; i++) {
        uint16_t x = t->pages[i].number;
        int k = 0;
        while (k < n && out[k] < x) k++;
        if (k < n && out[k] == x) continue;
        if (n == max) { if (k == n) continue; n--; }
        memmove(out + k + 1, out + k, (size_t)(n - k) * sizeof *out);
        out[k] = x;
        n++;
    }
    pthread_mutex_unlock(&t->mu);
    return n;
}

/* ---------- раскладка (последовательные атрибуты уровня 1) ---------- */
void ttx_layout(const ttx_page *p, const uint8_t *row0, ttx_grid *g) {
    for (int i = 0; i < TTX_COLS * TTX_ROWS; i++) { g->ch[i] = 0x20; g->attr[i] = 7; }
    int skip = -1;
    for (int r = 0; r < TTX_ROWS; r++) {
        const uint8_t *data = r == 0 ? row0 : p->has[r] ? p->rows[r] : NULL;
        if (r == skip) {   /* нижняя половина двойной высоты */
            for (int c = 0; c < TTX_COLS; c++) {
                int up = (r - 1) * TTX_COLS + c, i = r * TTX_COLS + c;
                if (g->attr[up] & TA_DH) { g->ch[i] = g->ch[up]; g->attr[i] = (uint16_t)((g->attr[up] & ~TA_DH) | TA_DH_BOTTOM); }
                else { g->ch[i] = 0x20; g->attr[i] = (uint16_t)((((g->attr[up] >> 3) & 7) << 3) | 7); }
            }
            skip = -1;
            continue;
        }
        if (!data) continue;
        int fg = 7, bg = 0, mosaic = 0, sep = 0, hold = 0, held = 0x20, held_sep = 0, dh = 0, dw = 0, flash = 0, conceal = 0, box = 0, prev = 0,
            any_dh = 0, right_half = 0;
        for (int c = 0; c < TTX_COLS; c++) {
            int code = data[c] & 0x7F, shown_mosaic = 0, shown_sep = sep;
            uint32_t shown;
            if (code >= 0x20) {
                if (mosaic && (code < 0x40 || code >= 0x60)) { shown = (uint32_t)code; shown_mosaic = 1; held = code; held_sep = sep; }
                else shown = ttx_glyph(code, r == 0 && c < 8 ? 0 : p->charset);   /* «P100» — латиница */
            } else {
                switch (code) {   /* «Set-At» */
                case 0x09: flash = 0; break;
                case 0x0C: dh = 0; dw = 0; break;
                case 0x18: conceal = 1; break;
                case 0x19: sep = 0; break;
                case 0x1A: sep = 1; break;
                case 0x1C: bg = 0; break;
                case 0x1D: bg = fg; break;
                case 0x1E: hold = 1; break;
                }
                if (hold && mosaic) { shown = (uint32_t)held; shown_mosaic = 1; shown_sep = held_sep; }
                else { shown = 0x20; shown_sep = sep; }
            }
            int i = r * TTX_COLS + c;
            /* двойная ширина: знак на эту и правую ячейку, у правой своего знака нет */
            int wide = dw && !right_half && code >= 0x20 && c < TTX_COLS - 1;
            g->ch[i] = shown;
            g->attr[i] = (uint16_t)((fg & 7) | ((bg & 7) << 3) | (shown_mosaic ? TA_MOSAIC : 0) | (shown_mosaic && shown_sep ? TA_SEPARATED : 0) |
                                    (dh ? TA_DH : 0) | (flash ? TA_FLASH : 0) | (conceal ? TA_CONCEAL : 0) | (box ? TA_BOX : 0) |
                                    (wide ? TA_DW : 0) | (right_half ? TA_DW_RIGHT : 0));
            right_half = wide;
            if (dh) any_dh = 1;
            if (code < 0x20) {   /* «Set-After» */
                if (code <= 0x07) { fg = code; mosaic = 0; held = 0x20; conceal = 0; }
                else if (code >= 0x10 && code <= 0x17) { fg = code - 0x10; mosaic = 1; conceal = 0; }
                else if (code == 0x0A) { if (prev == 0x0A) box = 0; }
                else if (code == 0x0B) { if (prev == 0x0B) box = 1; }
                else if (code == 0x08) flash = 1;
                else if (code == 0x0D) { dh = 1; dw = 0; }
                else if (code == 0x0E) { dw = 1; dh = 0; }
                else if (code == 0x0F) { dh = 1; dw = 1; }
                else if (code == 0x1F) hold = 0;
            }
            prev = code;
        }
        if (any_dh && r < TTX_ROWS - 1) skip = r + 1;
    }
}

void ttx_header_row(int number, const ttx_page *p, const char *typed, uint8_t out[TTX_COLS]) {
    memset(out, 0x20, TTX_COLS);
    char label[8];
    if (typed && *typed) {
        snprintf(label, sizeof label, "P%s", typed);
        for (size_t i = strlen(label); i < 4; i++) label[i] = '-';
        label[4] = 0;
    } else snprintf(label, sizeof label, "P%03X", number);
    for (int i = 0; label[i] && i < 6; i++) out[1 + i] = (uint8_t)label[i];
    if (p && p->has[0]) memcpy(out + 8, p->rows[0] + 8, TTX_COLS - 8);
}

size_t ttx_subtitle_text(const ttx_grid *g, char *out, size_t cap) {
    int boxes = 0;
    for (int i = 0; i < TTX_COLS * TTX_ROWS && !boxes; i++) if (g->attr[i] & TA_BOX) boxes = 1;
    size_t o = 0;
    if (cap) out[0] = 0;
    for (int r = 1; r < TTX_ROWS; r++) {
        char line[TTX_COLS * 4 + 1];
        size_t l = 0;
        for (int c = 0; c < TTX_COLS; c++) {
            int i = r * TTX_COLS + c;
            uint32_t ch = g->ch[i];
            int vis = (!boxes || (g->attr[i] & TA_BOX)) && !(g->attr[i] & (TA_DH_BOTTOM | TA_CONCEAL));
            if (g->attr[i] & TA_DW_RIGHT) continue;   /* правая половина двойной ширины — без своего знака */
            if (!vis || (g->attr[i] & TA_MOSAIC)) ch = 0x20;
            l += (size_t)u8_put(line + l, ch);
        }
        line[l] = 0;
        trim(line);
        /* пробелы внутри строки — по одному */
        char *w = line;
        for (char *q = line; *q; q++) if (!(*q == ' ' && w > line && w[-1] == ' ')) *w++ = *q;
        *w = 0;
        if (!line[0]) continue;
        size_t n = strlen(line);
        if (o + n + 2 >= cap) break;
        if (o) out[o++] = '\n';
        memcpy(out + o, line, n);
        o += n;
        out[o] = 0;
    }
    return o;
}
