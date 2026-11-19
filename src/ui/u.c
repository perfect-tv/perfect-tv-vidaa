#include "u.h"
#include "app.h"
#include "img.h"
#include "core/prefs.h"
#include "core/logocache.h"
#include "data/repo.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double EM = 22.5;

void u_init(void) { EM = SH / (prefs_screen_dp() / 10.0); }

/* d — пиксель холста, c — цвет с прозрачностью a (0..255): «над» с учётом прозрачности холста */
static inline uint32_t over(uint32_t d, uint32_t c, unsigned a) {
    if (a >= 255) return c | 0xff000000u;
    if (!a) return d;
    unsigned da = d >> 24;
    /* под холстом пусто («дыра» к видео / только что стёртый кадр) — цвет и прозрачность как есть:
     * без делений на пиксель; фон гида и текст поверх видео — самый частый случай */
    if (!da) return (c & 0xffffffu) | (a << 24);
    if (da == 255) {
        unsigned ia = 255 - a;
        unsigned rb = ((c & 0xff00ffu) * a + (d & 0xff00ffu) * ia + 0x800080u);
        rb = ((rb + ((rb >> 8) & 0xff00ffu)) >> 8) & 0xff00ffu;
        unsigned gg = ((c & 0x00ff00u) * a + (d & 0x00ff00u) * ia + 0x008000u);
        gg = ((gg + ((gg >> 8) & 0x00ff00u)) >> 8) & 0x00ff00u;
        return 0xff000000u | rb | gg;
    }
    unsigned dw = da * (255 - a) / 255, oa = a + dw;
    if (!oa) return 0;
    unsigned r = (((c >> 16) & 255) * a + ((d >> 16) & 255) * dw) / oa;
    unsigned gr = (((c >> 8) & 255) * a + ((d >> 8) & 255) * dw) / oa;
    unsigned b = ((c & 255) * a + (d & 255) * dw) / oa;
    return (oa << 24) | (r << 16) | (gr << 8) | b;
}

static int clipx0(const gfx_t *g, int x) { return x < g->clip.x0 ? g->clip.x0 : x; }
static int clipx1(const gfx_t *g, int x) { return x > g->clip.x1 ? g->clip.x1 : x; }
static int clipy0(const gfx_t *g, int y) { return y < g->clip.y0 ? g->clip.y0 : y; }
static int clipy1(const gfx_t *g, int y) { return y > g->clip.y1 ? g->clip.y1 : y; }

void d_clear(gfx_t *g, uint32_t c) {
    for (int y = 0; y < g->h; y++) {
        uint32_t *r = g->px + (size_t)y * g->stride;
        if (!c) memset(r, 0, (size_t)g->w * 4);
        else for (int x = 0; x < g->w; x++) r[x] = c;
    }
}

void d_fill(gfx_t *g, int x, int y, int w, int h, uint32_t c) {
    int x0 = clipx0(g, x), x1 = clipx1(g, x + w), y0 = clipy0(g, y), y1 = clipy1(g, y + h);
    unsigned a = c >> 24;
    if (x0 >= x1 || y0 >= y1 || !a) return;
    uint32_t hole = (c & 0xffffffu) | (a << 24);   /* результат на прозрачном холсте («дыра» к видео) */
    for (int j = y0; j < y1; j++) {
        uint32_t *r = g->px + (size_t)j * g->stride;
        if (a == 255) for (int i = x0; i < x1; i++) r[i] = c;
        else {
            int i = x0;
            /* по четыре пикселя: если под всеми пусто (обычный случай — видео под окном), без blend на пиксель */
            for (; i + 3 < x1; i += 4) {
                uint32_t d0 = r[i], d1 = r[i + 1], d2 = r[i + 2], d3 = r[i + 3];
                if (((d0 | d1 | d2 | d3) & 0xff000000u) == 0) {
                    r[i] = hole; r[i + 1] = hole; r[i + 2] = hole; r[i + 3] = hole;
                } else {
                    r[i] = over(d0, c, a); r[i + 1] = over(d1, c, a);
                    r[i + 2] = over(d2, c, a); r[i + 3] = over(d3, c, a);
                }
            }
            for (; i < x1; i++) r[i] = over(r[i], c, a);
        }
    }
}

static inline unsigned corner(float px, float py, float cx, float cy, float r) {
    float dx = px - cx, dy = py - cy, d = sqrtf(dx * dx + dy * dy), v = r + 0.5f - d;
    return v <= 0 ? 0 : v >= 1 ? 255 : (unsigned)(v * 255);
}
/* покрытие скруглённого прямоугольника в точке (центр пикселя) */
static inline unsigned rr_cov(float px, float py, int x, int y, int w, int h, int r) {
    if (px < x || py < y || px > x + w || py > y + h) return 0;
    if (r <= 0) return 255;
    float cx = px < x + r ? x + r : px > x + w - r ? x + w - r : -1;
    float cy = py < y + r ? y + r : py > y + h - r ? y + h - r : -1;
    if (cx < 0 || cy < 0) return 255;
    return corner(px, py, cx, cy, (float)r);
}

void d_round(gfx_t *g, int x, int y, int w, int h, int r, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) { d_fill(g, x, y, w, h, c); return; }
    d_fill(g, x, y + r, w, h - 2 * r, c);
    unsigned a = c >> 24;
    /* верхняя и нижняя полосы: середина (дальше r от краёв) — полное покрытие, считаем углы только в
     * двух квадратах r×r по краям; иначе rr_cov с float на каждый пиксель полосы */
    int mid0 = x + r, mid1 = x + w - r;
    for (int pass = 0; pass < 2; pass++) {
        int ys = pass ? y + h - r : y, ye = ys + r;
        int j0 = clipy0(g, ys), j1 = clipy1(g, ye);
        if (j0 >= j1) continue;
        if (mid1 > mid0) d_fill(g, mid0, j0, mid1 - mid0, j1 - j0, c);
        for (int j = j0; j < j1; j++) {
            uint32_t *row = g->px + (size_t)j * g->stride;
            for (int i = clipx0(g, x); i < clipx1(g, x + w); i++) {
                if (i >= mid0 && i < mid1) { i = mid1 - 1; continue; }
                unsigned cv = rr_cov(i + 0.5f, j + 0.5f, x, y, w, h, r);
                if (cv) row[i] = over(row[i], c, a * cv / 255);
            }
        }
    }
}

/* скругление каждого угла своё: tl — левый верхний, дальше по часовой */
void d_round4(gfx_t *g, int x, int y, int w, int h, int tl, int tr, int br, int bl, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    int lim = (w < h ? w : h) / 2;
    int rr[4] = {tl, tr, br, bl};
    for (int k = 0; k < 4; k++) rr[k] = rr[k] < 0 ? 0 : rr[k] > lim ? lim : rr[k];
    unsigned a = c >> 24;
    for (int j = clipy0(g, y); j < clipy1(g, y + h); j++) {
        uint32_t *row = g->px + (size_t)j * g->stride;
        float py = j + 0.5f;
        for (int i = clipx0(g, x); i < clipx1(g, x + w); i++) {
            float px = i + 0.5f;
            int left = px < x + w / 2.0f, top = py < y + h / 2.0f;
            int k = top ? (left ? 0 : 1) : (left ? 3 : 2), r = rr[k];
            unsigned cv = 255;
            if (r > 0) {
                float cx = left ? x + r : x + w - r, cy = top ? y + r : y + h - r;
                if ((left ? px < cx : px > cx) && (top ? py < cy : py > cy)) cv = corner(px, py, cx, cy, (float)r);
            }
            if (cv) row[i] = over(row[i], c, a * cv / 255);
        }
    }
}

void d_stroke(gfx_t *g, int x, int y, int w, int h, int r, int t, uint32_t c) {
    if (w <= 0 || h <= 0 || t <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    int ir = r - t > 0 ? r - t : 0;
    unsigned a = c >> 24;
    for (int j = clipy0(g, y); j < clipy1(g, y + h); j++) {
        uint32_t *row = g->px + (size_t)j * g->stride;
        int inner_row = j >= y + t + ir && j < y + h - t - ir;
        for (int i = clipx0(g, x); i < clipx1(g, x + w); i++) {
            if (inner_row && i >= x + t && i < x + w - t) { i = x + w - t - 1; continue; }
            float px = i + 0.5f, py = j + 0.5f;
            unsigned o = rr_cov(px, py, x, y, w, h, r), in = rr_cov(px, py, x + t, y + t, w - 2 * t, h - 2 * t, ir);
            unsigned cv = o > in ? o - in : 0;
            if (cv) row[i] = over(row[i], c, a * cv / 255);
        }
    }
}

static uint32_t mix(uint32_t a, uint32_t b, unsigned t) {   /* t 0..256 */
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8) {
        unsigned x = (a >> s) & 255, y = (b >> s) & 255;
        out |= ((x * (256 - t) + y * t) >> 8 & 255) << s;
    }
    return out;
}
void d_vgrad(gfx_t *g, int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    if (h <= 0) return;
    for (int j = 0; j < h; j++) d_fill(g, x, y + j, w, 1, mix(top, bottom, (unsigned)(j * 256 / h)));
}
void d_hgrad(gfx_t *g, int x, int y, int w, int h, uint32_t left, uint32_t right) {
    if (w <= 0) return;
    for (int i = 0; i < w; i++) d_fill(g, x + i, y, 1, h, mix(left, right, (unsigned)(i * 256 / w)));
}

/* сглаживание многоугольника: 4×4 выборки на пиксель, правило чётности */
static int inside(const float *xy, int n, float x, float y) {
    int c = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float xi = xy[2 * i], yi = xy[2 * i + 1], xj = xy[2 * j], yj = xy[2 * j + 1];
        if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) c = !c;
    }
    return c;
}
void d_poly(gfx_t *g, const float *xy, int n, uint32_t c) {
    if (n < 3) return;
    float minx = xy[0], maxx = xy[0], miny = xy[1], maxy = xy[1];
    for (int i = 1; i < n; i++) {
        minx = fminf(minx, xy[2 * i]); maxx = fmaxf(maxx, xy[2 * i]);
        miny = fminf(miny, xy[2 * i + 1]); maxy = fmaxf(maxy, xy[2 * i + 1]);
    }
    unsigned a = c >> 24;
    for (int j = clipy0(g, (int)floorf(miny)); j < clipy1(g, (int)ceilf(maxy)); j++) {
        uint32_t *row = g->px + (size_t)j * g->stride;
        for (int i = clipx0(g, (int)floorf(minx)); i < clipx1(g, (int)ceilf(maxx)); i++) {
            int cnt = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) cnt += inside(xy, n, i + (sx + 0.5f) / 4, j + (sy + 0.5f) / 4);
            if (cnt) row[i] = over(row[i], c, a * (unsigned)cnt / 16);
        }
    }
}
void d_line(gfx_t *g, float x0, float y0, float x1, float y1, float w, uint32_t c) {
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if (l < 0.01f) return;
    float nx = -dy / l * w / 2, ny = dx / l * w / 2;
    /* концы скруглены на полтолщины вдоль линии */
    float ex = dx / l * w / 4, ey = dy / l * w / 4;
    float p[8] = {x0 + nx - ex, y0 + ny - ey, x1 + nx + ex, y1 + ny + ey, x1 - nx + ex, y1 - ny + ey, x0 - nx - ex, y0 - ny - ey};
    d_poly(g, p, 4, c);
}
void d_circle(gfx_t *g, float cx, float cy, float r, uint32_t c) {
    unsigned a = c >> 24;
    for (int j = clipy0(g, (int)(cy - r - 1)); j < clipy1(g, (int)(cy + r + 2)); j++) {
        uint32_t *row = g->px + (size_t)j * g->stride;
        for (int i = clipx0(g, (int)(cx - r - 1)); i < clipx1(g, (int)(cx + r + 2)); i++) {
            unsigned cv = corner(i + 0.5f, j + 0.5f, cx, cy, r);
            if (cv) row[i] = over(row[i], c, a * cv / 255);
        }
    }
}
void d_arc(gfx_t *g, float cx, float cy, float r, float w, float a0, float a1, uint32_t c) {
    int seg = (int)(fabsf(a1 - a0) / 6) + 2;
    float pts[2 * 130];
    if (seg > 64) seg = 64;
    int n = 0;
    for (int i = 0; i <= seg; i++) {
        float a = (a0 + (a1 - a0) * i / seg) * (float)M_PI / 180;
        pts[2 * n] = cx + cosf(a) * (r + w / 2);
        pts[2 * n + 1] = cy + sinf(a) * (r + w / 2);
        n++;
    }
    for (int i = seg; i >= 0; i--) {
        float a = (a0 + (a1 - a0) * i / seg) * (float)M_PI / 180;
        pts[2 * n] = cx + cosf(a) * (r - w / 2);
        pts[2 * n + 1] = cy + sinf(a) * (r - w / 2);
        n++;
    }
    d_poly(g, pts, n, c);
}

/* ---------- текст ---------- */
/* Стрелок ← ↑ → ↓ в шрифтах нет: в тексте они рисуются значками (docs/design.md, «Стрелки на клавишах»). */
static int px(double em) { int p = (int)(em * EM + 0.5); return p < 6 ? 6 : p; }
static int arrow_at(const char *s) {
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] == 0xe2 && u[1] == 0x86 && u[2] >= 0x90 && u[2] <= 0x93) return u[2] - 0x90;   /* 0 ← 1 ↑ 2 → 3 ↓ */
    return -1;
}
static int has_arrow(const char *s, const char *e) {
    for (; s && *s && (!e || s < e); s++) if (arrow_at(s) >= 0) return 1;
    return 0;
}
static int arrow_w(double em) { return E(em * 1.05); }
/* ширина куска [s, e) со стрелками */
static int seg_w(double em, int f, const char *s, const char *e) {
    int w = 0;
    char buf[1024];
    while (s < e) {
        const char *p = s;
        while (p < e && arrow_at(p) < 0) p++;
        if (p > s) { size_t n = (size_t)(p - s) < sizeof buf - 1 ? (size_t)(p - s) : sizeof buf - 1; memcpy(buf, s, n); buf[n] = 0; w += text_width(px(em), f, buf); }
        if (p < e) { w += arrow_w(em); p += 3; }
        s = p;
    }
    return w;
}
static int seg_draw(gfx_t *g, int x, int y, double em, int f, uint32_t c, const char *s, const char *e) {
    static const int ic[] = {I_LEFT, I_UP, I_RIGHT, I_DOWN};
    int x0 = x;
    char buf[1024];
    int cy = y + text_ascent_w(px(em), f) - (int)(px(em) * 0.36);
    while (s < e) {
        const char *p = s;
        while (p < e && arrow_at(p) < 0) p++;
        if (p > s) { size_t n = (size_t)(p - s) < sizeof buf - 1 ? (size_t)(p - s) : sizeof buf - 1; memcpy(buf, s, n); buf[n] = 0; x += text_draw(g, x, y, px(em), f, c, buf, 0); }
        if (p < e) { d_icon(g, ic[arrow_at(p)], x + arrow_w(em) / 2, cy, (int)(px(em) * 0.8), c); x += arrow_w(em); p += 3; }
        s = p;
    }
    return x - x0;
}
int d_text(gfx_t *g, int x, int y, double em, int f, uint32_t c, const char *s, int maxw) {
    if (s && has_arrow(s, NULL)) {
        const char *end = s + strlen(s);
        if (maxw <= 0 || seg_w(em, f, s, end) <= maxw) return seg_draw(g, x, y, em, f, c, s, end);
        /* не помещается: самое длинное начало + «…» (стрелки — значками, поэтому мерить кусками) */
        int ell = text_width(px(em), f, "…");
        const char *e = s, *fit = s;
        while (*e) {
            const char *q = e;
            if (arrow_at(q) >= 0) q += 3; else { q++; while ((*q & 0xc0) == 0x80) q++; }
            if (seg_w(em, f, s, q) + ell > maxw) break;
            fit = e = q;
        }
        int w = seg_draw(g, x, y, em, f, c, s, fit);
        return w + text_draw(g, x + w, y, px(em), f, c, "…", 0);
    }
    return text_draw(g, x, y, px(em), f, c, s, maxw);
}
int d_text_w(double em, int f, const char *s) {
    if (s && has_arrow(s, NULL)) return seg_w(em, f, s, s + strlen(s));
    return text_width(px(em), f, s);
}
int d_text_r(gfx_t *g, int r, int y, double em, int f, uint32_t c, const char *s) { return d_text(g, r - d_text_w(em, f, s), y, em, f, c, s, 0); }
int d_text_c(gfx_t *g, int x, int w, int y, double em, int f, uint32_t c, const char *s) {
    int tw = d_text_w(em, f, s);
    if (tw > w) return d_text(g, x, y, em, f, c, s, w);
    return d_text(g, x + (w - tw) / 2, y, em, f, c, s, 0);
}
int d_line_h(double em, int f) { return text_line_height_w(px(em), f); }
int d_text_base(gfx_t *g, int x, int base, double em, int f, uint32_t c, const char *s, int maxw) { return d_text(g, x, base - text_ascent_w(px(em), f), em, f, c, s, maxw); }
int d_text_mid(gfx_t *g, int x, int cy, double em, int f, uint32_t c, const char *s, int maxw) {
    int p = px(em);
    int base = cy + (int)(p * 0.36);   /* половина высоты прописной */
    return d_text(g, x, base - text_ascent_w(p, f), em, f, c, s, maxw);
}
/* перенос по словам; последняя разрешённая строка — с многоточием */
int d_lines(gfx_t *g, int x, int y, int w, double em, int f, uint32_t c, const char *s, int max_lines, double gap) {
    if (!s || !*s || max_lines <= 0) return 0;
    if (!has_arrow(s, NULL)) return text_draw_wrapped(g, x, y, w, px(em), f, c, s, max_lines, E(gap));
    int lh = d_line_h(em, f) + E(gap), n = 0;
    while (*s && n < max_lines) {
        const char *e = s, *last_space = NULL;
        while (*e && *e != '\n') {
            const char *q = e;
            if (arrow_at(q) >= 0) q += 3; else { q++; while ((*q & 0xc0) == 0x80) q++; }
            if (seg_w(em, f, s, q) > w && e > s) break;
            if (*e == ' ') last_space = e;
            e = q;
        }
        const char *next = e;
        if (*e && *e != '\n' && last_space) { e = last_space; next = last_space + 1; }
        else if (*e == '\n') next = e + 1;
        seg_draw(g, x, y + n * lh, em, f, c, s, e);
        n++;
        s = next;
        while (*s == ' ') s++;
    }
    return n;
}

/* ---------- значки ---------- */
/* Значок — векторный путь (дуги, линии, многоугольники с 4×4 выборками): на кадр их десятки (строка гида —
 * архив, сердце, замок), и каждый раз считать это заново дорого на ТВ. Кэш покрытия: путь рисуется один раз
 * на пару «вид + размер», дальше — только наложение маски (как глифы шрифта). Цвет — при наложении. */
#define IC_MAX 64
typedef struct { int kind, size, w, h, ox, oy; uint8_t *cov; unsigned used; } icon_mask;
static icon_mask IC[IC_MAX];
static int nIC;
static unsigned ic_used;

static void icon_vec(gfx_t *g, int k, int cx, int cy, int size, uint32_t c);
static icon_mask *icon_mask_get(int k, int size) {
    for (int i = 0; i < nIC; i++) if (IC[i].kind == k && IC[i].size == size) { IC[i].used = ++ic_used; return &IC[i]; }
    int at;
    if (nIC < IC_MAX) at = nIC++;
    else {
        at = 0;
        for (int i = 1; i < nIC; i++) if (IC[i].used < IC[at].used) at = i;
        free(IC[at].cov);
    }
    int m = size / 2 + 4, w = size + 2 * m;
    uint32_t *px = calloc((size_t)w * w, 4);
    if (!px) { if (at < nIC) { memset(&IC[at], 0, sizeof IC[at]); nIC--; } return NULL; }
    gfx_t t;
    gfx_init(&t, px, w, w, w);
    icon_vec(&t, k, m + size / 2, m + size / 2, size, 0xffffffffu);
    /* обрезаем до реальных границ: значок занимает size×size, а маска — квадрат с запасом под обводку */
    int bx0 = w, by0 = w, bx1 = -1, by1 = -1;
    for (int j = 0; j < w; j++)
        for (int i = 0; i < w; i++)
            if (px[(size_t)j * w + i] >> 24) {
                if (i < bx0) bx0 = i;
                if (i > bx1) bx1 = i;
                if (j < by0) by0 = j;
                if (j > by1) by1 = j;
            }
    if (bx1 < 0) { bx0 = by0 = 0; bx1 = by1 = 0; }
    int mw = bx1 - bx0 + 1, mh = by1 - by0 + 1;
    uint8_t *cov = calloc((size_t)mw * mh, 1);
    if (!cov) { free(px); if (at < nIC) { memset(&IC[at], 0, sizeof IC[at]); nIC--; } return NULL; }
    for (int j = 0; j < mh; j++)
        for (int i = 0; i < mw; i++) cov[(size_t)j * mw + i] = (uint8_t)(px[(size_t)(by0 + j) * w + bx0 + i] >> 24);
    free(px);
    IC[at] = (icon_mask){k, size, mw, mh, m + size / 2 - bx0, m + size / 2 - by0, cov, ++ic_used};
    return &IC[at];
}
/* маска с честной прозрачностью: пиксели холста, где видео видно, остаются прозрачными */
static void mask_blit(gfx_t *g, const icon_mask *m, int x, int y, uint32_t c) {
    unsigned ca = c >> 24;
    if (!ca) return;
    int x0 = clipx0(g, x), x1 = clipx1(g, x + m->w), y0 = clipy0(g, y), y1 = clipy1(g, y + m->h);
    for (int j = y0; j < y1; j++) {
        uint32_t *row = g->px + (size_t)j * g->stride;
        const uint8_t *s = m->cov + (size_t)(j - y) * m->w;
        for (int i = x0; i < x1; i++) {
            unsigned a = s[i - x];
            if (a) row[i] = over(row[i], c & 0xffffffu, a * ca / 255);
        }
    }
}
void d_icon(gfx_t *g, int k, int cx, int cy, int size, uint32_t c) {
    if (size <= 0 || !(c >> 24)) return;
    icon_mask *m = icon_mask_get(k, size);
    if (!m) return;
    mask_blit(g, m, cx - m->ox, cy - m->oy, c);
}

static void icon_vec(gfx_t *g, int k, int cx, int cy, int size, uint32_t c) {
    float s = (float)size, x = (float)cx, y = (float)cy, h = s / 2, lw = fmaxf(1.5f, s * 0.12f);
    switch (k) {
    case I_PLAY: { float p[6] = {x - h * 0.6f, y - h * 0.8f, x + h * 0.8f, y, x - h * 0.6f, y + h * 0.8f}; d_poly(g, p, 3, c); break; }
    case I_PAUSE: d_fill(g, (int)(x - h * 0.6f), (int)(y - h * 0.75f), (int)(s * 0.22f + 1), (int)(s * 0.75f), c);
                  d_fill(g, (int)(x + h * 0.16f), (int)(y - h * 0.75f), (int)(s * 0.22f + 1), (int)(s * 0.75f), c); break;
    case I_STOP: d_fill(g, (int)(x - h * 0.65f), (int)(y - h * 0.65f), (int)(s * 0.65f), (int)(s * 0.65f), c); break;
    case I_ARCHIVE: case I_HISTORY: case I_REFRESH: {
        int cw = k == I_REFRESH;
        float r = h * 0.72f;
        d_arc(g, x, y, r, lw, cw ? -60 : 240, cw ? 240 : -60, c);
        /* стрелка у начала дуги */
        float a = (cw ? 240 : -60) * (float)M_PI / 180, ax = x + cosf(a) * r, ay = y + sinf(a) * r;
        float d = cw ? 1 : -1;
        float p[6] = {ax - h * 0.35f, ay - h * 0.05f * d, ax + h * 0.35f, ay - h * 0.05f * d, ax, ay + h * 0.4f * d};
        if (cw) { float q[6] = {ax - h * 0.05f, ay - h * 0.35f, ax - h * 0.05f, ay + h * 0.35f, ax + h * 0.4f, ay}; memcpy(p, q, sizeof p); }
        else { float q[6] = {ax + h * 0.05f, ay - h * 0.35f, ax + h * 0.05f, ay + h * 0.35f, ax - h * 0.4f, ay}; memcpy(p, q, sizeof p); }
        d_poly(g, p, 3, c);
        if (k != I_REFRESH) { d_line(g, x, y, x, y - r * 0.55f, lw * 0.9f, c); d_line(g, x, y, x + r * 0.45f, y + r * 0.25f, lw * 0.9f, c); }
        break;
    }
    case I_CLOCK: {
        float r = h * 0.78f;
        d_arc(g, x, y, r, lw, 0, 360, c);
        d_line(g, x, y, x, y - r * 0.6f, lw * 0.9f, c);
        d_line(g, x, y, x + r * 0.45f, y + r * 0.2f, lw * 0.9f, c);
        break;
    }
    case I_LOCK:
        d_round(g, (int)(x - h * 0.7f), (int)(y - h * 0.1f), (int)(s * 0.7f), (int)(s * 0.55f), (int)(s * 0.08f), c);
        d_arc(g, x, y - h * 0.15f, h * 0.38f, lw, 180, 360, c);
        d_line(g, x - h * 0.38f, y - h * 0.15f, x - h * 0.38f, y, lw, c);
        d_line(g, x + h * 0.38f, y - h * 0.15f, x + h * 0.38f, y, lw, c);
        break;
    case I_HEART: {
        float r = h * 0.42f;
        d_circle(g, x - r * 0.85f, y - r * 0.45f, r, c);
        d_circle(g, x + r * 0.85f, y - r * 0.45f, r, c);
        float p[8] = {x - r * 1.75f, y - r * 0.2f, x, y - r * 0.4f, x + r * 1.75f, y - r * 0.2f, x, y + h * 0.85f};
        d_poly(g, p, 4, c);
        break;
    }
    case I_STAR: {
        float p[20];
        for (int i = 0; i < 10; i++) {
            float a = (float)(-M_PI / 2 + i * M_PI / 5), r = i % 2 ? h * 0.4f : h * 0.95f;
            p[2 * i] = x + cosf(a) * r;
            p[2 * i + 1] = y + sinf(a) * r;
        }
        d_poly(g, p, 10, c);
        break;
    }
    case I_CHECK: d_line(g, x - h * 0.6f, y, x - h * 0.15f, y + h * 0.45f, lw, c); d_line(g, x - h * 0.15f, y + h * 0.45f, x + h * 0.65f, y - h * 0.5f, lw, c); break;
    case I_LEFT: case I_RIGHT: case I_UP: case I_DOWN: {
        float dx = k == I_LEFT ? -1 : k == I_RIGHT ? 1 : 0, dy = k == I_UP ? -1 : k == I_DOWN ? 1 : 0;
        float tx = x + dx * h * 0.7f, ty = y + dy * h * 0.7f;
        d_line(g, x - dx * h * 0.7f, y - dy * h * 0.7f, tx, ty, lw, c);
        d_line(g, tx, ty, tx - dx * h * 0.5f - dy * h * 0.5f, ty - dy * h * 0.5f - dx * h * 0.5f, lw, c);
        d_line(g, tx, ty, tx - dx * h * 0.5f + dy * h * 0.5f, ty - dy * h * 0.5f + dx * h * 0.5f, lw, c);
        break;
    }
    case I_CHEVRON: d_line(g, x - h * 0.25f, y - h * 0.5f, x + h * 0.25f, y, lw, c); d_line(g, x + h * 0.25f, y, x - h * 0.25f, y + h * 0.5f, lw, c); break;
    case I_SEARCH: d_arc(g, x - h * 0.15f, y - h * 0.15f, h * 0.5f, lw, 0, 360, c); d_line(g, x + h * 0.22f, y + h * 0.22f, x + h * 0.75f, y + h * 0.75f, lw * 1.2f, c); break;
    case I_CLOSE: d_line(g, x - h * 0.6f, y - h * 0.6f, x + h * 0.6f, y + h * 0.6f, lw, c); d_line(g, x + h * 0.6f, y - h * 0.6f, x - h * 0.6f, y + h * 0.6f, lw, c); break;
    case I_PLUS: d_line(g, x - h * 0.65f, y, x + h * 0.65f, y, lw, c); d_line(g, x, y - h * 0.65f, x, y + h * 0.65f, lw, c); break;
    case I_LIST: for (int i = -1; i <= 1; i++) d_fill(g, (int)(x - h * 0.7f), (int)(y + i * h * 0.5f - lw / 2), (int)(s * 0.7f), (int)lw + 1, c); break;
    case I_GRID: for (int i = 0; i < 4; i++) d_fill(g, (int)(x - h * 0.7f + (i % 2) * h * 0.8f), (int)(y - h * 0.7f + (i / 2) * h * 0.8f), (int)(h * 0.6f), (int)(h * 0.6f), c); break;
    case I_LIVE: d_circle(g, x, y, h * 0.35f, c); d_arc(g, x, y, h * 0.75f, lw * 0.8f, -45, 45, c); d_arc(g, x, y, h * 0.75f, lw * 0.8f, 135, 225, c); break;
    case I_GEAR:
        for (int i = 0; i < 8; i++) { float a = (float)(i * M_PI / 4); d_line(g, x + cosf(a) * h * 0.45f, y + sinf(a) * h * 0.45f, x + cosf(a) * h * 0.85f, y + sinf(a) * h * 0.85f, lw * 1.4f, c); }
        d_arc(g, x, y, h * 0.5f, lw * 1.3f, 0, 360, c);
        break;
    case I_BACKSPACE: {
        float p[10] = {x - h * 0.9f, y, x - h * 0.4f, y - h * 0.6f, x + h * 0.85f, y - h * 0.6f, x + h * 0.85f, y + h * 0.6f, x - h * 0.4f, y + h * 0.6f};
        for (int i = 0; i < 5; i++) d_line(g, p[2 * i], p[2 * i + 1], p[(2 * i + 2) % 10], p[(2 * i + 3) % 10], lw * 0.8f, c);
        d_line(g, x - h * 0.05f, y - h * 0.25f, x + h * 0.45f, y + h * 0.25f, lw * 0.8f, c);
        d_line(g, x + h * 0.45f, y - h * 0.25f, x - h * 0.05f, y + h * 0.25f, lw * 0.8f, c);
        break;
    }
    case I_INFO: d_arc(g, x, y, h * 0.8f, lw, 0, 360, c); d_fill(g, (int)(x - lw / 2), (int)(y - h * 0.1f), (int)lw + 1, (int)(h * 0.55f), c); d_circle(g, x, y - h * 0.38f, lw * 0.7f, c); break;
    case I_EYE_OFF:
        d_arc(g, x, y + h * 0.9f, h * 1.2f, lw, 225, 315, c);
        d_arc(g, x, y - h * 0.9f, h * 1.2f, lw, 45, 135, c);
        d_line(g, x - h * 0.7f, y + h * 0.7f, x + h * 0.7f, y - h * 0.7f, lw, c);
        break;
    default: break;
    }
}

void d_spinner(gfx_t *g, int cx, int cy, int r, double now) {
    float a = (float)fmod(now * 360.0, 360.0);
    d_arc(g, (float)cx, (float)cy, (float)r, r * 0.18f, 0, 360, WA(0.15));
    d_arc(g, (float)cx, (float)cy, (float)r, r * 0.18f, a, a + 100, C_WHITE);
}

/* ---------- бейджи, подсказки ---------- */
int d_badge_w(double em, int icon, const char *t) {
    int pad = E(em * 0.5), w = pad * 2;
    if (icon >= 0) w += E(em * 0.9) + (t && *t ? E(em * 0.35) : 0);
    if (t) w += d_text_w(em, F_S, t);
    return w;
}
int d_badge(gfx_t *g, int x, int cy, double em, uint32_t bg, uint32_t fg, int icon, const char *t) {
    int w = d_badge_w(em, icon, t), h = E(em * 1.6), pad = E(em * 0.5);
    d_round(g, x, cy - h / 2, w, h, E(0.4), bg);
    int tx = x + pad;
    if (icon >= 0) { d_icon(g, icon, tx + E(em * 0.45), cy, E(em * 0.85), fg); tx += E(em * 0.9) + E(em * 0.35); }
    if (t) d_text_mid(g, tx, cy, em, F_S, fg, t, 0);
    return w;
}

static int keycap_w(const hint *h) {
    int w = h->key ? d_text_w(1.1, F_S, h->key) + E(1.0) : E(2.0);
    return w < E(2.0) ? E(2.0) : w;
}
int d_hints_w(const hint *h, int n) {
    int w = 0;
    for (int i = 0; i < n; i++) w += keycap_w(&h[i]) + E(0.6) + d_text_w(1.15, F_R, h[i].action) + (i + 1 < n ? E(1.8) : 0);
    return w;
}
int d_hints(gfx_t *g, int x, int cy, const hint *h, int n) {
    int x0 = x;
    for (int i = 0; i < n; i++) {
        int kw = keycap_w(&h[i]), kh = E(1.7);
        d_stroke(g, x, cy - kh / 2, kw, kh, E(0.4), E(0.1) > 1 ? E(0.1) : 1, WA(0.40));
        if (h[i].key) d_text_c(g, x, kw, cy - d_line_h(1.1, F_S) / 2, 1.1, F_S, WA(0.85), h[i].key);
        else d_icon(g, h[i].icon, x + kw / 2, cy, E(0.95), WA(0.85));
        x += kw + E(0.6);
        x += d_text_mid(g, x, cy, 1.15, F_R, WA(0.60), h[i].action, 0);
        x += E(1.8);
    }
    return x - x0;
}

void d_fade_edges(gfx_t *g, int x, int y, int w, int h, int top, int bottom, uint32_t bg) {
    int f = E(3);
    if (f > h / 3) f = h / 3;
    if (top) d_vgrad(g, x, y, w, f, bg, bg & 0xffffffu);
    if (bottom) d_vgrad(g, x, y + h - f, w, f, bg & 0xffffffu, bg);
}

/* ---------- бегущая строка (как MarqueeMath у Android) ---------- */
#define MQ_PAUSE 1.5
#define MQ_SPEED 7.0
typedef struct { unsigned long long h; double t0; } mslot;
static mslot mq[8];
static double mq_last;
static unsigned long long mq_hash(const char *s) {
    unsigned long long h = 1469598103934665603ull;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
    return h;
}
int d_marquee(gfx_t *g, int x, int cy, double em, int face, uint32_t c, const char *s, int width, int active, int slot) {
    if (!s || !*s || width <= 0) return 0;
    int tw = d_text_w(em, face, s);
    if (!active || !P.marquee || tw <= width) { d_text_mid(g, x, cy, em, face, c, s, width); return 0; }
    double now = app_now(), speed = MQ_SPEED * EM, overflow = tw - width;
    double scroll = overflow / speed, cycle = 2 * MQ_PAUSE + scroll;
    mslot *m = &mq[slot & 7];
    unsigned long long h = mq_hash(s) ^ ((unsigned long long)width << 32);
    if (m->h != h) { m->h = h; m->t0 = now; }
    double t = fmod(now - m->t0, cycle);
    if (t < 0) t += cycle;
    double off = t <= MQ_PAUSE ? 0 : t >= MQ_PAUSE + scroll ? overflow : (t - MQ_PAUSE) * speed;
    int lh = d_line_h(em, face);
    gfx_push_clip(g, x, cy - lh, width, 2 * lh);
    d_text_mid(g, x - (int)off, cy, em, face, c, s, 0);
    gfx_pop_clip(g);
    mq_last = now;
    return 1;
}
int d_marquee_active(void) { return mq_last > 0 && app_now() - mq_last < 0.08; }

/* ---------- картинки и логотипы ---------- */
/* Картинка ARGB (src: sw×sh) в прямоугольник x, y, w, h — масштаб ближайшим пикселем, честная прозрачность. */
void d_bitmap(gfx_t *g, const uint32_t *src, int sw, int sh, int x, int y, int w, int h) {
    if (!src || sw <= 0 || sh <= 0 || w <= 0 || h <= 0) return;
    int x0 = clipx0(g, x), x1 = clipx1(g, x + w), y0 = clipy0(g, y), y1 = clipy1(g, y + h);
    for (int j = y0; j < y1; j++) {
        const uint32_t *s = src + (size_t)((long)(j - y) * sh / h) * (size_t)sw;
        uint32_t *r = g->px + (size_t)j * g->stride;
        for (int i = x0; i < x1; i++) {
            uint32_t c = s[(long)(i - x) * sw / w];
            unsigned a = c >> 24;
            if (a) r[i] = over(r[i], c & 0xffffffu, a);
        }
    }
}

int d_image(gfx_t *g, const char *url, int x, int y, int w, int h, int r, int cover) {
    if (!url || !*url || w <= 0 || h <= 0) return 0;
    const gfx_image *im = img_get(url, w, h, cover ? IMG_COVER : IMG_FIT);
    if (!im) return 0;
    gfx_image_draw(g, im, x, y, r, 255);
    return 1;
}

static int logo_draw(gfx_t *g, const char *url, int x, int y, int w, int h) {
    const gfx_image *im = logo_img(url, w, h, IMG_FIT);
    if (!im) return 0;
    gfx_image_draw(g, im, x, y, 0, 255);
    return 1;
}

void d_logo(gfx_t *g, const channel *c, int x, int y, int w, int h, int r, int badge, int on_white) {
    const char *url = repo_logo(c);
    int pad = (int)(h * 0.14);
    int have = 0;
    if (url && *url) {
        d_round(g, x, y, w, h, r, on_white ? C_LOGO_PLATE : C_FILL_SOFT);
        have = logo_draw(g, url, x + pad, y + pad, w - 2 * pad, h - 2 * pad);
        if (!have) {   /* плейлист не отдал картинку — пробуем второй источник (программа/плейлист) */
            const char *alt = repo_logo_alt(c, url);
            if (alt && *alt && strcmp(alt, url)) have = logo_draw(g, alt, x + pad, y + pad, w - 2 * pad, h - 2 * pad);
        }
    }
    if (!have) {
        uint32_t col = c->color;
        d_round(g, x, y, w, h, r, col);
        double em = h / EM * 0.38;
        if (em > 2.2) em = 2.2;
        if (d_text_w(em, F_B, c->mono) > w - 2 * pad) em *= 0.8;
        int shift = badge && P.number_mode == NUM_BADGE && c->num > 0 && h < E(5) ? h / 7 : 0;   /* не под бейджем номера */
        d_text_c(g, x, w, y + shift + (h - d_line_h(em, F_B)) / 2, em, F_B, 0xfff4f4f4u, c->mono);
    }
    if (badge && P.number_mode == NUM_BADGE && c->num > 0) {
        /* как Logos.drawBadge у Android-версии: кегль — не мельче 0,9 em и 26 % высоты логотипа. Бейдж — вплотную
         * к левому верхнему углу подложки: этот угол скруглён как у подложки, правый верхний и левый нижний — прямые,
         * правый нижний скруглён (радиус подложки, не меньше четверти высоты бейджа). Цифры Onest на пятую часть
         * выше цифр Roboto — кегль меньше, чтобы бейдж выглядел так же. */
        char n[16];
        snprintf(n, sizeof n, "%d", c->num);
        double size = h * 0.26 > E(0.9) ? h * 0.26 : E(0.9), em = size / EM * 0.82;
        int bh = (int)(size * 1.35), tw = d_text_w(em, F_B, n), bw = tw + (int)(size * 0.75);
        if (bw < bh * 6 / 5) bw = bh * 6 / 5;
        int br = r > bh / 4 ? r : bh / 4;
        if (br > bh / 2) br = bh / 2;
        d_round4(g, x, y, bw, bh, r, 0, br, 0, C_NUM_PLATE);
        d_text_c(g, x, bw, y + (bh - d_line_h(em, F_B)) / 2, em, F_B, WA(0.92), n);
    }
}
