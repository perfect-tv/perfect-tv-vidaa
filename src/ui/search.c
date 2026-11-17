/* Поиск во весь экран (docs/controls.md, «Поиск»): слева поле, клавиатура и «Запросы» (недавние и подсказки),
 * справа результаты: каналы, «Сейчас в эфире», «Скоро», «В архиве». Логика поиска — data/search.c. */
#include "screens.h"
#include "kbd.h"
#include "layer.h"
#include "keys.h"
#include "tv.h"
#include "u.h"
#include "app.h"
#include "core/clock.h"
#include "core/lang.h"
#include "data/search.h"
#include <stdio.h>
#include <string.h>

enum { Z_KB, Z_Q, Z_RES };
enum { R_HEAD, R_CH, R_PROG };
#define MAX_ROWS (SEARCH_PER_SECTION * (SR_SECTIONS + 1) + SR_SECTIONS + 1)
#define MAX_Q 5
typedef struct { int type, sec, idx; } row;

static struct {
    kb k;
    char last[2048];
    int zone;
    row rows[MAX_ROWS];
    int nrows, focus, top, built_rev;
    char q[MAX_Q][200];
    int nq, nrecent, qfocus;
} SR;
static layer SL;

static int selectable(int i) { return i >= 0 && i < SR.nrows && SR.rows[i].type != R_HEAD; }

static void build(void) {
    SR.nrows = 0;
    channel **ch;
    int n = search_channels(&ch);
    static const int heads[] = {S_SEARCH_NOW, S_SEARCH_SOON, S_SEARCH_ARCHIVE};
    if (n) {
        SR.rows[SR.nrows++] = (row){R_HEAD, -1, 0};
        for (int i = 0; i < n; i++) SR.rows[SR.nrows++] = (row){R_CH, -1, i};
    }
    for (int s = 0; s < SR_SECTIONS; s++) {
        search_hit *h;
        int m = search_hits(s, &h);
        if (!m) continue;
        SR.rows[SR.nrows++] = (row){R_HEAD, s, heads[s]};
        for (int i = 0; i < m; i++) SR.rows[SR.nrows++] = (row){R_PROG, s, i};
    }
    /* запросы: недавние, затем подсказки из найденного */
    SR.nrecent = SR.nq = 0;
    char rec[8][200];
    int nr = search_recent(rec, 8);
    char low[200], fq[200];
    search_fold(SR.k.text, fq, sizeof fq);
    for (int i = 0; i < nr && SR.nq < MAX_Q; i++) {
        search_fold(rec[i], low, sizeof low);
        if (fq[0] && (!strstr(low, fq) || !strcmp(low, fq))) continue;
        snprintf(SR.q[SR.nq++], 200, "%s", rec[i]);
    }
    SR.nrecent = SR.nq;
    char sug[MAX_Q][200];
    int ns = fq[0] ? search_suggest(sug, MAX_Q) : 0;
    for (int i = 0; i < ns && SR.nq < MAX_Q; i++) {
        int dup = 0;
        for (int k = 0; k < SR.nq && !dup; k++) if (!strcmp(SR.q[k], sug[i])) dup = 1;
        if (!dup) snprintf(SR.q[SR.nq++], 200, "%s", sug[i]);
    }
    if (SR.qfocus >= SR.nq) SR.qfocus = SR.nq - 1;
    if (SR.qfocus < 0) SR.qfocus = 0;
    if (!selectable(SR.focus)) {
        SR.focus = 0;
        while (SR.focus < SR.nrows && !selectable(SR.focus)) SR.focus++;
    }
    if (SR.zone == Z_RES && !selectable(SR.focus)) SR.zone = Z_KB;
    if (SR.zone == Z_Q && !SR.nq) SR.zone = Z_KB;
    SR.built_rev = search_rev();
}

static void set_text(const char *t) {
    snprintf(SR.k.text, sizeof SR.k.text, "%s", t);
    snprintf(SR.last, sizeof SR.last, "%s", t);
    search_set(SR.k.text);
    SR.focus = SR.top = 0;
    build();
}

static void move(int d) {
    int i = SR.focus;
    do i += d; while (i >= 0 && i < SR.nrows && !selectable(i));
    if (selectable(i)) SR.focus = i;
}

static channel *row_channel(const row *r, search_hit **hit) {
    *hit = NULL;
    if (r->type == R_CH) { channel **ch; search_channels(&ch); return ch[r->idx]; }
    search_hit *h;
    search_hits(r->sec, &h);
    *hit = &h[r->idx];
    return h[r->idx].c;
}

static void activate(int long_press) {
    if (!selectable(SR.focus)) return;
    search_hit *h;
    channel *c = row_channel(&SR.rows[SR.focus], &h);
    search_remember(SR.k.text);
    if (long_press) { tv_open_schedule(c, 0); return; }
    if (!h || SR.rows[SR.focus].sec == SR_NOW) {
        layer_pop_all();
        tv_play(c, NULL);
    } else if (SR.rows[SR.focus].sec == SR_ARCHIVE && repo_has_archive(c)) {
        int64_t start = h->start;
        layer_pop_all();
        tv_play_archive(c, start, NULL);
    } else {
        int i = repo_prog_at(c, h->start);
        prog p;
        if (i >= 0) p = repo_prog(c, i);
        else { memset(&p, 0, sizeof p); p.start = h->start; p.stop = h->stop; p.title = h->title; p.sub = p.desc = p.cat = p.icon = ""; }
        card_open(c, &p);
    }
}

static int s_has_long(layer *l, int key) {
    (void)l;
    if (key != KEY_OK) return 0;
    if (SR.zone == Z_KB) return kb_long(&SR.k, key);
    if (SR.zone == Z_Q) return SR.qfocus < SR.nrecent;
    return 1;
}

static int s_key(layer *l, int k, int ev) {
    (void)l;
    if (SR.built_rev != search_rev()) build();
    if (SR.zone == Z_KB) {
        if (k == KEY_BACK && ev == EV_SHORT) { layer_pop(&SL); return 1; }
        int r = kb_key(&SR.k, k, ev);
        if ((r == KB_OUT_RIGHT || r == KB_DONE) && selectable(SR.focus)) SR.zone = Z_RES;
        else if (r == KB_OUT_DOWN && SR.nq) { SR.zone = Z_Q; SR.qfocus = 0; }
        if (strcmp(SR.k.text, SR.last)) {   /* текст изменился: результаты сначала */
            snprintf(SR.last, sizeof SR.last, "%s", SR.k.text);
            search_set(SR.k.text);
            SR.focus = SR.top = 0;
            build();
        }
        return 1;
    }
    if (SR.zone == Z_Q) {
        if (k == KEY_UP) { if (SR.qfocus) SR.qfocus--; else SR.zone = Z_KB; }
        else if (k == KEY_DOWN) { if (SR.qfocus + 1 < SR.nq) SR.qfocus++; }
        else if (k == KEY_RIGHT) { if (selectable(SR.focus)) SR.zone = Z_RES; }
        else if (k == KEY_BACK && ev == EV_SHORT) SR.zone = Z_KB;
        else if (k == KEY_OK && ev == EV_SHORT && SR.qfocus < SR.nq) { char t[200]; snprintf(t, sizeof t, "%s", SR.q[SR.qfocus]); set_text(t); }
        else if (k == KEY_OK && ev == EV_LONG && SR.qfocus < SR.nrecent) { search_forget(SR.q[SR.qfocus]); build(); }
        return 1;
    }
    /* результаты */
    if (k == KEY_UP) move(-1);
    else if (k == KEY_DOWN) move(1);
    else if (k == KEY_CH_UP || k == KEY_CH_DOWN) for (int i = 0; i < 8; i++) move(k == KEY_CH_DOWN ? 1 : -1);
    else if (k == KEY_LEFT || (k == KEY_BACK && ev == EV_SHORT)) SR.zone = Z_KB;
    else if (k == KEY_OK) activate(ev == EV_LONG);
    return 1;
}

static void s_tick(layer *l, double now) {
    (void)l;
    search_tick(now);
    if (SR.built_rev != search_rev()) { build(); app_dirty(); }
}

static void when(const search_hit *h, int sec, char *out, size_t cap) {
    char a[8], b[8], d[40];
    fmt_hm(h->start, a);
    fmt_hm(h->stop, b);
    if (sec == SR_NOW) { snprintf(out, cap, "%s – %s", a, b); return; }
    fmt_day(h->start, d, sizeof d);
    snprintf(out, cap, "%s, %s", d, a);
}

static void draw_results(gfx_t *g, int x, int y, int w, int h) {
    int rh = E(4.6), hh = E(3.2);
    if (!SR.nrows) {
        const char *msg = !SR.k.text[0] ? tr(S_SEARCH_HINT) : search_busy() ? tr(S_SEARCH_BUSY) : tr(S_SEARCH_NONE);
        d_lines(g, x, y + E(0.6), w, 1.35, F_R, C_TEXT3, msg, 3, 0.2);
        if (search_busy() && SR.k.text[0]) d_spinner(g, x + E(1.2), y + E(5), E(1), app_now());
        if (search_busy()) app_dirty();
        return;
    }
    /* прокрутка к фокусу */
    int ypos[MAX_ROWS], acc = 0;
    for (int i = 0; i < SR.nrows; i++) { ypos[i] = acc; acc += SR.rows[i].type == R_HEAD ? hh : rh; }
    int fy = ypos[SR.focus], scroll = 0;
    if (fy + rh > h) scroll = fy + rh - h + E(1);
    if (SR.focus > 0 && SR.rows[SR.focus - 1].type == R_HEAD && fy - hh < scroll) scroll = fy - hh;
    for (int i = 0; i < SR.nrows; i++) {
        int ry = y + ypos[i] - scroll, rr = SR.rows[i].type == R_HEAD ? hh : rh;
        if (ry < y || ry + rr > y + h) continue;   /* только целые строки */
        row *r = &SR.rows[i];
        if (r->type == R_HEAD) {
            d_text(g, x + E(0.4), ry + E(0.9), 1.1, F_S, WA(0.5), r->sec < 0 ? tr(S_SEARCH_CHANNELS) : tr(r->idx), w);
            continue;
        }
        int foc = SR.zone == Z_RES && SR.focus == i, sel = SR.focus == i;
        d_round(g, x, ry, w, rh - E(0.4), E(0.6), foc ? C_WHITE : sel ? C_FILL_ACTIVE : C_FILL);
        uint32_t fg = foc ? 0xff101010u : C_WHITE, fg2 = foc ? 0xff404040u : C_TEXT3;
        search_hit *hit;
        channel *c = row_channel(r, &hit);
        int lw = E(5.6), lh = rh - E(1.2);
        d_logo(g, c, x + E(0.4), ry + E(0.4), lw, lh, E(0.4), 0, foc);
        int tx = x + lw + E(1.4), tw = w - lw - E(2.6);
        if (r->type == R_CH) {
            char nm[300];
            snprintf(nm, sizeof nm, "%d  %s", c->num, c->name);
            d_text(g, tx, ry + E(0.5), 1.3, F_S, fg, nm, tw);
            int pi = repo_prog_at(c, clock_now());
            if (pi >= 0) d_text(g, tx, ry + E(2.4), 1.1, F_R, fg2, repo_prog(c, pi).title, tw);
        } else {
            char wn[80];
            when(hit, r->sec, wn, sizeof wn);
            int iw = 0;
            if (r->sec == SR_NOW) iw = d_badge(g, tx, ry + E(1.25), 1.0, C_LIVE, 0xffffffffu, -1, tr(S_LIVE)) + E(0.6);
            else if (r->sec == SR_ARCHIVE) { d_icon(g, I_ARCHIVE, tx + E(0.7), ry + E(1.25), E(1.2), foc ? 0xff101010u : C_ARCHIVE); iw = E(1.8); }
            int ww = d_text(g, tx + iw, ry + E(0.5), 1.1, F_S, fg2, wn, tw - iw);
            d_text(g, tx + iw + ww + E(0.8), ry + E(0.5), 1.1, F_R, fg2, c->name, tw - iw - ww - E(0.8));
            d_text(g, tx, ry + E(2.3), 1.3, F_S, fg, hit->title, tw);
        }
    }
}

static void s_draw(layer *l, gfx_t *g) {
    (void)l;
    if (SR.built_rev != search_rev()) build();
    d_clear(g, C_SETTINGS_BG);
    int x0 = E(4), w0 = E(36), y = E(2.4);
    d_icon(g, I_SEARCH, x0 + E(0.9), y + E(1.1), E(1.6), C_WHITE);
    d_text(g, x0 + E(2.4), y, 1.8, F_B, C_WHITE, tr(S_SEARCH), w0);
    /* поле */
    int fy = E(6), fh = E(3.6);
    d_round(g, x0, fy, w0, fh, E(0.4), SR.zone == Z_KB ? C_WHITE : C_FILL_BTN);
    uint32_t tc = SR.zone == Z_KB ? 0xff101010u : C_WHITE;
    const char *t = SR.k.text;
    while (*t && d_text_w(1.3, F_R, t) > w0 - E(2.4)) { t++; while ((*t & 0xc0) == 0x80) t++; }
    int tw = 0;
    if (*t) tw = d_text_mid(g, x0 + E(1.2), fy + fh / 2, 1.3, F_R, tc, t, 0);
    else d_text_mid(g, x0 + E(1.2), fy + fh / 2, 1.3, F_R, SR.zone == Z_KB ? 0xff808080u : C_TEXT3, tr(S_SEARCH_PLACEHOLDER), w0 - E(2.4));
    if (SR.zone == Z_KB && (int)(app_now() * 2) % 2 == 0) d_fill(g, x0 + E(1.2) + tw + E(0.1), fy + E(0.7), E(0.2), fh - E(1.4), 0xff101010u);
    if (SR.zone == Z_KB) app_dirty();
    int ky = fy + fh + E(1.2);
    kb_draw(g, &SR.k, x0, ky, w0, SR.zone == Z_KB);
    /* запросы */
    int qy = ky + kb_height() + E(1.4);
    if (SR.nq) {
        d_text(g, x0 + E(0.4), qy, 1.1, F_S, WA(0.5), tr(S_SEARCH_QUERIES), w0);
        qy += E(2.4);
        for (int i = 0; i < SR.nq; i++) {
            int ry = qy + i * E(2.8);
            if (ry + E(2.6) > SH - E(1.6)) break;
            int foc = SR.zone == Z_Q && SR.qfocus == i;
            if (foc) d_round(g, x0, ry, w0, E(2.6), E(0.4), C_WHITE);
            uint32_t c = foc ? 0xff101010u : C_TEXT2;
            d_icon(g, i < SR.nrecent ? I_HISTORY : I_SEARCH, x0 + E(1.2), ry + E(1.3), E(1.2), c);
            d_text_mid(g, x0 + E(2.6), ry + E(1.3), 1.2, F_R, c, SR.q[i], w0 - E(3.4));
        }
    }
    int x1 = x0 + w0 + E(3), w1 = SW - E(4) - x1;
    draw_results(g, x1, fy, w1, SH - E(2.4) - fy);
}

static void s_closed(layer *l) { (void)l; search_clear(); }

void search_open_query(const char *q) {
    search_open();
    set_text(q);
}

void search_open(void) {
    SL.name = "search";
    SL.key = s_key;
    SL.has_long = s_has_long;
    SL.draw = s_draw;
    SL.tick = s_tick;
    SL.closed = s_closed;
    SL.covers = 1;
    memset(&SR, 0, sizeof SR);
    SR.k.edges = 1;
    SR.k.lang = lang_is_ru();
    search_clear();
    build();
    layer_push(&SL);
}
