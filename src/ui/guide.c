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
    int cat, cat_top, saved_gi, saved_ci, cat_search;
    int pl, saved_pl;
    int rows, row_h, list_y;
    int rev, mode_seen;    /* смена вида гида: зона сбрасывается, как в Android setView */
} G;

static layer L;
static char num_buf[6];      /* набор номера канала в гиде — сбрасывается при открытии */
static double num_at;

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
static int list_mode(void) { return P.guide_mode != GUIDE_GRID; }
/* «Категории + каналы»: колонка категорий на экране и в зоне каналов */
static int cats_fixed(void) { return P.guide_mode == GUIDE_CATS; }
/* «Категории + каналы» и «Каналы»: программа справа — только после → */
static int prog_hidden(void) { return P.guide_mode >= GUIDE_CATS; }
/* панель передачи справа: настройка «Описание в ТВ-гиде» и фокус в программе/датах */
static int desc_panel(void) { return P.guide_desc && list_mode() && (G.zone == Z_PROGS || G.zone == Z_DATES); }
const char *guide_mode_name(void) {
    switch (P.guide_mode) {
    case GUIDE_LIST: return tr(S_GUIDE_LIST);
    case GUIDE_CATS: return tr(S_GUIDE_CATS);
    case GUIDE_CHANNELS: return tr(S_GUIDE_CHANNELS);
    default: return tr(S_GUIDE_GRID);
    }
}

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
    if (tv_is_preview()) { G.top = 0; fit(); return; }   /* играет канал прежнего плейлиста */
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

static int cat_rows(void);

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
    G.cat_search = 0;
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
    char path[700];
    repo_channels_path(R.pl[i].id, path, sizeof path);
    if (!file_exists(path)) { R.cur = i; tv_playlist_changed(0); load_playlist(i, 1); return; }
    layer_pop(&L);
    /* видео не останавливаем: гид показывает новый плейлист, канал выбирается по OK */
    if (tv_preview_playlist(i)) { guide_open(0); load_epg(0); }
    else { tv_playlist_changed(0); if (!guide_is_open()) guide_open(0); load_epg(0); }
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
    if (k == KEY_BACK && ev == EV_SHORT && tv_is_preview()) {
        layer_pop(&L);
        tv_preview_back();
        return 1;
    }
    if (k == KEY_SEARCH && ev == EV_SHORT) { search_open(); return 1; }   /* поиск из любой зоны гида */
    if (k == KEY_MENU || (k == KEY_OK && ev == EV_LONG && G.zone != Z_PROGS)) {
        channel *c = cur_channel();
        if (c) tv_open_menu(c, g ? g->key : NULL, 1);
        return 1;
    }
    switch (G.zone) {
    case Z_CH:
        if (k == KEY_UP || k == KEY_DOWN) { if (n) G.ci = ((G.ci + (k == KEY_DOWN ? 1 : -1)) % n + n) % n; fit(); if (list_mode()) progs_to_now(); }
        else if (k == KEY_CH_UP || k == KEY_CH_DOWN) { G.ci += (k == KEY_CH_DOWN ? 1 : -1) * (G.rows > 1 ? G.rows - 1 : 1); fit(); if (list_mode()) progs_to_now(); }
        else if (k == KEY_LEFT && ev == EV_LONG) { channel *c = cur_channel(); if (c) tv_open_schedule(c, 0); }
        else if (k == KEY_LEFT) cats_open();
        else if (k == KEY_RIGHT) { if (list_mode()) { progs_to_now(); G.zone = Z_PROGS; } else grid_enter(); }
        else if (k == KEY_OK && ev == EV_SHORT) play_focused();
        else if (IS_DIGIT(k) && ev == EV_SHORT) {
            double t = app_now();
            if (t - num_at > 1.5) num_buf[0] = 0;
            num_at = t;
            size_t bl = strlen(num_buf);
            if (bl < 4) { num_buf[bl] = (char)('0' + k - KEY_D0); num_buf[bl + 1] = 0; }
            int num = atoi(num_buf);
            for (int i = 0; i < n; i++) if (g->ch[i]->num == num) { G.ci = i; fit(); break; }
        } else if (k == KEY_BACK && ev == EV_SHORT) layer_pop(&L);
        else if (k == KEY_GREEN || k == KEY_GUIDE) layer_pop(&L);
        else if (k == KEY_SEARCH) search_open();
        return 1;
    case Z_GRID:
        if (k == KEY_LEFT || k == KEY_RIGHT) grid_step(k == KEY_RIGHT ? 1 : -1);
        else if (k == KEY_UP || k == KEY_DOWN) { if (n) G.ci = ((G.ci + (k == KEY_DOWN ? 1 : -1)) % n + n) % n; fit(); }
        else if (k == KEY_CH_UP || k == KEY_CH_DOWN) { G.ci += (k == KEY_CH_DOWN ? 1 : -1) * (G.rows > 1 ? G.rows - 1 : 1); fit(); }
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
            dates_to_progs();
            G.zone = Z_DATES;
        } else if (k == KEY_LEFT || k == KEY_OK) G.zone = Z_PROGS;
        else if (k == KEY_BACK && ev == EV_SHORT) G.zone = Z_CH;
        return 1;
    case Z_CATS:
        /* над категориями — строка «Поиск»: ↑ с первой категории — на неё, с неё ↑ — к последней, ↓ — к первой */
        if (G.cat_search) {
            if (k == KEY_UP || k == KEY_DOWN) { G.cat_search = 0; G.cat = k == KEY_UP ? R.ngroups - 1 : 0; cats_preview(); }
            else if (k == KEY_OK && ev == EV_SHORT) { G.cat_search = 0; search_open(); }
            else if (k == KEY_RIGHT) { G.cat_search = 0; G.zone = Z_CH; }
            else if (k == KEY_LEFT && R.npl > 1) { G.cat_search = 0; G.saved_pl = R.cur; G.pl = R.cur; G.zone = Z_PLS; }
            else if (k == KEY_BACK && ev == EV_SHORT) { G.cat_search = 0; G.gi = G.saved_gi; G.ci = G.saved_ci; fit(); G.zone = Z_CH; }
            return 1;
        }
        if (k == KEY_UP && G.cat == 0) G.cat_search = 1;
        else if (k == KEY_UP || k == KEY_DOWN) { if (R.ngroups) G.cat = ((G.cat + (k == KEY_DOWN ? 1 : -1)) % R.ngroups + R.ngroups) % R.ngroups; cats_preview(); }
        else if (k == KEY_CH_UP || k == KEY_CH_DOWN) { G.cat += (k == KEY_CH_DOWN ? 1 : -1) * (cat_rows() - 1); G.cat = CLAMP(G.cat, 0, R.ngroups - 1); cats_preview(); }
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
    if (G.mode_seen != P.guide_mode) {   /* вид сменили в настройках или меню канала */
        G.mode_seen = P.guide_mode;
        if (G.zone == Z_GRID || G.zone == Z_PROGS || G.zone == Z_DATES) G.zone = Z_CH;
        G.live_window = 1;
    }
    if (G.rev != R.rev) {   /* каналы или категории изменились (скрыли, избранное) */
        G.rev = R.rev;
        if (G.gi >= R.ngroups) G.gi = R.ngroups - 1;
        if (G.gi < 0) G.gi = 0;
        fit();
    }
}

/* ---------- рисование ----------
 * Размеры — как у Android-версии (GuideLayer.GuideView), в em: безопасная зона 4 em по бокам и 2,4 em сверху и
 * снизу; шапка «канал + передача» 7,6 em (+1 em до заголовка категории), «только канал» 4,8 em; заголовок категории
 * со шкалой времени 2,6 em; строка канала 3,4 em с зазором 0,25 em; колонка каналов — 30 % ширины; категории и
 * плейлисты — колонки 19 em со строками 3,2 em; подсказки — 1,9 em над нижним полем. Строк столько, сколько
 * помещается, — при одинаковом «Размере интерфейса» столько же, сколько у Android-версии. */
#define F(x) ((x) * EM)
#define PADX F(4)
#define PADT F(2.4)
#define PADB F(2.4)
#define HEAD_H F(2.6)
#define ROW_H F(3.4)
#define GAP F(0.25)
#define CAT_W F(19)
#define CAT_ROW_H F(3.2)
#define DATES_W F(5.4)

static double top_block(void) {
    int h = P.guide_header;
    if (h == 0 && desc_panel()) h = 1;   /* с панелью передачи шапка — «только канал», как в Android */
    return h == 2 ? 0 : h == 1 ? F(4.8) + F(1) : F(7.6) + F(1);
}
static double body_top(void) { return PADT + top_block() + HEAD_H; }
static double hints_h(void) { return P.hints ? F(1.9) : 0; }
static double list_bottom(void) { return SH - PADB - hints_h(); }
static int visible_rows(void) { int n = (int)((list_bottom() - body_top() + GAP) / (ROW_H + GAP)); return n < 1 ? 1 : n; }
static int cat_rows(void) { int n = (int)((list_bottom() - body_top() + GAP) / (CAT_ROW_H + GAP)); return n < 1 ? 1 : n; }
static double ch_w(void) { return (SW - 2 * PADX) * 0.30; }
/* первая строка списка: фокус по центру, у краёв — без пустоты */
static int first_row(int focus, int rows, int n) {
    int f = focus - rows / 2;
    if (f > n - rows) f = n - rows;
    return f < 0 ? 0 : f;
}

static void outline(gfx_t *g, int x, int y, int w, int h, int r) { d_stroke(g, x, y, w, h, r, E(0.15) > 1 ? E(0.15) : 1, WA(0.8)); }

static void day_range(int64_t s, int64_t e, char *out, size_t cap) {
    char d[32], a[8], b[8];
    fmt_day(s, d, sizeof d);
    fmt_hm(s, a);
    fmt_hm(e, b);
    snprintf(out, cap, "%s, %s – %s", d, a, b);
}

/* передача (или получасовой слот без неё), в которую попадает t */
static int cell_at(const channel *c, int64_t t, int64_t *s, int64_t *e, prog *p) {
    int i = repo_prog_at(c, t);
    if (i >= 0) { *p = repo_prog(c, i); *s = p->start; *e = p->stop; return 1; }
    *s = align(t);
    *e = *s + SLOT;
    int n = repo_prog_after(c, *s);
    if (n >= 0) { prog q = repo_prog(c, n); if (q.start < *e && q.start > t) *e = q.start; }
    int pv = repo_prog_at(c, *s - 1);
    if (pv >= 0) { prog q = repo_prog(c, pv); if (q.stop > *s && q.stop <= t) *s = q.stop; }
    return 0;
}

static void draw_clock(gfx_t *g) {
    char clock[8];
    fmt_hm(clock_now(), clock);
    int t = (int)PADT;
    d_text_base(g, SW - (int)PADX - d_text_w(2.2, F_R, clock), t + E(1.7), 2.2, F_R, WA(0.9), clock, 0);
    if (repo_epg_loading()) {
        long progs = 0;
        int pr = loaders_epg_progress(&progs);
        const char *s = pr >= 0 ? trf(S_EPG_LOADING, pr) : trf(S_EPG_LOADING_N, trp(S_PROGRAMMES_N, progs));
        d_text_base(g, SW - (int)PADX - d_text_w(1.1, F_R, s), t + E(3.4), 1.1, F_R, WA(0.5), s, 0);
    }
}

static int clock_w(void) { return d_text_w(2.2, F_R, "00:00") + E(1); }

/* шапка «канал + передача»: логотип, канал, название, время, описание (передача — в фокусе гида) */
static void draw_top_full(gfx_t *g, channel *c) {
    int t = (int)PADT, x0 = (int)PADX;
    d_logo(g, c, x0, t, E(9), E(6.2), E(0.6), 1, 0);
    int x = x0 + E(9) + E(1.2), w = SW - (int)PADX - x - clock_w();
    int64_t s, e;
    prog p;
    memset(&p, 0, sizeof p);
    int has;
    if (G.zone == Z_PROGS || G.zone == Z_DATES) {
        has = G.pi >= 0 && G.pi < repo_prog_count(c);
        if (has) { p = repo_prog(c, G.pi); s = p.start; e = p.stop; } else has = cell_at(c, clock_now(), &s, &e, &p);
    } else has = cell_at(c, G.zone == Z_GRID ? G.ft : clock_now(), &s, &e, &p);
    char name[300], range[96];
    if (P.number_mode == NUM_NEAR_NAME) snprintf(name, sizeof name, "%d. %s", c->num, c->name);
    else snprintf(name, sizeof name, "%s", c->name);
    d_text_mid(g, x, t + E(0.6), 1.2, F_R, WA(0.7), name, w);
    d_marquee(g, x, t + E(2.35), 1.8, F_B, C_WHITE, has ? p.title : tr(S_NO_PROGRAMME), w, 1, 3);
    day_range(s, e, range, sizeof range);
    d_text_mid(g, x, t + E(4.05), 1.2, F_R, WA(0.8), range, w);
    if (has && p.desc[0]) d_lines(g, x, t + E(4.9), w, 1.15, F_R, WA(0.65), p.desc, 2, 0);
}

/* шапка «только канал»: логотип, название, категория и глубина архива */
static void draw_top_channel(gfx_t *g, channel *c) {
    int t = (int)PADT, x0 = (int)PADX;
    d_logo(g, c, x0, t + E(0.2), E(6.8), E(4.4), E(0.6), 1, 0);
    int x = x0 + E(6.8) + E(1.2), w = SW - (int)PADX - x - clock_w();
    char name[300], sub[300];
    if (P.number_mode == NUM_NEAR_NAME) snprintf(name, sizeof name, "%d. %s", c->num, c->name);
    else snprintf(name, sizeof name, "%s", c->name);
    d_marquee(g, x, t + E(1.5), 1.8, F_B, C_WHITE, name, w, 1, 3);
    group *gr = repo_group_of(c, NULL);
    if (repo_has_archive(c)) snprintf(sub, sizeof sub, "%s · %s %s", group_title(gr), tr(S_ARCHIVE), trp(S_DAYS_SHORT, c->archive_days));
    else snprintf(sub, sizeof sub, "%s", group_title(gr));
    d_text_mid(g, x, t + E(3.5), 1.2, F_R, WA(0.7), sub, w);
}

/* заголовок категории «‹ Эфирные 20» и, в сетке, шкала времени с подписью дня */
static void draw_head(gfx_t *g, int64_t ws, int dx) {
    if (desc_panel()) return;   /* каналы сжаты до логотипов — заголовка категории нет */
    double y = PADT + top_block();
    int cy = (int)(y + HEAD_H / 2), l = (int)PADX + dx, r = (int)(PADX + ch_w() - F(1)) + dx;
    group *gr = cur_group();
    if (R.ngroups > 1 && !cats_fixed()) d_text_mid(g, l, cy, 1.4, F_R, WA(0.5), "‹", 0);
    char cnt[16];
    snprintf(cnt, sizeof cnt, "%d", gr ? gr->n : 0);
    int cw = d_text_w(1.4, F_R, cnt), tx = l + E(1.6);
    int tw = d_text_mid(g, tx, cy, 1.4, F_B, C_WHITE, group_title(gr), r - tx - cw - E(1));
    d_text_mid(g, tx + tw + E(0.6), cy, 1.4, F_R, WA(0.5), cnt, 0);
    if (list_mode()) return;
    int gl = (int)(PADX + ch_w() + GAP) + dx, gw = SW - (int)PADX + dx - gl;
    double ppx = (double)gw / WINDOW;
    channel *fc = cur_channel();
    int64_t now = clock_now(), line_t = fc && fc == T.cur && T.arc && G.zone == Z_CH ? tv_position() : now;
    char hm[8];
    fmt_hm(line_t, hm);
    int nx = gl + (int)((line_t - ws) * ppx), hw = d_text_w(1.1, F_B, hm) / 2 + E(0.45) + E(0.3), tick_w = d_text_w(1.1, F_R, "00:00");
    for (int64_t t = align(ws); t < ws + WINDOW; t += SLOT) {
        if (t < ws) continue;
        int x = gl + (int)((t - ws) * ppx), lx = x + E(0.4);
        d_fill(g, x, (int)(y + F(0.6)), E(0.1) > 1 ? E(0.1) : 1, (int)(HEAD_H - F(0.6)), WA(0.25));
        if (nx < gl || nx > gl + gw || lx > nx + hw || lx + tick_w < nx - hw) {
            fmt_hm(t, hm);
            d_text_base(g, lx, (int)(y + F(1.6)), 1.1, F_R, WA(0.7), hm, 0);
        }
    }
    if (!(repo_epg_loading() && P.guide_header == 2)) {
        char day[32];
        int64_t dt = G.zone == Z_GRID ? G.ft : now;
        fmt_day(dt > ws ? dt : ws, day, sizeof day);
        d_text_base(g, gl + gw - d_text_w(1.1, F_R, day), (int)(y - F(0.1)), 1.1, F_R, WA(0.5), day, 0);
    }
}

/* строка канала: логотип, номер (режим «рядом с названием»), название; справа — архив, избранное, замок */
static void draw_row(gfx_t *g, channel *c, int l, int y, int r, int h, int focus, int logo_only) {
    int ch_focus = focus && G.zone == Z_CH;
    uint32_t bg = ch_focus ? C_WHITE : focus ? C_FILL_ACTIVE : C_FILL;
    d_round(g, l, y, r - l, h, E(0.6), bg);
    if (focus && !ch_focus && list_mode() && (G.zone == Z_PROGS || G.zone == Z_DATES)) outline(g, l, y, r - l, h, E(0.6));
    uint32_t fg = ch_focus ? 0xff101010u : C_WHITE;
    int cy = y + h / 2, x = l + E(0.35);
    d_logo(g, c, x, y + E(0.25), E(4.3), h - E(0.5), E(0.35), 1, ch_focus);
    if (logo_only) return;   /* каналы сжаты до логотипов — справа панель передачи */
    x += E(4.3) + E(0.7);
    if (P.number_mode == NUM_NEAR_NAME) {
        char n[16];
        snprintf(n, sizeof n, "%d", c->num);
        d_text_mid(g, x, cy, 1.15, F_R, ch_focus ? 0xff101010u : WA(0.5), n, 0);
        x += E(2.8);
    } else x += E(0.5);
    int fav = repo_is_fav(c), locked = repo_is_locked(c), arc = repo_has_archive(c);
    int isz = E(1.5), iw = (fav + locked + arc) * (isz + E(0.35));
    d_marquee(g, x, cy, 1.35, c == T.cur ? F_B : F_R, fg, c->name, r - x - E(0.8) - iw, ch_focus, 0);
    uint32_t ic = ch_focus ? 0xff101010u : WA(0.7);
    int ix = r - E(0.8);
    if (arc) { ix -= isz; d_icon(g, I_ARCHIVE, ix + isz / 2, cy, isz, ic); ix -= E(0.35); }
    if (fav) { ix -= isz; d_icon(g, I_HEART, ix + isz / 2, cy, isz, ic); ix -= E(0.35); }
    if (locked) { ix -= isz; d_icon(g, I_LOCK, ix + isz / 2, cy, isz, ic); }
}

/* сетка: ячейки окна [ws, ws + 2 ч) — передачи и пустые получасовые слоты */
static void draw_cells(gfx_t *g, channel *c, int y, int h, int gl, int gr, int64_t ws, int focus_row) {
    int64_t now = clock_now(), we = ws + WINDOW, covered = ws;
    double k = (double)(gr - gl) / WINDOW;
    int i = first_overlap(c, ws), pn = repo_prog_count(c), gap = (int)(GAP + 0.5);
    while (covered < we) {
        int64_t s, e;
        prog p;
        int is_prog = 0;
        if (i >= 0 && i < pn && (p = repo_prog(c, i)).start <= covered + 1) { s = p.start; e = p.stop; is_prog = 1; i++; }
        else {
            s = covered;
            e = align(covered) + SLOT;
            if (i >= 0 && i < pn) { prog q = repo_prog(c, i); if (q.start < e) e = q.start; }
        }
        if (e <= covered) { covered++; continue; }
        covered = e;
        int cl = gl + (int)(((s < ws ? ws : s) - ws) * k), cr = gl + (int)(((e > we ? we : e) - ws) * k) - gap;
        if (cr - cl < 2) continue;
        int foc = focus_row && s <= G.ft && G.ft < e;
        int is_now = s <= now && now < e, past = e <= now;
        uint32_t bg = foc ? C_WHITE : !is_prog ? WA(0.04) : is_now ? C_FILL_NOW : past ? C_FILL : C_FILL_CELL;
        d_round(g, cl, y, cr - cl, h, E(0.4), bg);
        int tl = cl + E(0.7), tw = cr - tl - E(0.5), cy = y + h / 2;
        if (is_prog && s > now && repo_reminder_find(c->key, s) >= 0 && tw > E(2)) {
            int isz = E(1.5);
            d_icon(g, I_CLOCK, cr - E(0.5) - isz / 2, cy, isz, foc ? 0xff101010u : C_ARCHIVE);
            tw -= isz + E(0.4);
        }
        if (tw > E(1)) {
            uint32_t fg = foc ? 0xff101010u : past ? WA(0.45) : C_WHITE;
            if (is_prog) d_marquee(g, tl, cy, 1.25, F_R, fg, p.title, tw, foc, 2);
            else if (foc) {
                char a[8], b[8], t[24];
                fmt_hm(s, a);
                fmt_hm(e, b);
                snprintf(t, sizeof t, "%s – %s", a, b);
                d_text_mid(g, tl, cy, 1.25, F_R, fg, t, tw);
            }
        }
    }
}

/* линия «сейчас» (красная) или, на канале с играющим архивом, — янтарная линия кадра; над ней — плашка со временем */
static void draw_now_line(gfx_t *g, int gl, int gr, int64_t ws, int shown) {
    channel *fc = cur_channel();
    int anchor = fc && fc == T.cur && T.arc && G.zone == Z_CH;
    int64_t lt = anchor ? tv_position() : clock_now();
    int x = gl + (int)((double)(lt - ws) / WINDOW * (gr - gl));
    if (x < gl || x > gr) return;
    double bt = body_top();
    uint32_t col = anchor ? C_ARCHIVE : C_LIVE;
    int lw = E(0.2) > 2 ? E(0.2) : 2;
    d_fill(g, x - lw / 2, (int)(bt - F(0.5)), lw, (int)(F(0.5) + shown * (ROW_H + GAP) - GAP), col);
    char hm[8];
    fmt_hm(lt, hm);
    int hw = d_text_w(1.1, F_B, hm) / 2 + E(0.45), cx = x, cy = (int)(bt - F(1.35));
    if (cx < gl + hw) cx = gl + hw;
    if (cx > gr - hw) cx = gr - hw;
    d_round(g, cx - hw, cy - E(0.85), hw * 2, E(1.7), E(0.35), col);
    d_text_mid(g, cx - d_text_w(1.1, F_B, hm) / 2, cy, 1.1, F_B, anchor ? C_ON_ARCHIVE : C_WHITE, hm, 0);
}

/* вид «Список»: передачи канала в фокусе — идущая (и играющая архивная) выше, со шкалой */
static double prog_h(const prog *p, int64_t now, int64_t arch_t) {
    int tall = (p->start <= now && now < p->stop) || (arch_t >= 0 && p->start <= arch_t && arch_t < p->stop);
    return tall ? ROW_H + F(0.9) : ROW_H;
}

static void draw_progs(gfx_t *g, int l, int r) {
    channel *c = cur_channel();
    int pn = c ? repo_prog_count(c) : 0;
    int64_t now = clock_now();
    double top = body_top(), bottom = list_bottom();
    if (!pn) { d_text_mid(g, l + E(1), (int)(top + ROW_H / 2), 1.35, F_R, WA(0.6), tr(S_NO_PROGRAMME), r - l - E(1)); return; }
    int watching = c == T.cur, arch = watching && T.arc;
    int64_t pos = arch ? tv_position() : now, arch_t = arch ? pos : -1;
    int fi = G.zone == Z_CH ? -1 : G.pi;
    if (fi < 0) { fi = repo_prog_at(c, now); if (fi < 0) fi = repo_prog_after(c, now); if (fi < 0) fi = pn - 1; }
    /* прокрутка: передача в фокусе по центру; высоты — только около неё (передач может быть тысячи) */
    double view = bottom - top;
    int from = fi - 40 < 0 ? 0 : fi - 40, to = fi + 40 > pn ? pn : fi + 40;
    double above = 0, below = 0;
    for (int i = from; i < to; i++) {
        prog p = repo_prog(c, i);
        double h = prog_h(&p, now, arch_t) + GAP;
        if (i < fi) above += h; else if (i > fi) below += h;
    }
    prog fp = repo_prog(c, fi);
    double fh = prog_h(&fp, now, arch_t);
    double off = above - view / 2 + fh / 2;   /* от начала окна from */
    if (from == 0 && off < 0) off = 0;
    double total_after = above + fh + GAP + below;
    if (to == pn && off > total_after - view) off = total_after - view > 0 ? total_after - view : 0;
    if (from == 0 && off < 0) off = 0;
    gfx_push_clip(g, l, (int)top, r - l, (int)(bottom - top));
    double y = top - off;
    for (int i = from; i < to && y <= bottom; i++) {
        prog p = repo_prog(c, i);
        double hh = prog_h(&p, now, arch_t);
        if (y + hh >= top) {
            int yi = (int)y, h = (int)hh;
            int live = p.start <= now && now < p.stop, past = p.stop <= now;
            int pf = G.zone == Z_PROGS && i == G.pi, ghost = G.zone == Z_DATES && i == G.pi;
            int playing_arc = arch && p.start <= pos && pos < p.stop, playing_live = watching && !arch && live;
            uint32_t bg = pf ? C_WHITE : ghost ? C_FILL_ACTIVE : (live && !arch) || playing_arc ? C_FILL_PLAYING : 0;
            if (bg) d_round(g, l, yi, r - l, h, E(0.4), bg);
            if (ghost) outline(g, l, yi, r - l, h, E(0.4));
            uint32_t fg = pf ? 0xff101010u : past ? WA(0.62) : C_WHITE;
            int tall = live || playing_arc;
            int cy = tall ? yi + (int)(ROW_H / 2) - E(0.1) : yi + h / 2;
            char hm[8];
            fmt_hm(p.start, hm);
            d_text_mid(g, l + E(0.8), cy, 1.3, F_R, fg, hm, 0);
            uint32_t ic = pf ? 0xff101010u : WA(0.8);
            int isz = E(1.7), ix = r - E(0.8);
            if (live) {
                uint32_t lc = playing_live ? C_LIVE : ic;
                int lw = d_text_w(1.2, F_B, "LIVE"), pad = E(0.8);
                ix -= lw + 2 * pad;
                d_stroke(g, ix, cy - E(0.85), lw + 2 * pad, E(1.7), E(0.3), E(0.12) > 1 ? E(0.12) : 1, lc);
                d_text_mid(g, ix + pad, cy, 1.2, F_B, lc, "LIVE", 0);
                ix -= E(0.5);
            }
            if ((past || playing_arc) && repo_has_archive(c) && p.start >= now - (int64_t)c->archive_days * 86400 - SLOT) {
                ix -= isz;
                d_icon(g, I_ARCHIVE, ix + isz / 2, cy, isz, playing_arc ? C_ARCHIVE : ic);
                ix -= E(0.5);
            }
            if (p.start > now && repo_reminder_find(c->key, p.start) >= 0) { ix -= isz; d_icon(g, I_CLOCK, ix + isz / 2, cy, isz, ic); ix -= E(0.5); }
            int tx = l + E(5.4);
            d_marquee(g, tx, cy, 1.35, tall ? F_B : F_R, fg, p.title, ix - tx - E(0.3), pf, 1);
            if (tall) {
                int by = yi + h - E(0.95), bw = r - E(0.8) - tx, bh = E(0.25) > 2 ? E(0.25) : 2;
                d_round(g, tx, by, bw, bh, bh / 2, pf ? 0x33000000u : WA(0.22));
                double fl = (double)(now - p.start) / (double)(p.stop - p.start);
                if (live) d_round(g, tx, by, (int)(bw * (fl < 0 ? 0 : fl > 1 ? 1 : fl)), bh, bh / 2, pf ? 0xff101010u : C_WHITE);
                if (playing_arc) {
                    double fa = (double)(pos - p.start) / (double)(p.stop - p.start);
                    d_round(g, tx, by, (int)(bw * (fa < 0 ? 0 : fa > 1 ? 1 : fa)), bh, bh / 2, C_ARCHIVE);
                }
            }
        }
        y += hh + GAP;
    }
    gfx_pop_clip(g);
}

/* колонка дат: день недели и число; в фокусе — белая, день выбранной передачи — рамкой */
static void draw_dates(gfx_t *g, int l, int r) {
    channel *c = cur_channel();
    int pn = c ? repo_prog_count(c) : 0;
    if (!pn) return;
    int lo = day_of(repo_prog(c, 0).start), hi = day_of(repo_prog(c, pn - 1).start);
    int fi = G.zone == Z_CH ? repo_prog_at(c, clock_now()) : G.pi;
    if (fi < 0 || fi >= pn) fi = G.pi < pn ? G.pi : pn - 1;
    int cur = G.zone == Z_DATES ? G.day : day_of(repo_prog(c, fi).start);
    double th = F(3.6), step = th + F(0.3), top = body_top(), bottom = list_bottom(), view = bottom - top;
    int n = hi - lo + 1;
    double off = (cur - lo) * step - view / 2 + th / 2;
    if (off > n * step - view) off = n * step - view;
    if (off < 0) off = 0;
    gfx_push_clip(g, l, (int)top, r - l, (int)view);
    for (int d = lo; d <= hi; d++) {
        int y = (int)(top + (d - lo) * step - off);
        if (y + th < top || y > bottom) continue;
        int focus = G.zone == Z_DATES && d == G.day;
        if (focus) d_round(g, l, y, r - l, (int)th, E(0.4), C_WHITE);
        else if (d == cur) { d_round(g, l, y, r - l, (int)th, E(0.4), C_FILL_ACTIVE); outline(g, l, y, r - l, (int)th, E(0.4)); }
        int64_t t = day_start(clock_now()) + (int64_t)d * 86400 + 7200;
        char wd[16], dm[24], num[8];
        fmt_day_short(t, wd, sizeof wd, dm, sizeof dm);
        snprintf(num, sizeof num, "%d", local_tm(t).d);
        int face = focus || d == cur ? F_B : F_R;
        uint32_t fg = focus ? 0xff101010u : d == cur ? C_WHITE : WA(0.72);
        int cy = y + (int)(th / 2);
        d_text_c(g, l, r - l, cy - E(0.6) - d_line_h(1.25, face) / 2, 1.25, face, fg, wd);
        d_text_c(g, l, r - l, cy + E(0.75) - d_line_h(1.25, face) / 2, 1.25, face, fg, num);
    }
    gfx_pop_clip(g);
}

/* колонка категорий: сверху — строка «Поиск»; название, число каналов, избранное / замок; в фокусе — белая */
static void draw_cats(gfx_t *g, int l) {
    int w = (int)CAT_W, r = l + w;
    double hy = PADT + top_block();
    int sf = G.cat_search && G.zone == Z_CATS;
    d_round(g, l, (int)(hy - F(0.3)), w, (int)HEAD_H, E(0.4), sf ? C_WHITE : C_FILL);
    int scy = (int)(hy + HEAD_H / 2 - F(0.3));
    uint32_t sc = sf ? 0xff101010u : WA(0.7);
    d_icon(g, I_SEARCH, l + E(1.8), scy, E(1.4), sc);
    d_text_mid(g, l + E(3.2), scy, 1.3, F_R, sf ? 0xff101010u : WA(0.85), tr(S_SEARCH), w - E(4.2));
    int rows = cat_rows(), first = first_row(G.cat, rows, R.ngroups);
    double bt = body_top();
    for (int k = 0; k < rows && first + k < R.ngroups; k++) {
        int i = first + k;
        group *gr = &R.groups[i];
        int y = (int)(bt + k * (CAT_ROW_H + GAP)), h = (int)CAT_ROW_H, cy = y + h / 2;
        int focus = i == G.cat, white = focus && G.zone == Z_CATS && !G.cat_search;
        if (focus) d_round(g, l, y, w, h, E(0.4), white ? C_WHITE : C_FILL_ACTIVE);
        char n[16];
        snprintf(n, sizeof n, "%d", gr->n);
        int cw = d_text_w(1.1, F_R, n);
        d_text_mid(g, r - E(1) - cw, cy, 1.1, F_R, white ? 0xff101010u : WA(0.5), n, 0);
        int tx = l + E(1), isz = E(1.5);
        uint32_t ic = white ? 0xff101010u : WA(0.7);
        if (gr->kind == GK_FAV) { d_icon(g, I_HEART, tx + isz / 2, cy, isz, ic); tx += isz + E(0.5); }
        else if (P.pin[0] && repo_group_locked(gr->key)) { d_icon(g, I_LOCK, tx + isz / 2, cy, isz, ic); tx += isz + E(0.5); }
        d_text_mid(g, tx, cy, 1.3, focus ? F_B : F_R, white ? 0xff101010u : focus ? C_WHITE : WA(0.85), group_title(gr), r - E(1) - cw - E(1) - tx);
    }
}

/* колонка плейлистов: название и число каналов; открытый — выделен, в фокусе — белая */
static void draw_pls(gfx_t *g, int l) {
    int w = (int)CAT_W, r = l + w;
    double hy = PADT + top_block();
    d_text_mid(g, l + E(1), (int)(hy + HEAD_H / 2 - F(0.3)), 1.4, F_B, WA(0.85), tr(S_PLAYLISTS), w - E(2));
    int rows = cat_rows(), first = first_row(G.pl, rows, R.npl);
    double bt = body_top();
    for (int k = 0; k < rows && first + k < R.npl; k++) {
        int i = first + k;
        int y = (int)(bt + k * (CAT_ROW_H + GAP)), h = (int)CAT_ROW_H, cy = y + h / 2;
        int focus = i == G.pl, open = i == R.cur;
        d_round(g, l, y, w, h, E(0.4), focus ? C_WHITE : open ? C_FILL_ACTIVE : C_FILL);
        char n[16] = "";
        if (R.pl[i].count > 0) snprintf(n, sizeof n, "%d", R.pl[i].count);
        int cw = d_text_w(1.1, F_R, n);
        d_text_mid(g, r - E(1) - cw, cy, 1.1, F_R, focus ? 0xff101010u : WA(0.5), n, 0);
        d_text_mid(g, l + E(1), cy, 1.3, open ? F_B : F_R, focus ? 0xff101010u : C_WHITE, R.pl[i].name[0] ? R.pl[i].name : "—", r - E(1) - cw - E(2) - l - E(1));
    }
}

static void draw_hints(gfx_t *g) {
    if (!P.hints) return;
    hint h[6];
    int n = 0;
    switch (G.zone) {
    case Z_CH:
        h[n++] = (hint){-1, tr(S_K_OK), tr(S_H_WATCH)};
        h[n++] = (hint){-1, "←", tr(S_H_CATEGORIES)};
        h[n++] = (hint){-1, "→", tr(list_mode() ? S_H_PROGRAMME : S_H_INTO_GRID)};
        h[n++] = (hint){-1, tr(S_K_HOLD_OK), tr(S_H_MENU)};
        break;
    case Z_GRID:
        h[n++] = (hint){-1, tr(S_K_OK), tr(S_H_ABOUT)};
        h[n++] = (hint){-1, "← →", tr(S_H_PROGRAMMES)};
        h[n++] = (hint){-1, tr(S_K_BACK), tr(S_H_TO_CHANNELS)};
        break;
    case Z_PROGS:
        h[n++] = (hint){-1, tr(S_K_OK), tr(S_H_WATCH)};
        h[n++] = (hint){-1, "←", tr(S_H_TO_CHANNELS)};
        h[n++] = (hint){-1, "→", tr(S_H_DAYS)};
        h[n++] = (hint){-1, "↑ ↓", tr(S_H_PROGRAMME_PICK)};
        break;
    case Z_DATES:
        h[n++] = (hint){-1, "↑ ↓", tr(S_H_DATE)};
        h[n++] = (hint){-1, "←", tr(S_H_PROGRAMMES)};
        h[n++] = (hint){-1, tr(S_K_OK), tr(S_H_WATCH)};
        h[n++] = (hint){-1, tr(S_K_BACK), tr(S_H_TO_CHANNELS)};
        break;
    case Z_CATS:
        h[n++] = (hint){-1, "↑ ↓", tr(S_H_CATEGORY)};
        h[n++] = (hint){-1, "→", tr(S_H_TO_CHANNELS)};
        h[n++] = (hint){-1, tr(S_K_BACK), tr(S_H_CANCEL)};
        break;
    case Z_PLS:
        h[n++] = (hint){-1, "↑ ↓", tr(S_H_PLAYLIST)};
        h[n++] = (hint){-1, "→", tr(S_H_CATEGORIES)};
        h[n++] = (hint){-1, tr(S_K_BACK), tr(S_H_CANCEL)};
        break;
    }
    d_hints(g, (int)PADX, SH - (int)(PADB + F(0.8)), h, n);
}

static double panel_w(void) { return (SW - 2 * PADX) * 0.30; }

/* панель передачи справа (настройка «Описание в ТВ-гиде»): картинка, время, название, описание и что сделает OK */
static void draw_panel(gfx_t *g, int l, int r) {
    channel *c = cur_channel();
    int pn = c ? repo_prog_count(c) : 0;
    if (!c || G.pi < 0 || G.pi >= pn) return;
    prog p = repo_prog(c, G.pi);
    double top = body_top(), bottom = list_bottom();
    int w = r - l;
    if (w < E(6)) return;
    int ih = (int)(w * 9 / 16), cap = (int)((bottom - top) * 0.4);
    if (ih > cap) ih = cap;
    if (!d_image(g, p.icon, l, (int)top, w, ih, E(0.4), 1)) d_logo(g, c, l, (int)top, w, ih, E(0.4), 0, 0);
    double y = top + ih + F(1.2);
    char range[96];
    day_range(p.start, p.stop, range, sizeof range);
    d_text_mid(g, l, (int)(y + E(0.6)), 1.15, F_R, WA(0.7), range, w);
    y += F(1.9);
    d_lines(g, l, (int)(y + E(0.6)), w, 1.5, F_B, C_WHITE, p.title, 2, 0.1);
    y += 2 * d_line_h(1.5, F_B) + F(0.7);
    int64_t now = clock_now();
    int live = p.start <= now && now < p.stop;
    int arc = p.stop <= now && repo_has_archive(c) && p.start >= now - (int64_t)c->archive_days * 86400 - SLOT;
    hint h = {-1, tr(S_K_OK), live ? tr(S_WATCH_LIVE) : arc ? tr(S_WATCH_ARCHIVE) : tr(S_H_ABOUT)};
    d_hints(g, l, (int)(bottom - E(0.9)), &h, 1);
    int dl = (int)(bottom - E(2.6));
    int lh = d_line_h(1.15, F_R) + E(0.15);
    int maxl = lh > 0 ? (int)((dl - y) / lh) : 0;
    if (maxl > 0) d_lines(g, l, (int)y, w, 1.15, F_R, WA(0.65), p.desc, maxl, 0.15);
}

/* фон: в видах «Категории + каналы» и «Каналы» затемнена левая часть, справа видно видео */
static void draw_bg(gfx_t *g) {
    int mid = SH * 2 / 5;
    int right_part = prog_hidden() && G.zone != Z_PROGS && G.zone != Z_DATES && G.zone != Z_GRID;
    if (!right_part) {
        d_vgrad(g, 0, 0, SW, mid, C_GUIDE_TOP, 0xdc0c0d0eu);
        d_vgrad(g, 0, mid, SW, SH - mid, 0xdc0c0d0eu, C_GUIDE_BOTTOM);
        return;
    }
    int xa = (int)(PADX + (cats_fixed() ? CAT_W + F(1) : 0) + ch_w() + F(1.5));
    if (xa > SW * 7 / 10) xa = SW * 7 / 10;
    d_vgrad(g, 0, 0, xa, mid, C_GUIDE_TOP, 0xdc0c0d0eu);
    d_vgrad(g, 0, mid, xa, SH - mid, 0xdc0c0d0eu, C_GUIDE_BOTTOM);
    d_hgrad(g, xa, 0, SW * 22 / 100, SH, 0xdc0c0d0eu, 0x000c0d0eu);
    int th = (int)(PADT + top_block() + HEAD_H);
    d_vgrad(g, 0, 0, SW, th + (int)F(2), 0xbe08090bu, 0x0008090bu);
}

static void draw(layer *l, gfx_t *g) {
    (void)l;
    draw_bg(g);
    group *gr = cur_group();
    channel *c = cur_channel();
    int n = gr ? gr->n : 0;
    G.rows = visible_rows();
    fit();
    G.top = first_row(G.ci, G.rows, n);
    int panel = desc_panel();
    if (P.guide_header != 2) {
        if (c) { if (P.guide_header == 1 || (panel && P.guide_header == 0)) draw_top_channel(g, c); else draw_top_full(g, c); }
        draw_clock(g);
    }
    draw_hints(g);
    /* колонки категорий и плейлистов слева, список и сетка сдвигаются вправо */
    int pls = G.zone == Z_PLS, cats = G.zone == Z_CATS || pls || (cats_fixed() && G.zone == Z_CH);
    if (cats_fixed() && G.zone == Z_CH) G.cat = G.gi;
    int dx = (int)((cats + pls) * (CAT_W + F(1)));
    if (pls) draw_pls(g, (int)PADX);
    if (cats) draw_cats(g, (int)PADX + (pls ? (int)(CAT_W + F(1)) : 0));
    if (dx) gfx_push_clip(g, (int)(PADX - GAP) + dx, 0, SW - (int)(PADX - GAP) - dx, SH);
    if (G.zone == Z_CH || G.live_window) {
        channel *fc = cur_channel();
        int64_t base = fc && fc == T.cur && T.arc ? tv_position() : clock_now();
        G.ws = base - SLOT;
    }
    draw_head(g, G.ws, dx);
    double bt = body_top();
    int chw = panel ? (int)F(5.6) : (int)ch_w();
    int lx = (int)PADX + dx, r = (int)PADX + chw + dx;
    if (!n) d_text_mid(g, lx + E(1), (int)(bt + F(2)), 1.35, F_R, WA(0.6), tr(S_EMPTY_CATEGORY), SW - 2 * (int)PADX);
    int gl = (int)(PADX + chw + GAP) + dx, grr = SW - (int)PADX + dx;
    int shown = 0;
    for (int k = 0; k < G.rows && G.top + k < n; k++, shown++) {
        int i = G.top + k;
        int y = (int)(bt + k * (ROW_H + GAP)), h = (int)(bt + k * (ROW_H + GAP) + ROW_H) - y;
        draw_row(g, gr->ch[i], lx, y, r, h, i == G.ci, panel);
        if (!list_mode()) draw_cells(g, gr->ch[i], y, h, gl, grr, G.ws, i == G.ci && G.zone == Z_GRID);
    }
    if (n && !list_mode()) draw_now_line(g, gl, grr, G.ws, shown);
    int show_progs = list_mode() && (!prog_hidden() || G.zone == Z_PROGS || G.zone == Z_DATES);
    if (n && show_progs) {
        int dr = SW - (int)PADX + dx - (panel ? (int)panel_w() + (int)E(1) : 0), dl = dr - (int)DATES_W;
        draw_progs(g, (int)PADX + chw + (int)F(1.2) + dx, dl - E(1));
        draw_dates(g, dl, dr);
        if (panel) draw_panel(g, dr + E(0.6), SW - (int)PADX + dx);
    }
    if (dx) gfx_pop_clip(g);
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
    G.mode_seen = P.guide_mode;
    num_buf[0] = 0;
    num_at = 0;
    sync_from_tv();
    G.zone = Z_CH;
    G.live_window = 1;
    if (to_programmes && list_mode()) { progs_to_now(); G.zone = Z_PROGS; }
    layer_push(&L);
}
