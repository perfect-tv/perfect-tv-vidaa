#include "dvbsub.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MAX_REGIONS 16
#define MAX_CLUTS 8
#define MAX_OBJS 32
#define HIST 16

typedef struct { int obj, x, y; } objref;
typedef struct {
    int id, used, w, h, depth, clut, bg8, bg4, bg2;
    uint8_t *pix;   /* коды пикселей */
    objref obj[MAX_OBJS];
    int nobj;
} region;
typedef struct { int id, used; uint32_t c2[4], c4[16], c8[256]; } clut;

struct dvbsub {
    pthread_mutex_t mu;
    int comp, anc;
    int timeout, pending;
    int64_t set_pts;
    struct { int id, x, y; } preg[MAX_REGIONS];
    int npreg;
    region reg[MAX_REGIONS];
    clut cl[MAX_CLUTS];
    int dw, dh;
    dvbsub_show hist[HIST];
    int hn, hpos, next_id, count;
};

/* ---------- палитры по умолчанию (EN 300 743, 10.1–10.3) ---------- */
static uint32_t D2[4], D4[16], D8[256];
static int defaults_ready;
static uint32_t argb(int r, int g, int b, int a) { return (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b; }
static void defaults(void) {
    if (defaults_ready) return;
    D2[0] = 0;
    D2[1] = argb(255, 255, 255, 255);
    D2[2] = argb(0, 0, 0, 255);
    D2[3] = argb(127, 127, 127, 255);
    for (int i = 0; i < 16; i++) {
        int v = i < 8 ? 255 : 127;
        D4[i] = i ? argb(i & 1 ? v : 0, i & 2 ? v : 0, i & 4 ? v : 0, 255) : 0;
    }
    D8[0] = 0;
    for (int i = 1; i < 256; i++) {
        int r, g, b, a;
        if (i < 8) { r = i & 1 ? 255 : 0; g = i & 2 ? 255 : 0; b = i & 4 ? 255 : 0; a = 63; }
        else switch (i & 0x88) {
            case 0x00:
                r = (i & 1 ? 85 : 0) + (i & 0x10 ? 170 : 0); g = (i & 2 ? 85 : 0) + (i & 0x20 ? 170 : 0); b = (i & 4 ? 85 : 0) + (i & 0x40 ? 170 : 0); a = 255; break;
            case 0x08:
                r = (i & 1 ? 85 : 0) + (i & 0x10 ? 170 : 0); g = (i & 2 ? 85 : 0) + (i & 0x20 ? 170 : 0); b = (i & 4 ? 85 : 0) + (i & 0x40 ? 170 : 0); a = 127; break;
            case 0x80:
                r = 127 + (i & 1 ? 43 : 0) + (i & 0x10 ? 85 : 0); g = 127 + (i & 2 ? 43 : 0) + (i & 0x20 ? 85 : 0); b = 127 + (i & 4 ? 43 : 0) + (i & 0x40 ? 85 : 0); a = 255; break;
            default:
                r = (i & 1 ? 43 : 0) + (i & 0x10 ? 85 : 0); g = (i & 2 ? 43 : 0) + (i & 0x20 ? 85 : 0); b = (i & 4 ? 43 : 0) + (i & 0x40 ? 85 : 0); a = 255; break;
            }
        D8[i] = argb(r, g, b, a);
    }
    defaults_ready = 1;
}

static int clampi(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
/* Y Cr Cb T (ITU-R BT.601, «студийный» диапазон, как у FFmpeg) -> ARGB; Y = 0 — прозрачный */
static uint32_t ycc(int y, int cr, int cb, int t) {
    if (!y) return 0;
    double yy = 1.164 * (y - 16);
    int r = clampi((int)(yy + 1.596 * (cr - 128) + 0.5));
    int g = clampi((int)(yy - 0.392 * (cb - 128) - 0.813 * (cr - 128) + 0.5));
    int b = clampi((int)(yy + 2.017 * (cb - 128) + 0.5));
    return argb(r, g, b, 255 - t);
}

dvbsub *dvbsub_new(int comp, int anc) {
    defaults();
    dvbsub *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    pthread_mutex_init(&d->mu, NULL);
    d->comp = comp;
    d->anc = anc;
    d->dw = 720;
    d->dh = 576;
    return d;
}

void dvbsub_show_clear(dvbsub_show *s) {
    for (int i = 0; i < s->nreg; i++) free(s->reg[i].px);
    s->nreg = 0;
}

static void reset_state(dvbsub *d) {
    for (int i = 0; i < MAX_REGIONS; i++) { free(d->reg[i].pix); memset(&d->reg[i], 0, sizeof d->reg[i]); }
    memset(d->cl, 0, sizeof d->cl);
}

void dvbsub_free(dvbsub *d) {
    if (!d) return;
    reset_state(d);
    for (int i = 0; i < HIST; i++) dvbsub_show_clear(&d->hist[i]);
    pthread_mutex_destroy(&d->mu);
    free(d);
}

static int be16(const uint8_t *p) { return p[0] << 8 | p[1]; }

static region *region_get(dvbsub *d, int id, int create) {
    for (int i = 0; i < MAX_REGIONS; i++) if (d->reg[i].used && d->reg[i].id == id) return &d->reg[i];
    if (!create) return NULL;
    for (int i = 0; i < MAX_REGIONS; i++) if (!d->reg[i].used) { d->reg[i].used = 1; d->reg[i].id = id; return &d->reg[i]; }
    return NULL;
}
static clut *clut_get(dvbsub *d, int id, int create) {
    for (int i = 0; i < MAX_CLUTS; i++) if (d->cl[i].used && d->cl[i].id == id) return &d->cl[i];
    if (!create) return NULL;
    for (int i = 0; i < MAX_CLUTS; i++)
        if (!d->cl[i].used) {
            clut *c = &d->cl[i];
            c->used = 1;
            c->id = id;
            memcpy(c->c2, D2, sizeof D2);
            memcpy(c->c4, D4, sizeof D4);
            memcpy(c->c8, D8, sizeof D8);
            return c;
        }
    return NULL;
}

/* ---------- сегменты ---------- */
static void emit(dvbsub *d);

static void page_composition(dvbsub *d, const uint8_t *s, int len, int64_t pts) {
    if (len < 2) return;
    if (d->pending) emit(d);   /* прежний набор без «конца набора» */
    int state = (s[1] >> 2) & 3;
    if (state == 1 || state == 2) reset_state(d);   /* точка входа / смена режима — всё заново */
    d->timeout = s[0];
    d->npreg = 0;
    for (int q = 2; q + 6 <= len && d->npreg < MAX_REGIONS; q += 6) {
        d->preg[d->npreg].id = s[q];
        d->preg[d->npreg].x = be16(s + q + 2);
        d->preg[d->npreg].y = be16(s + q + 4);
        d->npreg++;
    }
    d->set_pts = pts;
    d->pending = 1;
}

static void region_fill(region *r) {
    int v = r->depth == 1 ? r->bg2 : r->depth == 2 ? r->bg4 : r->bg8;
    if (r->pix) memset(r->pix, v, (size_t)r->w * (size_t)r->h);
}

static void region_composition(dvbsub *d, const uint8_t *s, int len) {
    if (len < 10) return;
    region *r = region_get(d, s[0], 1);
    if (!r) return;
    int fill = (s[1] >> 3) & 1, w = be16(s + 2), h = be16(s + 4);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    if (!r->pix || r->w != w || r->h != h) {
        free(r->pix);
        r->pix = calloc((size_t)w * (size_t)h, 1);
        r->w = w;
        r->h = h;
        fill = 1;
    }
    r->depth = (s[6] >> 2) & 7;
    r->clut = s[7];
    r->bg8 = s[8];
    r->bg4 = s[9] >> 4;
    r->bg2 = (s[9] >> 2) & 3;
    if (fill) region_fill(r);
    r->nobj = 0;
    for (int q = 10; q + 6 <= len && r->nobj < MAX_OBJS;) {
        int type = s[q + 2] >> 6;
        r->obj[r->nobj].obj = be16(s + q);
        r->obj[r->nobj].x = be16(s + q + 2) & 0xFFF;
        r->obj[r->nobj].y = be16(s + q + 4) & 0xFFF;
        r->nobj++;
        q += 6 + (type == 1 || type == 2 ? 2 : 0);
    }
}

static void clut_definition(dvbsub *d, const uint8_t *s, int len) {
    if (len < 2) return;
    clut *c = clut_get(d, s[0], 1);
    if (!c) return;
    for (int q = 2; q + 2 <= len;) {
        int e = s[q], f = s[q + 1], y, cr, cb, t;
        if (f & 1) {   /* полный диапазон */
            if (q + 6 > len) break;
            y = s[q + 2]; cr = s[q + 3]; cb = s[q + 4]; t = s[q + 5];
            q += 6;
        } else {
            if (q + 4 > len) break;
            y = s[q + 2] & 0xFC;
            cr = (((s[q + 2] & 3) << 2) | (s[q + 3] >> 6)) << 4;
            cb = (s[q + 3] << 2) & 0xF0;
            t = (s[q + 3] << 6) & 0xC0;
            q += 4;
        }
        uint32_t col = ycc(y, cr, cb, t);
        if ((f & 0x80) && e < 4) c->c2[e] = col;
        if ((f & 0x40) && e < 16) c->c4[e] = col;
        if (f & 0x20) c->c8[e] = col;
    }
}

/* ---- пиксели объекта: коды длин серий ---- */
typedef struct { const uint8_t *p; int n, bit; } bits;
static int getb(bits *b, int k) {
    int v = 0;
    while (k--) {
        int byte = b->bit >> 3;
        int x = byte < b->n ? (b->p[byte] >> (7 - (b->bit & 7))) & 1 : 0;
        v = v << 1 | x;
        b->bit++;
    }
    return v;
}
static void put(region *r, int *x, int y, int run, int v) {
    for (; run > 0; run--, (*x)++)
        if (*x >= 0 && *x < r->w && y >= 0 && y < r->h) r->pix[(size_t)y * (size_t)r->w + (size_t)*x] = (uint8_t)v;
}

/* строка 2-битных кодов; map — в глубину области. Длина в байтах. */
static int str2(region *r, const uint8_t *p, int n, int *x, int y, const uint8_t *map) {
    bits b = {p, n, 0};
    for (;;) {
        if (b.bit >= n * 8) break;
        int c = getb(&b, 2), run = 1;
        if (!c) {
            if (getb(&b, 1)) { run = getb(&b, 3) + 3; c = getb(&b, 2); }
            else if (getb(&b, 1)) { run = 1; c = 0; }
            else {
                int sw = getb(&b, 2);
                if (sw == 0) break;
                if (sw == 1) { run = 2; c = 0; }
                else if (sw == 2) { run = getb(&b, 4) + 12; c = getb(&b, 2); }
                else { run = getb(&b, 8) + 29; c = getb(&b, 2); }
            }
        }
        put(r, x, y, run, map ? map[c] : c);
    }
    return (b.bit + 7) / 8;
}
static int str4(region *r, const uint8_t *p, int n, int *x, int y, const uint8_t *map) {
    bits b = {p, n, 0};
    for (;;) {
        if (b.bit >= n * 8) break;
        int c = getb(&b, 4), run = 1;
        if (!c) {
            if (!getb(&b, 1)) {
                int l = getb(&b, 3);
                if (!l) break;
                run = l + 2;
                c = 0;
            } else if (!getb(&b, 1)) { run = getb(&b, 2) + 4; c = getb(&b, 4); }
            else {
                int sw = getb(&b, 2);
                if (sw == 0) { run = 1; c = 0; }
                else if (sw == 1) { run = 2; c = 0; }
                else if (sw == 2) { run = getb(&b, 4) + 9; c = getb(&b, 4); }
                else { run = getb(&b, 8) + 25; c = getb(&b, 4); }
            }
        }
        put(r, x, y, run, map ? map[c] : c);
    }
    return (b.bit + 7) / 8;
}
static int str8(region *r, const uint8_t *p, int n, int *x, int y) {
    int i = 0;
    while (i < n) {
        int c = p[i++], run = 1;
        if (!c) {
            if (i >= n) break;
            int f = p[i++];
            if (!(f & 0x80)) {
                if (!(f & 0x7F)) break;
                run = f & 0x7F;
                c = 0;
            } else {
                if (i >= n) break;
                run = f & 0x7F;
                c = p[i++];
            }
        }
        put(r, x, y, run, c);
    }
    return i;
}

/* блок пикселей одного поля объекта в области r, начиная со строки y0 */
static void pixel_block(region *r, const uint8_t *p, int n, int x0, int y0) {
    uint8_t m24[4] = {0, 7, 8, 15}, m28[4] = {0, 0x77, 0x88, 0xff}, m48[16];
    for (int i = 0; i < 16; i++) m48[i] = (uint8_t)(i * 0x11);
    int x = x0, y = y0, i = 0;
    while (i < n) {
        int type = p[i++];
        switch (type) {
        case 0x10: i += str2(r, p + i, n - i, &x, y, r->depth == 2 ? m24 : r->depth == 3 ? m28 : NULL); break;
        case 0x11: i += str4(r, p + i, n - i, &x, y, r->depth == 3 ? m48 : NULL); break;
        case 0x12: i += str8(r, p + i, n - i, &x, y); break;
        case 0x20: if (i + 2 <= n) { m24[0] = p[i] >> 4; m24[1] = p[i] & 15; m24[2] = p[i + 1] >> 4; m24[3] = p[i + 1] & 15; } i += 2; break;
        case 0x21: if (i + 4 <= n) memcpy(m28, p + i, 4); i += 4; break;
        case 0x22: if (i + 16 <= n) memcpy(m48, p + i, 16); i += 16; break;
        case 0xF0: x = x0; y += 2; break;
        default: return;
        }
    }
}

static void object_data(dvbsub *d, const uint8_t *s, int len) {
    if (len < 3) return;
    int id = be16(s), coding = (s[2] >> 2) & 3;
    if (coding != 0 || len < 7) return;   /* знаками (coding 1) не бывает на практике */
    int top = be16(s + 3), bot = be16(s + 5);
    if (7 + top + bot > len) return;
    const uint8_t *tp = s + 7, *bp = s + 7 + top;
    if (!bot) { bp = tp; bot = top; }   /* нижнее поле — как верхнее */
    for (int i = 0; i < MAX_REGIONS; i++) {
        region *r = &d->reg[i];
        if (!r->used || !r->pix) continue;
        for (int k = 0; k < r->nobj; k++) {
            if (r->obj[k].obj != id) continue;
            pixel_block(r, tp, top, r->obj[k].x, r->obj[k].y);
            pixel_block(r, bp, bot, r->obj[k].x, r->obj[k].y + 1);
        }
    }
}

/* набор готов: области страницы -> ARGB, в историю показов */
static void emit(dvbsub *d) {
    d->pending = 0;
    dvbsub_show *sh = &d->hist[d->hpos];
    dvbsub_show_clear(sh);
    sh->pts = d->set_pts;
    sh->end = d->timeout ? d->set_pts + (int64_t)d->timeout * 90000 : -1;
    sh->dw = d->dw;
    sh->dh = d->dh;
    sh->id = ++d->next_id;
    for (int k = 0; k < d->npreg && sh->nreg < DVBSUB_REGIONS; k++) {
        region *r = region_get(d, d->preg[k].id, 0);
        if (!r || !r->pix) continue;
        clut *c = clut_get(d, r->clut, 0);
        const uint32_t *pal = r->depth == 1 ? (c ? c->c2 : D2) : r->depth == 2 ? (c ? c->c4 : D4) : (c ? c->c8 : D8);
        int mask = r->depth == 1 ? 3 : r->depth == 2 ? 15 : 255;
        size_t n = (size_t)r->w * (size_t)r->h;
        uint32_t *px = malloc(n * 4);
        if (!px) continue;
        int any = 0;
        for (size_t i = 0; i < n; i++) { px[i] = pal[r->pix[i] & mask]; any |= px[i] >> 24 != 0; }
        if (!any) { free(px); continue; }   /* совсем прозрачная область — не рисовать */
        dvbsub_region *o = &sh->reg[sh->nreg++];
        o->x = d->preg[k].x;
        o->y = d->preg[k].y;
        o->w = r->w;
        o->h = r->h;
        o->px = px;
    }
    d->hpos = (d->hpos + 1) % HIST;
    if (d->hn < HIST) d->hn++;
    d->count++;
}

void dvbsub_feed(dvbsub *d, const uint8_t *s, size_t n, int64_t pts) {
    if (n < 3 || s[0] != 0x20 || s[1] != 0x00) return;
    pthread_mutex_lock(&d->mu);
    size_t p = 2;
    while (p + 6 <= n && s[p] == 0x0F) {
        int type = s[p + 1], page = be16(s + p + 2), len = be16(s + p + 4);
        if (p + 6 + (size_t)len > n) break;
        const uint8_t *seg = s + p + 6;
        int ours = !d->comp || page == d->comp || (d->anc && page == d->anc);
        if (ours) switch (type) {
            case 0x10: if (page == d->comp || !d->comp) page_composition(d, seg, len, pts); break;
            case 0x11: region_composition(d, seg, len); break;
            case 0x12: clut_definition(d, seg, len); break;
            case 0x13: object_data(d, seg, len); break;
            case 0x14: if (len >= 5) { d->dw = be16(seg + 1) + 1; d->dh = be16(seg + 3) + 1; } break;
            case 0x80: if (d->pending) emit(d); break;
            }
        p += 6 + (size_t)len;
    }
    pthread_mutex_unlock(&d->mu);
}

int dvbsub_at(dvbsub *d, int64_t pts, int have, dvbsub_show *out) {
    pthread_mutex_lock(&d->mu);
    dvbsub_show *best = NULL;
    for (int i = 0; i < d->hn; i++) {
        dvbsub_show *s = &d->hist[i];
        if (s->pts <= pts && (!best || s->pts > best->pts)) best = s;
    }
    int rc;
    if (!best || !best->nreg || (best->end >= 0 && pts >= best->end)) rc = -1;
    else if (best->id == have) rc = 0;
    else {
        dvbsub_show_clear(out);
        *out = *best;
        out->nreg = 0;
        for (int i = 0; i < best->nreg; i++) {
            const dvbsub_region *r = &best->reg[i];
            uint32_t *px = malloc((size_t)r->w * (size_t)r->h * 4);
            if (!px) continue;
            memcpy(px, r->px, (size_t)r->w * (size_t)r->h * 4);
            out->reg[out->nreg] = *r;
            out->reg[out->nreg].px = px;
            out->nreg++;
        }
        rc = 1;
    }
    pthread_mutex_unlock(&d->mu);
    return rc;
}

int dvbsub_count(dvbsub *d) { return d->count; }
