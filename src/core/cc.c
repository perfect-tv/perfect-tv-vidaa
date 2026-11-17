/* Закрытые субтитры CEA-608 / CEA-708 (core/cc.h). */
#include "cc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HIST 48
#define QMAX 32
#define REORDER 27000   /* 0,3 с в метках 90 кГц */

enum { M_POP, M_ROLL, M_PAINT, M_TEXT };
typedef struct { uint32_t ch; uint8_t color, italic, underline; } cell;
typedef struct {
    cell m[2][15][32];
    int disp, mode, roll, row, col, color, italic, underline, changed;
} c608;
typedef struct {
    int defined, visible, rows, cols, av, ah, rel, ap, prow, pcol, priority;
    uint32_t t[15][42];
} win708;
typedef struct { win708 w[8]; int cur, changed; } c708;

struct ccdec {
    struct { int64_t pts; int n; uint8_t t[31 * 3]; } q[QMAX];
    int nq;
    int64_t newest;
    int kind, num;
    int field_ch[2], xds[2], mode4[4];
    uint16_t last_ctl[2];
    unsigned present608;
    uint64_t present708;
    c608 a;
    uint8_t pkt[130];
    int pkt_n, pkt_size;
    c708 b;
    cc_show hist[HIST];
    uint32_t hseq[HIST], seq;
    int hn, hpos;
};

/* ---------- 608: знаки ---------- */
static const uint32_t BASIC[96] = {
    ' ', '!', '"', '#', '$', '%', '&', 0x2019, '(', ')', 0xE1, '+', ',', '-', '.', '/', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ';', '<', '=', '>', '?',
    '@', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '[', 0xE9, ']', 0xED, 0xF3,
    0xFA, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', 0xE7, 0xF7, 0xD1, 0xF1, 0x2588};
static const uint32_t SPECIAL[16] = {0xAE, 0xB0, 0xBD, 0xBF, 0x2122, 0xA2, 0xA3, 0x266A, 0xE0, ' ', 0xE8, 0xE2, 0xEA, 0xEE, 0xF4, 0xFB};
static const uint32_t EXT12[32] = {0xC1, 0xC9, 0xD3, 0xDA, 0xDC, 0xFC, 0x2018, 0xA1, '*', '\'', 0x2014, 0xA9, 0x2120, 0x2022, 0x201C, 0x201D,
                                   0xC0, 0xC2, 0xC7, 0xC8, 0xCA, 0xCB, 0xEB, 0xCE, 0xCF, 0xEF, 0xD4, 0xD9, 0xF9, 0xDB, 0xAB, 0xBB};
static const uint32_t EXT13[32] = {0xC3, 0xE3, 0xCD, 0xCC, 0xEC, 0xD2, 0xF2, 0xD5, 0xF5, '{', '}', '\\', '^', '_', '|', '~',
                                   0xC4, 0xE4, 0xD6, 0xF6, 0xDF, 0xA5, 0xA4, 0x2502, 0xC5, 0xE5, 0xD8, 0xF8, 0x250C, 0x2510, 0x2514, 0x2518};
static const uint32_t COLORS[8] = {0xffebebebu, 0xff4cd964u, 0xff5b8cffu, 0xff4cd9e8u, 0xffff5a5au, 0xffffe14du, 0xffe46cffu, 0xffebebebu};

static int utf8(uint32_t cp, char *o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    o[0] = (char)(0xE0 | cp >> 12);
    o[1] = (char)(0x80 | ((cp >> 6) & 63));
    o[2] = (char)(0x80 | (cp & 63));
    return 3;
}

/* ---------- история показов ---------- */
static void push_show(ccdec *d, const cc_show *s) {
    d->hist[d->hpos] = *s;
    d->hseq[d->hpos] = ++d->seq;
    d->hpos = (d->hpos + 1) % HIST;
    if (d->hn < HIST) d->hn++;
}

/* ---------- 608: декодер выбранного канала ---------- */
static cell (*mem(c608 *a, int displayed))[32] { return a->m[displayed ? a->disp : !a->disp]; }

static void put(c608 *a, uint32_t cp) {
    if (a->mode == M_TEXT) return;
    if (a->row < 0) a->row = 14;
    cell(*m)[32] = mem(a, a->mode != M_POP);
    m[a->row][a->col] = (cell){cp, (uint8_t)a->color, (uint8_t)a->italic, (uint8_t)a->underline};
    if (a->col < 31) a->col++;
    if (a->mode != M_POP) a->changed = 1;
}

static void clear_mem(c608 *a, int displayed) {
    memset(mem(a, displayed), 0, sizeof a->m[0]);
    if (displayed) a->changed = 1;
}

static void roll_up(c608 *a) {
    cell(*m)[32] = mem(a, 1);
    int base = a->row < 0 ? 14 : a->row, top = base - a->roll + 1;
    if (top < 0) top = 0;
    for (int r = 0; r < 15; r++) if (r < top || r > base) memset(m[r], 0, sizeof m[r]);   /* вне окна — нет */
    for (int r = top; r < base; r++) memcpy(m[r], m[r + 1], sizeof m[r]);
    memset(m[base], 0, sizeof m[base]);
    a->col = 0;
    a->changed = 1;
}

static void misc(c608 *a, int c2) {
    switch (c2) {
    case 0x20: a->mode = M_POP; break;                                        /* RCL */
    case 0x21: if (a->col > 0) { a->col--; if (a->row >= 0) mem(a, a->mode != M_POP)[a->row][a->col].ch = 0; if (a->mode != M_POP) a->changed = 1; } break;   /* BS */
    case 0x24: if (a->row >= 0) for (int c = a->col; c < 32; c++) mem(a, a->mode != M_POP)[a->row][c].ch = 0; if (a->mode != M_POP) a->changed = 1; break;   /* DER */
    case 0x25: case 0x26: case 0x27:                                          /* RU2…RU4 */
        if (a->mode != M_ROLL) { clear_mem(a, 1); clear_mem(a, 0); if (a->row < 0) a->row = 14; }
        a->mode = M_ROLL;
        a->roll = c2 - 0x23;
        if (a->row < a->roll - 1) a->row = a->roll - 1;
        break;
    case 0x29: a->mode = M_PAINT; break;                                      /* RDC */
    case 0x2A: case 0x2B: a->mode = M_TEXT; break;                            /* TR, RTD */
    case 0x2C: clear_mem(a, 1); break;                                        /* EDM */
    case 0x2D: if (a->mode == M_ROLL) roll_up(a); else if (a->mode == M_PAINT && a->row < 14) { a->row++; a->col = 0; } break;   /* CR */
    case 0x2E: clear_mem(a, 0); break;                                        /* ENM */
    case 0x2F: a->disp = !a->disp; a->mode = M_POP; a->changed = 1; break;    /* EOC */
    }
}

static void pac(c608 *a, int c1, int c2) {
    static const int base[8] = {10, 0, 2, 11, 13, 4, 6, 8};
    int row = base[c1 & 7] + ((c2 & 0x20) && (c1 & 7) ? 1 : 0);
    if (a->mode == M_ROLL && a->row >= 0 && row != a->row) {   /* окно роликов — на новую нижнюю строку */
        cell(*m)[32] = mem(a, 1);
        cell tmp[4][32];
        int n = a->roll;
        for (int k = 0; k < n; k++) { int r = a->row - n + 1 + k; if (r >= 0) memcpy(tmp[k], m[r], sizeof tmp[k]); else memset(tmp[k], 0, sizeof tmp[k]); }
        memset(m, 0, sizeof a->m[0]);
        if (row < n - 1) row = n - 1;
        for (int k = 0; k < n; k++) memcpy(m[row - n + 1 + k], tmp[k], sizeof tmp[k]);
        a->changed = 1;
    }
    a->row = row;
    int attr = c2 & 0x1F;
    a->underline = attr & 1;
    if (attr < 0x10) { int c = attr >> 1; a->italic = c == 7; a->color = c == 7 ? 0 : c; a->col = 0; }
    else { a->italic = 0; a->color = 0; a->col = ((attr - 0x10) >> 1) * 4; }
}

/* пара байт поля f (0 / 1) */
static void pair608(ccdec *d, int f, uint8_t b1, uint8_t b2) {
    b1 &= 0x7F;
    b2 &= 0x7F;
    if (!b1 && !b2) return;
    c608 *a = &d->a;
    if (b1 >= 0x10 && b1 <= 0x1F) {
        uint16_t pr = (uint16_t)(b1 << 8 | b2);
        if (d->last_ctl[f] == pr) { d->last_ctl[f] = 0; return; }   /* команды передаются дважды */
        d->last_ctl[f] = pr;
        d->xds[f] = 0;
        int ch = (b1 & 0x08) ? 1 : 0, c1 = b1 & ~0x08, cc = f * 2 + ch + 1;
        d->field_ch[f] = ch;
        if ((c1 == 0x14 || c1 == 0x15) && b2 >= 0x20 && b2 <= 0x2F) {
            if (b2 == 0x2A || b2 == 0x2B) d->mode4[cc - 1] = 1;
            else if (b2 == 0x20 || (b2 >= 0x25 && b2 <= 0x27) || b2 == 0x29) d->mode4[cc - 1] = 0;
        }
        if (d->kind != CC_608 || d->num != cc) return;
        if ((c1 == 0x14 || c1 == 0x15) && b2 >= 0x20 && b2 <= 0x2F) misc(a, b2);
        else if (c1 == 0x17 && b2 >= 0x21 && b2 <= 0x23) { a->col += b2 - 0x20; if (a->col > 31) a->col = 31; }
        else if (c1 == 0x11 && b2 >= 0x20 && b2 <= 0x2F) {   /* смена цвета посреди строки — пробел */
            int c = (b2 - 0x20) >> 1;
            a->underline = b2 & 1;
            a->italic = c == 7;
            if (c != 7) a->color = c;
            put(a, ' ');
        } else if (c1 == 0x11 && b2 >= 0x30 && b2 <= 0x3F) put(a, SPECIAL[b2 - 0x30]);
        else if ((c1 == 0x12 || c1 == 0x13) && b2 >= 0x20 && b2 <= 0x3F) {   /* расширенный — вместо предыдущего */
            if (a->col > 0) a->col--;
            put(a, (c1 == 0x12 ? EXT12 : EXT13)[b2 - 0x20]);
        } else if (b2 >= 0x40) pac(a, c1, b2);
        return;
    }
    d->last_ctl[f] = 0;
    if (b1 < 0x10) { if (f == 1) d->xds[f] = b1 != 0x0F; return; }   /* XDS (поле 2) — не субтитры */
    if (d->xds[f]) return;
    int cc = f * 2 + d->field_ch[f] + 1;
    if (!d->mode4[cc - 1]) d->present608 |= 1u << (cc - 1);
    if (d->kind != CC_608 || d->num != cc) return;
    put(a, BASIC[b1 - 0x20]);
    if (b2 >= 0x20) put(a, BASIC[b2 - 0x20]);
}

static void snap608(ccdec *d, int64_t pts) {
    c608 *a = &d->a;
    if (!a->changed) return;
    a->changed = 0;
    cc_show s;
    memset(&s, 0, sizeof s);
    s.pts = pts;
    cell(*m)[32] = mem(a, 1);
    for (int r = 0; r < 15 && s.n < 16; r++) {
        int c0 = -1, c1 = -1;
        for (int c = 0; c < 32; c++) if (m[r][c].ch && m[r][c].ch != ' ') { if (c0 < 0) c0 = c; c1 = c; }
        if (c0 < 0) continue;
        cc_row *o = &s.row[s.n++];
        o->x = c0 / 32.0f;
        o->y = r / 15.0f;
        o->color = COLORS[m[r][c0].color & 7];
        o->italic = m[r][c0].italic;
        o->underline = m[r][c0].underline;
        size_t k = 0;
        for (int c = c0; c <= c1 && k + 4 < sizeof o->text; c++) k += (size_t)utf8(m[r][c].ch ? m[r][c].ch : ' ', o->text + k);
        o->text[k] = 0;
    }
    push_show(d, &s);
}

/* ---------- 708 ---------- */
static uint32_t g2(int c) {
    switch (c) {
    case 0x20: case 0x21: return ' ';
    case 0x25: return 0x2026;
    case 0x2A: return 0x160;
    case 0x2C: return 0x152;
    case 0x30: return 0x2588;
    case 0x31: return 0x2018;
    case 0x32: return 0x2019;
    case 0x33: return 0x201C;
    case 0x34: return 0x201D;
    case 0x35: return 0x2022;
    case 0x39: return 0x2122;
    case 0x3A: return 0x161;
    case 0x3C: return 0x153;
    case 0x3D: return 0x2120;
    case 0x3F: return 0x178;
    case 0x76: return 0x215B;
    case 0x77: return 0x215C;
    case 0x78: return 0x215D;
    case 0x79: return 0x215E;
    default: return '_';
    }
}

static void w_clear(win708 *w) { memset(w->t, 0, sizeof w->t); w->prow = w->pcol = 0; }

static void w_put(c708 *b, uint32_t cp) {
    win708 *w = &b->w[b->cur];
    if (!w->defined) return;
    if (w->prow < w->rows && w->pcol < w->cols) w->t[w->prow][w->pcol] = cp;
    if (w->pcol < w->cols) w->pcol++;
    if (w->visible) b->changed = 1;
}

static void w_cr(c708 *b) {
    win708 *w = &b->w[b->cur];
    if (!w->defined) return;
    w->pcol = 0;
    if (w->prow + 1 < w->rows) w->prow++;
    else {   /* последняя строка — окно сдвигается вверх */
        memmove(w->t[0], w->t[1], sizeof w->t[0] * (size_t)(w->rows - 1));
        memset(w->t[w->rows - 1], 0, sizeof w->t[0]);
    }
    if (w->visible) b->changed = 1;
}

static void block708(c708 *b, const uint8_t *p, int n) {
    for (int i = 0; i < n; i++) {
        int c = p[i];
        win708 *w = &b->w[b->cur];
        if (c == 0x10) {   /* EXT1 */
            if (++i >= n) break;
            int e = p[i];
            if (e < 0x08) {}
            else if (e < 0x10) i += 1;
            else if (e < 0x18) i += 2;
            else if (e < 0x20) i += 3;
            else if (e < 0x80) w_put(b, g2(e));
            else if (e < 0x88) i += 4;
            else if (e < 0x90) i += 5;
            else if (e < 0xA0) { if (i + 1 < n) i += 1 + (p[i + 1] & 0x3F); }   /* переменной длины */
            else w_put(b, e == 0xA0 ? 0x33C4 : '_');
            continue;
        }
        if (c < 0x20) {
            if (c == 0x08) { if (w->defined && w->pcol > 0) { w->pcol--; w->t[w->prow][w->pcol] = 0; if (w->visible) b->changed = 1; } }
            else if (c == 0x0C) { if (w->defined) { w_clear(w); if (w->visible) b->changed = 1; } }
            else if (c == 0x0D) w_cr(b);
            else if (c == 0x0E) { if (w->defined) { memset(w->t[w->prow], 0, sizeof w->t[0]); w->pcol = 0; if (w->visible) b->changed = 1; } }
            else if (c >= 0x11 && c <= 0x17) i += 1;
            else if (c >= 0x18) i += 2;
            continue;
        }
        if (c < 0x80) { w_put(b, c == 0x7F ? 0x266A : (uint32_t)c); continue; }
        if (c >= 0xA0) { w_put(b, (uint32_t)c); continue; }
        /* C1 */
        if (c <= 0x87) { b->cur = c - 0x80; continue; }
        if (c >= 0x88 && c <= 0x8C) {
            if (++i >= n) break;
            int m = p[i];
            for (int k = 0; k < 8; k++) {
                if (!(m & (1 << k))) continue;
                win708 *x = &b->w[k];
                int was = x->defined && x->visible;
                if (c == 0x88) { if (x->defined) memset(x->t, 0, sizeof x->t); }
                else if (c == 0x89) x->visible = 1;
                else if (c == 0x8A) x->visible = 0;
                else if (c == 0x8B) x->visible = !x->visible;
                else { x->defined = x->visible = 0; }
                if (was || (x->defined && x->visible)) b->changed = 1;
            }
            continue;
        }
        if (c == 0x8D) { i += 1; continue; }
        if (c == 0x8E) continue;
        if (c == 0x8F) { memset(b->w, 0, sizeof b->w); b->changed = 1; continue; }
        if (c == 0x90) { i += 2; continue; }
        if (c == 0x91) { i += 3; continue; }
        if (c == 0x92) {   /* SPL */
            if (i + 2 >= n) break;
            if (w->defined) { w->prow = p[i + 1] & 0x0F; w->pcol = p[i + 2] & 0x3F; if (w->prow >= w->rows) w->prow = w->rows - 1; }
            i += 2;
            continue;
        }
        if (c == 0x97) { i += 4; continue; }
        if (c >= 0x98) {   /* DFx */
            if (i + 6 >= n) break;
            const uint8_t *q = p + i + 1;
            win708 *x = &b->w[c - 0x98];
            int fresh = !x->defined;
            x->defined = 1;
            x->priority = q[0] & 7;
            x->visible = (q[0] & 0x20) != 0;
            x->rel = (q[1] & 0x80) != 0;
            x->av = q[1] & 0x7F;
            x->ah = q[2];
            x->ap = q[3] >> 4;
            x->rows = (q[3] & 0x0F) + 1;
            x->cols = (q[4] & 0x3F) + 1;
            if (x->rows > 15) x->rows = 15;
            if (x->cols > 42) x->cols = 42;
            if (fresh) w_clear(x);
            if (x->prow >= x->rows) x->prow = x->rows - 1;
            b->cur = c - 0x98;
            b->changed = 1;
            i += 6;
            continue;
        }
    }
}

static void snap708(ccdec *d, int64_t pts) {
    c708 *b = &d->b;
    if (!b->changed) return;
    b->changed = 0;
    cc_show s;
    memset(&s, 0, sizeof s);
    s.pts = pts;
    for (int k = 0; k < 8; k++) {
        win708 *w = &b->w[k];
        if (!w->defined || !w->visible) continue;
        float ay = w->rel ? w->av / 100.0f : w->av / 75.0f, ax = w->rel ? w->ah / 100.0f : w->ah / 210.0f;
        int hpos = w->ap % 3, vpos = w->ap / 3;
        float top = ay - (vpos == 1 ? 0.5f : vpos == 2 ? 1.0f : 0) * (w->rows / 15.0f);
        for (int r = 0; r < w->rows && s.n < 16; r++) {
            int c0 = -1, c1 = -1;
            for (int c = 0; c < w->cols; c++) if (w->t[r][c] && w->t[r][c] != ' ') { if (c0 < 0) c0 = c; c1 = c; }
            if (c0 < 0) continue;
            cc_row *o = &s.row[s.n++];
            o->align = hpos;
            o->x = ax + (hpos == 0 ? c0 / 42.0f : 0);
            o->y = top + r / 15.0f;
            if (o->y < 0) o->y = 0;
            if (o->y > 14 / 15.0f) o->y = 14 / 15.0f;
            o->color = COLORS[0];
            size_t q = 0;
            for (int c = c0; c <= c1 && q + 4 < sizeof o->text; c++) q += (size_t)utf8(w->t[r][c] ? w->t[r][c] : ' ', o->text + q);
            o->text[q] = 0;
        }
    }
    push_show(d, &s);
}

static void packet708(ccdec *d) {
    int size = d->pkt_n < d->pkt_size ? d->pkt_n : d->pkt_size, pos = 1;
    while (pos < size) {
        int h = d->pkt[pos++], sn = h >> 5, bs = h & 0x1F;
        if (sn == 7 && bs) { if (pos >= size) break; sn = d->pkt[pos++] & 0x3F; }
        if (!sn || pos + bs > size) break;
        if (bs) d->present708 |= 1ull << sn;
        if (d->kind == CC_708 && d->num == sn) block708(&d->b, d->pkt + pos, bs);
        pos += bs;
    }
    d->pkt_n = 0;
}

/* ---------- кадры по порядку показа ---------- */
static void picture(ccdec *d, int64_t pts, const uint8_t *t, int n) {
    for (int i = 0; i < n; i++) {
        const uint8_t *x = t + 3 * i;
        int valid = x[0] & 4, type = x[0] & 3;
        if (type <= 1) { if (valid) pair608(d, type, x[1], x[2]); continue; }
        if (!valid) continue;
        if (type == 3) {
            if (d->pkt_n) packet708(d);
            int code = x[1] & 0x3F;
            d->pkt_size = code ? code * 2 : 128;
            d->pkt[0] = x[1];
            d->pkt[1] = x[2];
            d->pkt_n = 2;
        } else if (d->pkt_n && d->pkt_n + 2 <= (int)sizeof d->pkt) {
            d->pkt[d->pkt_n++] = x[1];
            d->pkt[d->pkt_n++] = x[2];
        }
        if (d->pkt_n && d->pkt_n >= d->pkt_size) packet708(d);
    }
    if (d->kind == CC_608) snap608(d, pts);
    else if (d->kind == CC_708) snap708(d, pts);
}

static void release(ccdec *d, int all) {
    while (d->nq && (all || d->nq > 24 || d->q[0].pts <= d->newest - REORDER)) {
        picture(d, d->q[0].pts, d->q[0].t, d->q[0].n);
        memmove(d->q, d->q + 1, sizeof d->q[0] * (size_t)(d->nq - 1));
        d->nq--;
    }
}

void cc_feed(ccdec *d, int64_t pts, const uint8_t *t, int n) {
    if (n <= 0) return;
    if (n > 31) n = 31;
    if (pts < 0) pts = d->newest >= 0 ? d->newest : 0;
    /* разрыв (переход потока назад) — что набралось, сразу */
    if (d->newest >= 0 && pts < d->newest - 10 * 90000LL) { release(d, 1); d->newest = pts; }
    if (d->nq == QMAX) { picture(d, d->q[0].pts, d->q[0].t, d->q[0].n); memmove(d->q, d->q + 1, sizeof d->q[0] * (QMAX - 1)); d->nq--; }
    int k = d->nq;
    while (k > 0 && d->q[k - 1].pts > pts) k--;
    memmove(d->q + k + 1, d->q + k, sizeof d->q[0] * (size_t)(d->nq - k));
    d->q[k].pts = pts;
    d->q[k].n = n;
    memcpy(d->q[k].t, t, (size_t)n * 3);
    d->nq++;
    if (pts > d->newest) d->newest = pts;
    release(d, 0);
}

void cc_flush(ccdec *d) { release(d, 1); }

/* ---------- разбор видео ---------- */
static void cc_data(ccdec *d, const uint8_t *p, size_t n, int64_t pts) {
    if (n < 2 || !(p[0] & 0x40)) return;
    size_t cnt = p[0] & 0x1F;
    if (2 + cnt * 3 > n) cnt = (n - 2) / 3;
    cc_feed(d, pts, p + 2, (int)cnt);
}

static void t35(ccdec *d, const uint8_t *p, size_t n, int64_t pts) {
    if (n < 4 || p[0] != 0xB5) return;
    int prov = p[1] << 8 | p[2];
    if (prov == 0x31 && n >= 8 && !memcmp(p + 3, "GA94", 4) && p[7] == 3) cc_data(d, p + 8, n - 8, pts);
    else if (prov == 0x2F && p[3] == 3) cc_data(d, p + 4, n - 4, pts);
}

static void sei(ccdec *d, const uint8_t *p, size_t n, int64_t pts) {
    uint8_t buf[4096];
    size_t m = 0;
    for (size_t i = 0; i < n && m < sizeof buf; i++) {   /* без байтов предотвращения эмуляции */
        if (i >= 2 && p[i] == 3 && p[i - 1] == 0 && p[i - 2] == 0) continue;
        buf[m++] = p[i];
    }
    size_t i = 0;
    while (i + 2 <= m) {
        size_t type = 0, size = 0;
        while (i < m && buf[i] == 0xFF) { type += 255; i++; }
        if (i >= m) break;
        type += buf[i++];
        while (i < m && buf[i] == 0xFF) { size += 255; i++; }
        if (i >= m) break;
        size += buf[i++];
        if (i + size > m) break;
        if (type == 4) t35(d, buf + i, size, pts);
        i += size;
        if (i < m && buf[i] == 0x80) break;
    }
}

void cc_video(ccdec *d, int codec, const uint8_t *es, size_t n, int64_t pts) {
    size_t i = 0;
    while (i + 3 < n) {
        if (es[i] || es[i + 1] || es[i + 2] != 1) { i++; continue; }
        size_t s = i + 3, e = s;
        while (e + 2 < n && !(es[e] == 0 && es[e + 1] == 0 && (es[e + 2] == 1 || (es[e + 2] == 0 && e + 3 < n && es[e + 3] == 1)))) e++;
        if (e + 2 >= n) e = n;
        if (s >= n) break;
        if (codec == CC_VIDEO_H264) {
            int t = es[s] & 0x1F;
            if (t == 6) sei(d, es + s + 1, e - s - 1, pts);
            else if (t == 1 || t == 5) break;   /* дальше — срезы кадра */
        } else if (codec == CC_VIDEO_HEVC) {
            int t = (es[s] >> 1) & 0x3F;
            if (t == 39 && e > s + 2) sei(d, es + s + 2, e - s - 2, pts);
            else if (t < 32) break;
        } else if (codec == CC_VIDEO_MPEG2) {
            int code = es[s];
            if (code == 0xB2 && e - s >= 6 && !memcmp(es + s + 1, "GA94", 4) && es[s + 5] == 3) cc_data(d, es + s + 6, e - s - 6, pts);
            else if (code >= 0x01 && code <= 0xAF) break;
        }
        i = e;
    }
}

/* ---------- общее ---------- */
ccdec *cc_new(void) {
    ccdec *d = calloc(1, sizeof *d);
    if (d) cc_reset(d);
    return d;
}
void cc_free(ccdec *d) { free(d); }
void cc_reset(ccdec *d) {
    int kind = d->kind, num = d->num;
    memset(d, 0, sizeof *d);
    d->newest = -1;
    d->a.row = -1;
    d->kind = kind;
    d->num = num;
}
unsigned cc_present608(const ccdec *d) { return d->present608; }
uint64_t cc_present708(const ccdec *d) { return d->present708; }
void cc_select(ccdec *d, int kind, int num) {
    if (d->kind == kind && d->num == num) return;
    d->kind = kind;
    d->num = num;
    memset(&d->a, 0, sizeof d->a);
    d->a.row = -1;
    memset(&d->b, 0, sizeof d->b);
    d->pkt_n = 0;
    d->hn = d->hpos = 0;
}
int cc_at(const ccdec *d, int64_t pts, cc_show *out) {
    int best = -1;
    for (int i = 0; i < d->hn; i++) {
        const cc_show *s = &d->hist[i];
        if (s->pts <= pts && (best < 0 || s->pts > d->hist[best].pts || (s->pts == d->hist[best].pts && d->hseq[i] > d->hseq[best]))) best = i;
    }
    if (best < 0 || !d->hist[best].n) return 0;
    *out = d->hist[best];
    return 1;
}
