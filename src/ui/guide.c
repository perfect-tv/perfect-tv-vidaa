/* ТВ-гид (docs/controls.md, «ТВ-гид»): шапка с передачей, список каналов категории и справа — сетка программы
 * (окно 2 ч, шаг 30 мин, «сейчас» на 25 %) или вид «Каналы + программа» (передачи канала в фокусе и даты).
 * ← из каналов — колонка категорий (ещё ← — плейлисты), всё сдвигается вправо. */
#include "screens.h"
#include "layer.h"
#include "keys.h"
#include "tv.h"
#include "u.h"
#include "app.h"
#include "core/clock.h"
#include "core/lang.h"
#include "core/prefs.h"
#include "data/loaders.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { Z_CH, Z_GRID, Z_PROGS, Z_DATES, Z_CATS, Z_PLS };
#define WINDOW 7200
#define SLOT 1800

static struct {
    int zone, gi, ci, top;
    int64_t ws;            /* начало окна сетки */
    int64_t ft;            /* время передачи в фокусе сетки */
    int live_window;
    int pi, ptop;          /* вид «Список»: передача в фокусе, верх списка */
    int day;               /* вид «Список»: день в фокусе (смещение от сегодня) */
    int cat, cat_top, saved_gi, saved_ci;
    int pl, saved_pl;
    int rows, row_h, list_y;
    int rev;
} G;

static layer L;

int guide_is_open(void) { return layer_has(&L); }
static group *cur_group(void) { return G.gi >= 0 && G.gi < R.ngroups ? &R.groups[G.gi] : NULL; }
static channel *cur_channel(void) { group *g = cur_group(); return g && G.ci >= 0 && G.ci < g->n ? g->ch[G.ci] : NULL; }
static const char *group_title(const group *g) {
    if (!g) return "";
    if (g->kind == GK_FAV) return tr(S_FAVOURITES);
    if (g->kind == GK_ALL) return tr(S_ALL_CHANNELS);
    return g->title ? g->title : tr(S_NO_GROUP);
}
static int64_t align(int64_t t) { return t - ((t % SLOT) + SLOT) % SLOT; }
static int list_mode(void) { return P.guide_mode == GUIDE_LIST; }

static void fit(void) {
    group *g = cur_group();
    int n = g ? g->n : 0;
    if (G.ci >= n) G.ci = n - 1;
    if (G.ci < 0) G.ci = 0;
    if (G.rows < 1) G.rows = 1;
    if (G.top > G.ci) G.top = G.ci;
    if (G.ci >= G.top + G.rows) G.top = G.ci - G.rows + 1;
    if (G.top > n - G.rows) G.top = n - G.rows;
    if (G.top < 0) G.top = 0;
}

/* вид «Список»: передача, идущая сейчас (или ближайшая), у канала в фокусе */
static void progs_to_now(void) {
    channel *c = cur_channel();
    int64_t t = T.cur == c && T.arc ? tv_position() : clock_now();
    int i = repo_prog_at(c, t);
    if (i < 0) i = repo_prog_after(c, t);
    if (i < 0) i = repo_prog_count(c) - 1;
    G.pi = i < 0 ? 0 : i;
    G.ptop = G.pi - 3;
}

static void sync_from_tv(void) {
    G.gi = 0;
    G.ci = 0;
    group *g = repo_group(T.group);
    if (g) G.gi = repo_group_index(g);
    if (T.cur) {
        int i = repo_index_in(cur_group(), T.cur);
        if (i < 0) { g = repo_group_of(T.cur, NULL); G.gi = repo_group_index(g); i = repo_index_in(g, T.cur); }
        G.ci = i < 0 ? 0 : i;
    }
    G.top = G.ci - G.rows / 2;
    fit();
}

/* ---------- клавиши ---------- */
static void play_focused(void) {
    channel *c = cur_channel();
    group *g = cur_group();
    if (!c) return;
    layer_pop(&L);
    tv_play(c, g ? g->key : NULL);
}

static int first_overlap(const channel *c, int64_t t) {
    int i = repo_prog_at(c, t);
    return i >= 0 ? i : repo_prog_after(c, t);
}

static void grid_enter(void) {
    channel *c = cur_channel();
    int64_t t = T.cur == c && T.arc ? tv_position() : clock_now();
    G.zone = Z_GRID;
    G.live_window = 0;
    G.ft = t;
    if (G.ws > t || t >= G.ws + WINDOW) G.ws = align(t - SLOT);
}
/* передача (или пустой получасовой слот), в которой ft */
static void focus_range(const channel *c, int64_t *s, int64_t *e) {
    int i = repo_prog_at(c, G.ft);
    if (i >= 0) { prog p = repo_prog(c, i); *s = p.start; *e = p.stop; return; }
    *s = align(G.ft);
    *e = *s + SLOT;
    int n = repo_prog_after(c, *s);
    if (n >= 0) { prog p = repo_prog(c, n); if (p.start < *e) *e = p.start; }
    int pv = repo_prog_at(c, *s - 1);
    if (pv >= 0) { prog p = repo_prog(c, pv); if (p.stop > *s && p.stop <= G.ft) *s = p.stop; }
}
static void grid_step(int dir) {
    channel *c = cur_channel();
    int64_t s, e;
    focus_range(c, &s, &e);
    G.ft = dir > 0 ? e : s - 1;
    int64_t ns, ne;
    focus_range(c, &ns, &ne);
    G.ft = ns > G.ft ? ns : G.ft;
    if (dir > 0 && ns < G.ws + WINDOW && ne > G.ws + WINDOW) G.ws = align(ns) > G.ws ? align(ns) : G.ws + SLOT;
    else if (dir > 0 && ns >= G.ws + WINDOW) G.ws = align(ns);
    if (dir < 0 && ns < G.ws) G.ws = align(ns);
    int64_t lo = day_start(clock_now()) - 14 * 86400;
    if (G.ws < lo) G.ws = lo;
}

static void grid_ok(void) {
    channel *c = cur_channel();
    int i = repo_prog_at(c, G.ft);
    if (i < 0) return;
    prog p = repo_prog(c, i);
    card_open(c, &p);
}

static void progs_ok(int long_press) {
    channel *c = cur_channel();
    group *g = cur_group();
    if (!c || G.pi >= repo_prog_count(c)) return;
    prog p = repo_prog(c, G.pi);
    int64_t now = clock_now();
    if (!long_press && p.start <= now && now < p.stop) { layer_pop(&L); tv_play(c, g ? g->key : NULL); return; }
    if (!long_press && p.stop <= now && repo_has_archive(c) && p.start >= now - (int64_t)c->archive_days * 86400) {
        layer_pop(&L);
        tv_play_archive(c, p.start, g ? g->key : NULL);
        return;
    }
    card_open(c, &p);
}

static int day_of(int64_t t) { return (int)((day_start(t) - day_start(clock_now()) + 43200) / 86400); }
static void dates_to_progs(void) {
    channel *c = cur_channel();
    int64_t ds = day_start(clock_now()) + (int64_t)G.day * 86400 + 3600;
    ds = day_start(ds);
    int now_i = repo_prog_at(c, clock_now());
    if (now_i >= 0 && day_of(repo_prog(c, now_i).start) == G.day) G.pi = now_i;
    else {
        int i = repo_prog_after(c, ds);
        if (i < 0) i = repo_prog_count(c) - 1;
        G.pi = i < 0 ? 0 : i;
    }
    G.ptop = G.pi - 2;
    G.zone = Z_PROGS;
}

static void cats_open(void) {
    G.saved_gi = G.gi;
    G.saved_ci = G.ci;
    G.cat = G.gi;
    G.zone = Z_CATS;
}
static void cats_preview(void) {
    channel *c = cur_channel();
    G.gi = G.cat;
    int i = repo_index_in(cur_group(), c);
    G.ci = i >= 0 ? i : 0;
    G.top = G.ci - G.rows / 2;
    fit();
}

static void switch_playlist(int i) {
    if (i == R.cur || i < 0 || i >= R.npl) { G.zone = Z_CATS; return; }
    loaders_cancel();
    R.cur = i;
    layer_pop(&L);
    char path[700];
    repo_channels_path(R.pl[i].id, path, sizeof path);
    if (!file_exists(path)) { repo_select(i); load_playlist(i, 1); return; }
    tv_playlist_changed(0);
    if (!guide_is_open()) guide_open(0);
    load_epg(0);
}

static int has_long(layer *l, int k) {
    (void)l;
    if (k == KEY_OK) return G.zone == Z_CH || G.zone == Z_PROGS || G.zone == Z_CATS;
    if (k == KEY_LEFT) return G.zone == Z_CH && !list_mode();
    return 0;
}

static int key(layer *l, int k, int ev) {
    (void)l;
    group *g = cur_group();
    int n = g ? g->n : 0;
    if (k == KEY_MENU || (k == KEY_OK && ev == EV_LONG && G.zone != Z_PROGS)) {
        channel *c = cur_channel();
        if (c) tv_open_menu(c, g ? g->key : NULL, 1);
        return 1;
    }
    switch (G.zone) {
    case Z_CH:
        if (k == KEY_UP || k == KEY_DOWN) { if (n) G.ci = ((G.ci + (k == KEY_DOWN ? 1 : -1)) % n + n) % n; fit(); if (list_mode()) progs_to_now(); }
        else if (k == KEY_CH_UP || k == KEY_CH_DOWN) { G.ci += k == KEY_CH_DOWN ? G.rows : -G.rows; fit(); if (list_mode()) progs_to_now(); }
        else if (k == KEY_LEFT && ev == EV_LONG) { channel *c = cur_channel(); if (c) tv_open_schedule(c, 0); }
        else if (k == KEY_LEFT) cats_open();
        else if (k == KEY_RIGHT) { if (list_mode()) { progs_to_now(); G.zone = Z_PROGS; } else grid_enter(); }
        else if (k == KEY_OK && ev == EV_SHORT) play_focused();
        else if (IS_DIGIT(k) && ev == EV_SHORT) {
            static char buf[6];
            static double at;
            double t = app_now();
            if (t - at > 1.5) buf[0] = 0;
            at = t;
            size_t bl = strlen(buf);
            if (bl < 4) { buf[bl] = (char)('0' + k - KEY_D0); buf[bl + 1] = 0; }
            int num = atoi(buf);
            for (int i = 0; i < n; i++) if (g->ch[i]->num == num) { G.ci = i; fit(); break; }
        } else if (k == KEY_BACK && ev == EV_SHORT) layer_pop(&L);
        else if (k == KEY_GREEN || k == KEY_GUIDE) layer_pop(&L);
        else if (k == KEY_SEARCH) search_open();
        return 1;
    case Z_GRID:
        if (k == KEY_LEFT || k == KEY_RIGHT) grid_step(k == KEY_RIGHT ? 1 : -1);
        else if (k == KEY_UP || k == KEY_DOWN) { if (n) G.ci = ((G.ci + (k == KEY_DOWN ? 1 : -1)) % n + n) % n; fit(); }
        else if (k == KEY_CH_UP || k == KEY_CH_DOWN) { G.ci += k == KEY_CH_DOWN ? G.rows : -G.rows; fit(); }
        else if (k == KEY_OK) grid_ok();
        else if (k == KEY_BACK && ev == EV_SHORT) { G.zone = Z_CH; G.live_window = 1; }
        return 1;
    case Z_PROGS: {
        channel *c = cur_channel();
        int pn = repo_prog_count(c);
        if (k == KEY_UP || k == KEY_DOWN) { G.pi += k == KEY_DOWN ? 1 : -1; }
        else if (k == KEY_CH_UP || k == KEY_CH_DOWN) G.pi += k == KEY_CH_DOWN ? 8 : -8;
        else if (k == KEY_OK) progs_ok(ev == EV_LONG);
        else if (k == KEY_LEFT || (k == KEY_BACK && ev == EV_SHORT)) G.zone = Z_CH;
        else if (k == KEY_RIGHT && pn) { G.day = day_of(repo_prog(c, G.pi).start); G.zone = Z_DATES; }
        if (G.pi >= pn) G.pi = pn - 1;
        if (G.pi < 0) G.pi = 0;
        return 1;
    }
    case Z_DATES:
        if (k == KEY_UP || k == KEY_DOWN) {
            channel *c = cur_channel();
            int pn = repo_prog_count(c);
            int lo = pn ? day_of(repo_prog(c, 0).start) : 0, hi = pn ? day_of(repo_prog(c, pn - 1).start) : 0;
            G.day += k == KEY_DOWN ? 1 : -1;
            if (G.day < lo) G.day = lo;
            if (G.day > hi) G.day = hi;
        } else if (k == KEY_LEFT || k == KEY_OK || (k == KEY_BACK && ev == EV_SHORT)) dates_to_progs();
        return 1;
    case Z_CATS:
        if (k == KEY_UP || k == KEY_DOWN) { if (R.ngroups) G.cat = ((G.cat + (k == KEY_DOWN ? 1 : -1)) % R.ngroups + R.ngroups) % R.ngroups; cats_preview(); }
        else if (k == KEY_CH_UP || k == KEY_CH_DOWN) { G.cat += k == KEY_CH_DOWN ? 10 : -10; G.cat = CLAMP(G.cat, 0, R.ngroups - 1); cats_preview(); }
        else if (k == KEY_RIGHT || (k == KEY_OK && ev == EV_SHORT)) { cats_preview(); G.zone = Z_CH; if (list_mode()) progs_to_now(); }
        else if (k == KEY_LEFT && R.npl > 1) { G.saved_pl = R.cur; G.pl = R.cur; G.zone = Z_PLS; }
        else if (k == KEY_BACK && ev == EV_SHORT) { G.gi = G.saved_gi; G.ci = G.saved_ci; fit(); G.zone = Z_CH; }
        return 1;
    case Z_PLS:
        if (k == KEY_UP || k == KEY_DOWN) G.pl = ((G.pl + (k == KEY_DOWN ? 1 : -1)) % R.npl + R.npl) % R.npl;
        else if (k == KEY_OK || k == KEY_RIGHT) switch_playlist(G.pl);
        else if (k == KEY_BACK && ev == EV_SHORT) G.zone = Z_CATS;
        return 1;
    }
    return 1;
}

static void tick(layer *l, double now) {
    (void)l;
    (void)now;
    if (G.rev != R.rev) {   /* каналы или категории изменились (скрыли, избранное) */
        G.rev = R.rev;
        if (G.gi >= R.ngroups) G.gi = R.ngroups - 1;
        if (G.gi < 0) G.gi = 0;
        fit();
    }
}

/* ---------- рисование ---------- */
static void draw_header(gfx_t *g, int x, int y, int w, channel *c) {
    char clock[8];
    fmt_hm(clock_now(), clock);
    d_text_r(g, SW - E(4), y - E(0.3), 2.2, F_S, C_WHITE, clock);
    if (!c || P.guide_header == 2) return;
    int lw = E(9.6), lh = E(6.6);
    d_logo(g, c, x, y, lw, lh, E(0.6), 1, 0);
    int tx = x + lw + E(1.6), tw = w - lw - E(1.6) - E(8);
    prog p;
    memset(&p, 0, sizeof p);
    int has = 0;
    if (G.zone == Z_GRID) { int i = repo_prog_at(c, G.ft); if (i >= 0) { p = repo_prog(c, i); has = 1; } }
    else if (G.zone == Z_PROGS && G.pi < repo_prog_count(c)) { p = repo_prog(c, G.pi); has = 1; }
    else { int i = repo_prog_at(c, clock_now()); if (i >= 0) { p = repo_prog(c, i); has = 1; } }
    char name[300];
    if (P.number_mode == NUM_NEAR_NAME) snprintf(name, sizeof name, "%d. %s", c->num, c->name);
    else snprintf(name, sizeof name, "%s", c->name);
    if (P.guide_header == 1 || !has) {
        d_text(g, tx, y, 1.6, F_B, C_WHITE, name, tw);
        group *gr = repo_group_of(c, NULL);
        char sub[300];
        snprintf(sub, sizeof sub, "%s%s%s", group_title(gr), repo_has_archive(c) ? "  ·  " : "", repo_has_archive(c) ? trp(S_DAYS, c->archive_days) : "");
        d_text(g, tx, y + E(2.6), 1.2, F_R, C_TEXT3, sub, tw);
        if (!has && repo_prog_count(c) == 0) d_text(g, tx, y + E(4.6), 1.2, F_R, C_TEXT3, tr(S_NO_EPG), tw);
        return;
    }
    char a1[8], a2[8], d[32], line[400];
    fmt_hm(p.start, a1);
    fmt_hm(p.stop, a2);
    fmt_day(p.start, d, sizeof d);
    snprintf(line, sizeof line, "%s  ·  %s, %s – %s", name, d, a1, a2);
    d_text(g, tx, y - E(0.2), 1.2, F_S, C_TEXT3, line, tw);
    d_text(g, tx, y + E(1.6), 1.8, F_B, C_WHITE, p.title, tw);
    if (p.desc[0]) d_lines(g, tx, y + E(4.2), tw, 1.15, F_R, C_TEXT2, p.desc, 2, 0);
}

static void draw_row(gfx_t *g, channel *c, int x, int y, int w, int h, int focused, int active, int compact) {
    int playing = c == T.cur;
    uint32_t bg = focused ? (active ? C_WHITE : C_FILL_ACTIVE) : playing ? C_FILL_MARKED : C_FILL;
    d_round(g, x, y, w, h - E(0.3), E(0.6), bg);
    uint32_t fg = focused && active ? 0xff101010u : C_WHITE, fg2 = focused && active ? 0xff404040u : C_TEXT3;
    int lx = x + E(0.25), ly = y + E(0.25), lh = h - E(0.8), lw = lh * 96 / 66;
    int tx = x + E(0.6);
    if (P.number_mode == NUM_NEAR_NAME) {
        char n[16];
        snprintf(n, sizeof n, "%d", c->num);
        d_text_mid(g, tx, y + (h - E(0.3)) / 2, 1.25, F_S, fg2, n, 0);
        tx += E(3.4);
        lx = tx - E(0.35);
    }
    d_logo(g, c, lx, ly, lw, lh, E(0.35), 1, focused && active);
    tx = lx + lw + E(0.9);
    int cy = y + (h - E(0.3)) / 2;
    int rx = x + w - E(0.8);
    if (repo_is_locked(c)) { d_icon(g, I_LOCK, rx - E(0.7), cy, E(1.3), fg2); rx -= E(2.0); }
    if (compact) { d_text_mid(g, tx, cy, 1.3, focused ? F_B : F_S, fg, c->name, rx - tx); return; }
    int i = repo_prog_at(c, clock_now());
    if (i < 0) { d_text_mid(g, tx, cy, 1.3, focused ? F_B : F_S, fg, c->name, rx - tx); return; }
    prog p = repo_prog(c, i);
    d_text_mid(g, tx, cy - E(0.8), 1.25, focused ? F_B : F_S, fg, c->name, rx - tx);
    d_text_mid(g, tx, cy + E(0.9), 1.1, F_R, fg2, p.title, rx - tx - E(5));
    double frac = (double)(clock_now() - p.start) / (double)(p.stop - p.start);
    int bw = E(3.6), bx = rx - bw;
    d_round(g, bx, cy + E(0.75), bw, E(0.3), E(0.15), focused && active ? 0x40000000u : C_FILL_MARKED);
    d_round(g, bx, cy + E(0.75), (int)(bw * (frac < 0 ? 0 : frac > 1 ? 1 : frac)), E(0.3), E(0.15), focused && active ? 0xff202020u : C_WHITE);
}

static void draw_grid(gfx_t *g, int x, int y, int w) {
    int64_t now = clock_now();
    if (G.zone == Z_CH || G.live_window) {
        channel *fc = cur_channel();
        int64_t base = fc && fc == T.cur && T.arc ? tv_position() : now;
        G.ws = base - WINDOW / 4;
    }
    int64_t ws = G.ws, we = ws + WINDOW;
    double ppx = (double)w / WINDOW;
    /* линейка времени */
    int ry = y - E(2.4);
    channel *fc0 = cur_channel();
    int64_t line_t = fc0 && fc0 == T.cur && T.arc ? tv_position() : now;
    for (int64_t t = align(ws) + (align(ws) < ws ? SLOT : 0); t < we; t += SLOT) {
        int tx = x + (int)((t - ws) * ppx);
        int lx = x + (int)((line_t - ws) * ppx);
        if (tx > lx - E(5.5) && tx < lx + E(3)) continue;   /* подпись под плашкой «сейчас» не рисуется */
        char hm[8];
        fmt_hm(t, hm);
        d_fill(g, tx, ry + E(1.6), 1, E(0.6), C_LINE_STRONG);
        d_text(g, tx + E(0.4), ry, 1.15, F_R, C_TEXT3, hm, 0);
    }
    group *gr = cur_group();
    gfx_push_clip(g, x, y - E(2.6), w, G.rows * G.row_h + E(2.6));
    for (int r = 0; r < G.rows; r++) {
        int i = G.top + r;
        if (!gr || i >= gr->n) break;
        channel *c = gr->ch[i];
        int cy = y + r * G.row_h, ch = G.row_h - E(0.3);
        int k = first_overlap(c, ws), pn = repo_prog_count(c);
        int64_t covered = ws;
        int focus_row = i == G.ci && G.zone == Z_GRID;
        int64_t fs = 0, fe = 0;
        if (focus_row) focus_range(c, &fs, &fe);
        /* пустые получасовые слоты и передачи */
        while (covered < we) {
            int64_t s, e;
            prog p;
            int is_prog = 0;
            if (k >= 0 && k < pn && (p = repo_prog(c, k)).start <= covered + 1) { s = p.start; e = p.stop; is_prog = 1; k++; }
            else {
                s = covered;
                e = align(covered) + SLOT;
                if (k >= 0 && k < pn) { prog q = repo_prog(c, k); if (q.start < e) e = q.start; }
            }
            if (e <= covered) { covered++; continue; }
            int x0 = x + (int)(((s < ws ? ws : s) - ws) * ppx), x1 = x + (int)(((e > we ? we : e) - ws) * ppx);
            covered = e;
            if (x1 - x0 < 2) continue;
            int foc = focus_row && s <= G.ft && G.ft < e;
            uint32_t bg = foc ? C_WHITE : !is_prog ? C_FILL : e <= now ? C_FILL : s <= now ? C_FILL_NOW : C_FILL_CELL;
            d_round(g, x0 + E(0.15), cy, x1 - x0 - E(0.3), ch, E(0.5), bg);
            if (is_prog) {
                uint32_t fg = foc ? 0xff101010u : e <= now ? WA(0.45) : C_WHITE;
                int tx = x0 + E(0.8), tw = x1 - x0 - E(1.4);
                int ic = 0;
                if (repo_reminder_find(c->key, s) >= 0) ic = I_CLOCK;
                else if (e <= now && repo_has_archive(c) && s >= now - (int64_t)c->archive_days * 86400) ic = I_ARCHIVE;
                if (ic && tw > E(3)) {
                    int playing_arc = T.cur == c && T.arc && s <= tv_position() && tv_position() < e;
                    d_icon(g, ic, tx + E(0.6), cy + ch / 2, E(1.3), foc ? 0xff101010u : playing_arc || ic == I_CLOCK ? C_ARCHIVE : WA(0.6));
                    tx += E(1.8);
                    tw -= E(1.8);
                }
                if (tw > E(1.2)) d_text_mid(g, tx, cy + ch / 2, 1.25, foc ? F_B : F_R, fg, p.title, tw);
            }
        }
    }
    /* «сейчас» и, если смотрим архив на этом канале, янтарная линия кадра */
    channel *fc = cur_channel();
    int arc_line = fc && fc == T.cur && T.arc;
    int64_t lt = arc_line ? tv_position() : now;
    if (lt >= ws && lt < we) {
        int lx = x + (int)((lt - ws) * ppx);
        uint32_t col = arc_line ? C_ARCHIVE : C_LIVE;
        d_fill(g, lx - 1, y - E(0.6), E(0.15) > 2 ? E(0.15) : 2, G.rows * G.row_h, col);
        char hm[8];
        fmt_hm(lt, hm);
        int bw = d_text_w(1.1, F_B, hm) + E(1.0);
        d_round(g, lx - bw / 2, ry - E(0.1), bw, E(2.0), E(0.4), col);
        d_text_c(g, lx - bw / 2, bw, ry + E(0.1), 1.1, F_B, arc_line ? C_ON_ARCHIVE : C_WHITE, hm);
    }
    gfx_pop_clip(g);
}

static void draw_progs(gfx_t *g, int x, int y, int w, int h, int dates_x) {
    channel *c = cur_channel();
    int pn = repo_prog_count(c);
    int64_t now = clock_now();
    int row = E(3.4), rows = h / row;
    if (!pn) { d_text(g, x + E(1), y + E(1), 1.3, F_R, C_TEXT3, tr(S_NO_PROGRAMMES), w - E(2)); return; }
    if (G.ptop > G.pi) G.ptop = G.pi;
    if (G.pi >= G.ptop + rows) G.ptop = G.pi - rows + 1;
    if (G.ptop > pn - rows) G.ptop = pn - rows;
    if (G.ptop < 0) G.ptop = 0;
    int active = G.zone == Z_PROGS;
    int64_t play_t = T.cur == c ? tv_position() : -1;
    for (int r = 0; r < rows && G.ptop + r < pn; r++) {
        int i = G.ptop + r;
        prog p = repo_prog(c, i);
        int ry = y + r * row, foc = i == G.pi && (active || G.zone == Z_DATES);
        int live = p.start <= now && now < p.stop, playing = play_t >= p.start && play_t < p.stop;
        uint32_t bg = foc ? (active ? C_WHITE : C_FILL_ACTIVE) : playing ? C_FILL_PLAYING : live ? C_FILL_NOW : 0;
        if (bg) d_round(g, x, ry, w, row - E(0.3), E(0.5), bg);
        if (foc && !active) d_stroke(g, x, ry, w, row - E(0.3), E(0.5), E(0.15), C_WHITE);
        uint32_t fg = foc && active ? 0xff101010u : p.stop <= now ? WA(0.55) : C_WHITE;
        int cy = ry + (row - E(0.3)) / 2;
        char hm[8];
        fmt_hm(p.start, hm);
        d_text_mid(g, x + E(1), cy, 1.3, playing ? F_B : F_R, fg, hm, 0);
        int tx = x + E(5.6);
        int ic = -1;
        uint32_t icol = fg;
        if (live) { ic = I_LIVE; icol = foc && active ? fg : T.cur == c && !T.arc ? C_LIVE : C_WHITE; }
        else if (p.stop <= now && repo_has_archive(c) && p.start >= now - (int64_t)c->archive_days * 86400) { ic = I_ARCHIVE; if (playing && !(foc && active)) icol = C_ARCHIVE; }
        else if (repo_reminder_find(c->key, p.start) >= 0) { ic = I_CLOCK; if (!(foc && active)) icol = C_ARCHIVE; }
        if (ic >= 0) d_icon(g, ic, tx + E(0.7), cy, E(1.5), icol);
        tx += E(2.2);
        d_text_mid(g, tx, cy, 1.3, playing ? F_B : F_R, fg, p.title, x + w - tx - E(1));
        if (live) {
            double frac = (double)(now - p.start) / (double)(p.stop - p.start);
            d_fill(g, tx, cy + E(1.1), (int)((x + w - tx - E(1)) * frac), E(0.2), foc && active ? 0xff303030u : WA(0.7));
        }
    }
    /* даты */
    if (dates_x > 0) {
        int lo = day_of(repo_prog(c, 0).start), hi = day_of(repo_prog(c, pn - 1).start);
        int cur_day = G.zone == Z_DATES ? G.day : day_of(repo_prog(c, G.pi).start);
        int dh = E(4.2), dy = y;
        int first = cur_day - (h / dh) / 2;
        if (first > hi - h / dh + 1) first = hi - h / dh + 1;
        if (first < lo) first = lo;
        for (int d = first; d <= hi && dy + dh <= y + h; d++, dy += dh) {
            int64_t t = day_start(clock_now()) + (int64_t)d * 86400 + 7200;
            char wd[16], dm[24];
            fmt_day_short(t, wd, sizeof wd, dm, sizeof dm);
            int foc = d == cur_day;
            int dw = SW - E(4) - dates_x;
            uint32_t bg = foc ? (G.zone == Z_DATES ? C_WHITE : C_FILL_ACTIVE) : d == 0 ? C_FILL_MARKED : 0;
            if (bg) d_round(g, dates_x, dy, dw, dh - E(0.3), E(0.5), bg);
            if (foc && G.zone != Z_DATES) d_stroke(g, dates_x, dy, dw, dh - E(0.3), E(0.5), E(0.15), C_WHITE);
            uint32_t fg = foc && G.zone == Z_DATES ? 0xff101010u : C_WHITE;
            d_text_c(g, dates_x, dw, dy + E(0.4), 1.15, F_S, fg, d == 0 ? tr(S_TODAY) : wd);
            d_text_c(g, dates_x, dw, dy + E(2.1), 1.1, F_R, foc && G.zone == Z_DATES ? 0xff404040u : C_TEXT3, dm);
        }
    }
}

static void draw_cats(gfx_t *g, int x, int y, int w, int h) {
    int row = E(3.2), rows = h / row;
    if (G.cat_top > G.cat) G.cat_top = G.cat;
    if (G.cat >= G.cat_top + rows) G.cat_top = G.cat - rows + 1;
    if (G.cat_top < 0) G.cat_top = 0;
    d_text(g, x, y - E(2.8), 1.3, F_B, C_TEXT3, tr(S_CATEGORIES), w);
    for (int r = 0; r < rows && G.cat_top + r < R.ngroups; r++) {
        int i = G.cat_top + r;
        group *gr = &R.groups[i];
        int ry = y + r * row, foc = i == G.cat;
        uint32_t bg = foc ? (G.zone == Z_CATS ? C_WHITE : C_FILL_ACTIVE) : i == G.gi ? C_FILL_MARKED : 0;
        if (bg) d_round(g, x, ry, w, row - E(0.3), E(0.5), bg);
        uint32_t fg = foc && G.zone == Z_CATS ? 0xff101010u : C_WHITE;
        int cy = ry + (row - E(0.3)) / 2, tx = x + E(1);
        if (gr->kind == GK_FAV) { d_icon(g, I_HEART, tx + E(0.6), cy, E(1.2), foc && G.zone == Z_CATS ? fg : C_LIVE); tx += E(1.8); }
        else if (repo_group_locked(gr->key) && P.pin[0]) { d_icon(g, I_LOCK, tx + E(0.6), cy, E(1.2), fg); tx += E(1.8); }
        char n[16];
        snprintf(n, sizeof n, "%d", gr->n);
        int nw = d_text_w(1.15, F_R, n);
        d_text_mid(g, tx, cy, 1.3, foc ? F_B : F_R, fg, group_title(gr), x + w - tx - nw - E(2));
        d_text_mid(g, x + w - E(1) - nw, cy, 1.15, F_R, foc && G.zone == Z_CATS ? 0xff404040u : C_TEXT3, n, 0);
    }
}

static void draw_pls(gfx_t *g, int x, int y, int w) {
    int row = E(3.6);
    d_text(g, x, y - E(2.8), 1.3, F_B, C_TEXT3, tr(S_PLAYLISTS), w);
    for (int i = 0; i < R.npl; i++) {
        int ry = y + i * row, foc = i == G.pl;
        uint32_t bg = foc ? C_WHITE : i == R.cur ? C_FILL_MARKED : 0;
        if (bg) d_round(g, x, ry, w, row - E(0.3), E(0.5), bg);
        uint32_t fg = foc ? 0xff101010u : C_WHITE;
        d_text(g, x + E(1), ry + E(0.3), 1.25, F_S, fg, R.pl[i].name, w - E(2));
        d_text(g, x + E(1), ry + E(1.9), 1.05, F_R, foc ? 0xff404040u : C_TEXT3, trp(S_CHANNELS_N, R.pl[i].count), w - E(2));
    }
}

static void draw_hints(gfx_t *g) {
    if (!P.hints) return;
    hint h[6];
    int n = 0;
    switch (G.zone) {
    case Z_CH:
        h[n++] = (hint){-1, tr(S_K_OK), tr(S_H_WATCH)};
        h[n++] = (hint){I_LEFT, NULL, tr(S_H_CATEGORIES)};
        h[n++] = (hint){I_RIGHT, NULL, tr(S_H_PROGRAMME)};
        h[n++] = (hint){-1, tr(S_K_MENU), tr(S_H_MENU)};
        h[n++] = (hint){-1, tr(S_K_BACK), tr(S_H_CLOSE)};
        break;
    case Z_GRID: case Z_PROGS:
        h[n++] = (hint){-1, tr(S_K_OK), G.zone == Z_GRID ? tr(S_H_CARD) : tr(S_H_WATCH)};
        if (G.zone == Z_PROGS) h[n++] = (hint){-1, tr(S_K_HOLD_OK), tr(S_H_CARD)};
        if (G.zone == Z_PROGS) h[n++] = (hint){I_RIGHT, NULL, tr(S_H_DAYS)};
        h[n++] = (hint){-1, tr(S_K_BACK), tr(S_H_BACK)};
        break;
    default:
        h[n++] = (hint){-1, tr(S_K_OK), tr(S_OK)};
        h[n++] = (hint){-1, tr(S_K_BACK), tr(S_H_BACK)};
        break;
    }
    d_hints(g, E(4), SH - E(2.4) - E(0.85), h, n);
}

static void draw(layer *l, gfx_t *g) {
    (void)l;
    d_vgrad(g, 0, 0, SW, SH, C_GUIDE_TOP, C_GUIDE_BOTTOM);
    int header_h = P.guide_header == 2 ? E(2.4) : E(10.4);
    int hint_h = P.hints ? E(4.4) : E(2.4);
    int shift = G.zone == Z_CATS ? E(20) : G.zone == Z_PLS ? E(40) : 0;
    group *gr = cur_group();
    channel *c = cur_channel();
    draw_header(g, E(4), E(2.4), SW - E(8), c);
    /* заголовок категории */
    int ty = E(2.4) + header_h - E(1.0) + E(0.6);
    int lx = E(4) + shift;
    char cnt[16];
    snprintf(cnt, sizeof cnt, "%d", gr ? gr->n : 0);
    if (G.zone != Z_CATS && G.zone != Z_PLS) d_icon(g, I_LEFT, lx + E(0.5), ty + E(1.0), E(1.0), C_TEXT3);
    int tw = d_text(g, lx + E(1.6), ty, 1.4, F_B, C_WHITE, group_title(gr), E(30));
    d_text(g, lx + E(1.6) + tw + E(0.8), ty + E(0.15), 1.4, F_R, WA(0.5), cnt, 0);
    long progs = 0;
    int pr = loaders_epg_progress(&progs);
    if (repo_epg_loading()) {
        const char *s = pr >= 0 ? trf(S_EPG_LOADING, pr) : trf(S_EPG_LOADING_N, trp(S_PROGRAMMES_N, progs));
        d_text_r(g, SW - E(4), ty + E(0.3), 1.1, F_R, C_TEXT3, s);
    }
    G.list_y = ty + E(3.6);
    int avail = SH - hint_h - G.list_y;
    G.row_h = E(4.3);
    G.rows = avail / G.row_h;
    fit();
    int cw = list_mode() ? E(30) : E(24);
    if (shift) {
        if (G.zone == Z_PLS) draw_pls(g, E(4), G.list_y, E(18));
        draw_cats(g, E(4) + (G.zone == Z_PLS ? E(20) : 0), G.list_y, E(19), avail);
    }
    for (int r = 0; r < G.rows; r++) {
        int i = G.top + r;
        if (!gr || i >= gr->n) break;
        draw_row(g, gr->ch[i], lx, G.list_y + r * G.row_h, cw, G.row_h, i == G.ci, G.zone == Z_CH, !list_mode());
    }
    if (!gr || !gr->n) d_text(g, lx + E(1), G.list_y + E(1), 1.3, F_R, C_TEXT3, tr(S_EMPTY_CATEGORY), cw);
    int px = lx + cw + E(1.2);
    if (list_mode()) {
        int dates_w = E(8);
        int pw = SW - E(4) - dates_w - E(1) - px;
        if (pw > E(10)) draw_progs(g, px, G.list_y, pw, avail, SW - E(4) - dates_w);
    } else {
        int gw = SW - E(4) - px;
        if (gw > E(10)) draw_grid(g, px, G.list_y, gw);
    }
    draw_hints(g);
}

static void closed(layer *l) { (void)l; }

void guide_open(int to_programmes) {
    L.name = "guide";
    L.key = key;
    L.has_long = has_long;
    L.draw = draw;
    L.tick = tick;
    L.closed = closed;
    L.covers = 0;   /* под гидом видно видео (затемнение сверху плотнее) */
    G.rows = 10;
    G.rev = R.rev;
    sync_from_tv();
    G.zone = Z_CH;
    G.live_window = 1;
    if (to_programmes && list_mode()) { progs_to_now(); G.zone = Z_PROGS; }
    layer_push(&L);
}
