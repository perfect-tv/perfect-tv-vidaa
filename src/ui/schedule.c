/* «Программа и архив» (docs/controls.md): слева вкладки дат, в центре передачи канала по дням, справа —
 * панель передачи в фокусе; в шапке — «Программа» / «Напоминания · N». */
#include "screens.h"
#include "layer.h"
#include "keys.h"
#include "tv.h"
#include "u.h"
#include "app.h"
#include "core/clock.h"
#include "core/lang.h"
#include "core/prefs.h"
#include <stdio.h>
#include <string.h>

enum { Z_LIST, Z_DATES, Z_TABS };
static struct {
    channel *c;
    int zone, tab;          /* tab 0 — программа, 1 — напоминания */
    int pi, top, day, rtop, ri, btn;
} S;
static layer L;

static int day_of(int64_t t) { return (int)((day_start(t) - day_start(clock_now()) + 43200) / 86400); }
static int count(void) { return repo_prog_count(S.c); }
static int in_archive(const prog *p) {
    int64_t now = clock_now();
    return p->stop <= now && repo_has_archive(S.c) && p->start >= now - (int64_t)S.c->archive_days * 86400;
}

static void to_now(void) {
    int64_t t = T.cur == S.c ? tv_position() : clock_now();
    int i = repo_prog_at(S.c, t);
    if (i < 0) i = repo_prog_after(S.c, t);
    if (i < 0) i = count() - 1;
    S.pi = i < 0 ? 0 : i;
    S.top = S.pi - 3;
}

static void act(int long_press) {
    if (S.tab == 1) {
        if (S.ri >= 0 && S.ri < R.nrem) { repo_reminder_remove(S.ri); tv_toast(tr(S_REMINDER_REMOVED)); if (S.ri >= R.nrem) S.ri = R.nrem - 1; }
        return;
    }
    if (S.pi >= count()) return;
    prog p = repo_prog(S.c, S.pi);
    int64_t now = clock_now();
    if (!long_press && p.start <= now && now < p.stop) { layer_pop_all(); tv_play(S.c, NULL); return; }
    if (!long_press && in_archive(&p)) { layer_pop_all(); tv_play_archive(S.c, p.start, NULL); return; }
    card_open(S.c, &p);
}

static int has_long(layer *l, int k) { (void)l; return k == KEY_OK && S.zone == Z_LIST && S.tab == 0; }

static int key(layer *l, int k, int ev) {
    (void)l;
    int n = S.tab ? R.nrem : count();
    if (k == KEY_BACK && ev == EV_SHORT && S.zone == Z_LIST) { layer_pop(&L); return 1; }
    switch (S.zone) {
    case Z_LIST:
        if (k == KEY_UP || k == KEY_DOWN || k == KEY_CH_UP || k == KEY_CH_DOWN) {
            int d = k == KEY_DOWN ? 1 : k == KEY_UP ? -1 : k == KEY_CH_DOWN ? 8 : -8;
            if (S.tab) S.ri = CLAMP(S.ri + d, 0, n - 1);
            else S.pi = CLAMP(S.pi + d, 0, n - 1);
        } else if (k == KEY_LEFT && !S.tab && n) { S.day = day_of(repo_prog(S.c, S.pi).start); S.zone = Z_DATES; }
        else if (k == KEY_RIGHT) { S.zone = Z_TABS; S.btn = S.tab; }
        else if (k == KEY_OK) act(ev == EV_LONG);
        return 1;
    case Z_DATES:
        if (k == KEY_UP || k == KEY_DOWN) {
            int lo = n ? day_of(repo_prog(S.c, 0).start) : 0, hi = n ? day_of(repo_prog(S.c, n - 1).start) : 0;
            S.day = CLAMP(S.day + (k == KEY_DOWN ? 1 : -1), lo, hi);
            /* список — к заголовку дня */
            int64_t ds = day_start(day_start(clock_now()) + (int64_t)S.day * 86400 + 7200);
            int i = repo_prog_after(S.c, ds);
            if (i >= 0) { S.top = i; }
        } else if (k == KEY_RIGHT || k == KEY_OK || (k == KEY_BACK && ev == EV_SHORT)) {
            int now_i = repo_prog_at(S.c, clock_now());
            if (now_i >= 0 && day_of(repo_prog(S.c, now_i).start) == S.day) S.pi = now_i;
            else {
                int64_t ds = day_start(day_start(clock_now()) + (int64_t)S.day * 86400 + 7200);
                int i = repo_prog_after(S.c, ds);
                S.pi = i >= 0 ? i : S.pi;
            }
            S.zone = Z_LIST;
        }
        return 1;
    case Z_TABS:
        if (k == KEY_LEFT) { if (S.btn == 0) S.zone = Z_LIST; else { S.btn = 0; S.tab = 0; } }
        else if (k == KEY_RIGHT) { if (S.btn == 0) { S.btn = 1; S.tab = 1; S.ri = 0; } }
        else if (k == KEY_DOWN || k == KEY_OK || (k == KEY_BACK && ev == EV_SHORT)) S.zone = Z_LIST;
        return 1;
    }
    return 1;
}

static void draw_tabs(gfx_t *g, int right, int cy) {
    const char *t1 = tr(S_SCHEDULE), *t2 = trf(S_REMINDERS_N, R.nrem);
    int w2 = d_text_w(1.2, F_S, t2) + E(2.4), w1 = d_text_w(1.2, F_S, t1) + E(2.4), h = E(2.6);
    int x2 = right - w2, x1 = x2 - E(0.8) - w1;
    for (int i = 0; i < 2; i++) {
        int x = i ? x2 : x1, w = i ? w2 : w1;
        int foc = S.zone == Z_TABS && S.btn == i, open = S.tab == i;
        uint32_t bg = foc ? C_WHITE : open ? C_FILL_MARKED : C_FILL_BTN;
        d_round(g, x, cy - h / 2, w, h, E(0.4), bg);
        d_text_c(g, x, w, cy - d_line_h(1.2, F_S) / 2, 1.2, F_S, foc ? 0xff101010u : C_WHITE, i ? t2 : t1);
    }
}

static void draw_reminders(gfx_t *g, int x, int y, int w, int h) {
    if (!R.nrem) { d_text(g, x + E(1), y + E(1), 1.3, F_R, C_TEXT3, tr(S_NO_REMINDERS), w); return; }
    int row = E(4.2), rows = h / row;
    if (S.rtop > S.ri) S.rtop = S.ri;
    if (S.ri >= S.rtop + rows) S.rtop = S.ri - rows + 1;
    if (S.rtop < 0) S.rtop = 0;
    for (int r = 0; r < rows && S.rtop + r < R.nrem; r++) {
        int i = S.rtop + r;
        reminder *m = &R.rem[i];
        int ry = y + r * row, foc = i == S.ri && S.zone == Z_LIST;
        if (foc) d_round(g, x, ry, w, row - E(0.3), E(0.5), C_WHITE);
        uint32_t fg = foc ? 0xff101010u : C_WHITE, fg2 = foc ? 0xff404040u : C_TEXT3;
        char d[32], hm[8], line[300];
        fmt_day(m->start, d, sizeof d);
        fmt_hm(m->start, hm);
        snprintf(line, sizeof line, "%s, %s  ·  %s", d, hm, m->name);
        d_text(g, x + E(1), ry + E(0.35), 1.1, F_R, fg2, line, w - E(2));
        d_text(g, x + E(1), ry + E(1.9), 1.3, F_S, fg, m->title, w - E(2));
    }
    if (S.zone == Z_LIST) d_text(g, x, y + h + E(0.4), 1.1, F_R, C_TEXT3, tr(S_REMIND_REMOVE), w);
}

static void draw(layer *l, gfx_t *g) {
    (void)l;
    int x = E(4), y = E(2.4), w = SW - E(8), h = SH - E(4.8);
    d_fill(g, 0, 0, SW, SH, C_OVERLAY);
    d_round(g, x, y, w, h, E(1), C_POPUP);
    int hx = x + E(1.5), hy = y + E(1.2);
    channel *c = S.c;
    if (c && S.tab == 0) {
        d_logo(g, c, hx, hy, E(5.8), E(4), E(0.4), 0, 0);
        d_text(g, hx + E(7), hy - E(0.1), 1.5, F_B, C_WHITE, c->name, w / 2);
        d_text(g, hx + E(7), hy + E(2.2), 1.1, F_R, C_TEXT3, repo_has_archive(c) ? trf(S_EPG_STATUS_OK, tr(S_SCHEDULE_ARCHIVE), trp(S_DAYS, c->archive_days)) : tr(S_SCHEDULE), w / 2);
    } else d_text(g, hx, hy + E(0.6), 1.5, F_B, C_WHITE, tr(S_REMINDERS), w / 2);
    draw_tabs(g, x + w - E(1.5), hy + E(2));
    int ly = hy + E(5.4), lh = y + h - E(1.5) - ly;
    int dx = x + E(1.5), dw = E(8);
    int lx = dx + dw + E(1), pw = w * 30 / 100, lw = x + w - E(1.5) - pw - E(1.5) - lx;
    if (S.tab == 1) { draw_reminders(g, dx, ly, w - E(3), lh - E(2)); return; }
    int n = count();
    if (!n) { d_text(g, lx, ly + E(1), 1.3, F_R, C_TEXT3, tr(S_NO_PROGRAMMES), lw); return; }
    int64_t now = clock_now();
    int64_t play_t = T.cur == c ? tv_position() : -1;
    /* даты */
    int lo = day_of(repo_prog(c, 0).start), hi = day_of(repo_prog(c, n - 1).start);
    int focus_day = S.zone == Z_DATES ? S.day : day_of(repo_prog(c, S.pi).start);
    int play_day = play_t > 0 ? day_of(play_t) : 99;
    int dh = E(4.0), drows = lh / dh;
    int first = focus_day - drows / 2;
    if (first > hi - drows + 1) first = hi - drows + 1;
    if (first < lo) first = lo;
    for (int d = first, k = 0; d <= hi && k < drows; d++, k++) {
        int ry = ly + k * dh;
        char wd[16], dm[24];
        fmt_day_short(day_start(clock_now()) + (int64_t)d * 86400 + 7200, wd, sizeof wd, dm, sizeof dm);
        int foc = d == focus_day;
        uint32_t bg = foc && S.zone == Z_DATES ? C_WHITE : d == play_day ? C_FILL_MARKED : 0;
        if (bg) d_round(g, dx, ry, dw, dh - E(0.3), E(0.5), bg);
        if (foc && S.zone != Z_DATES) d_stroke(g, dx, ry, dw, dh - E(0.3), E(0.5), E(0.15), C_WHITE);
        uint32_t fg = foc && S.zone == Z_DATES ? 0xff101010u : C_WHITE;
        d_text_c(g, dx, dw, ry + E(0.35), 1.15, F_S, fg, d == 0 ? tr(S_TODAY) : wd);
        d_text_c(g, dx, dw, ry + E(2.0), 1.05, F_R, foc && S.zone == Z_DATES ? 0xff404040u : C_TEXT3, dm);
    }
    /* передачи */
    int row = E(3.4), rows = lh / row;
    if (S.top > S.pi) S.top = S.pi;
    if (S.pi >= S.top + rows) S.top = S.pi - rows + 1;
    if (S.top > n - rows) S.top = n - rows;
    if (S.top < 0) S.top = 0;
    for (int r = 0; r < rows && S.top + r < n; r++) {
        int i = S.top + r;
        prog p = repo_prog(c, i);
        int ry = ly + r * row;
        int foc = i == S.pi && S.zone != Z_DATES, playing = play_t >= p.start && play_t < p.stop, live = p.start <= now && now < p.stop;
        uint32_t bg = foc && S.zone == Z_LIST ? C_WHITE : playing ? C_FILL_PLAYING : 0;
        if (bg) d_round(g, lx, ry, lw, row - E(0.3), E(0.5), bg);
        if (foc && S.zone == Z_TABS) d_stroke(g, lx, ry, lw, row - E(0.3), E(0.5), E(0.15), C_WHITE);
        uint32_t fg = foc && S.zone == Z_LIST ? 0xff101010u : C_WHITE;
        int cy = ry + (row - E(0.3)) / 2;
        char hm[8];
        fmt_hm(p.start, hm);
        d_text_mid(g, lx + E(1), cy, 1.45, F_R, fg, hm, 0);
        int tx = lx + E(6.2);
        int ic = live ? I_PLAY : in_archive(&p) ? I_ARCHIVE : repo_reminder_find(c->key, p.start) >= 0 ? I_CLOCK : -1;
        if (ic >= 0) d_icon(g, ic, tx + E(0.85), cy, E(1.7), fg);
        tx += E(2.6);
        d_text_mid(g, tx, cy - (live ? E(0.35) : 0), 1.45, playing ? F_B : F_R, fg, p.title, lx + lw - tx - E(1.5));
        if (live) {
            double frac = (double)(now - p.start) / (double)(p.stop - p.start);
            int bw = lx + lw - tx - E(1.5);
            d_round(g, tx, cy + E(0.95), bw, E(0.25), E(0.12), foc && S.zone == Z_LIST ? 0x40000000u : C_FILL_MARKED);
            d_round(g, tx, cy + E(0.95), (int)(bw * frac), E(0.25), E(0.12), fg);
        }
        if (p.desc[0] || p.sub[0]) d_icon(g, I_CHEVRON, lx + lw - E(0.9), cy, E(0.9), foc && S.zone == Z_LIST ? fg : C_TEXT3);
    }
    d_fade_edges(g, lx, ly, lw, rows * row, S.top > 0, S.top + rows < n, C_POPUP);
    /* панель передачи */
    int px = x + w - E(1.5) - pw;
    prog p = repo_prog(c, S.pi);
    d_round(g, px, ly, pw, lh, E(0.6), C_FILL);
    int ix = px + E(1.2), iw = pw - E(2.4), iy = ly + E(1.2);
    char a1[8], a2[8], d[32], line[120];
    fmt_hm(p.start, a1);
    fmt_hm(p.stop, a2);
    fmt_day(p.start, d, sizeof d);
    snprintf(line, sizeof line, "%s – %s", a1, a2);
    d_text(g, ix, iy, 1.1, F_R, C_TEXT3, d, iw);
    d_text(g, ix, iy + E(1.8), 1.4, F_B, C_WHITE, line, iw);
    int used = d_lines(g, ix, iy + E(4.2), iw, 1.4, F_S, C_WHITE, p.title, 3, 0);
    int ty = iy + E(4.2) + used * d_line_h(1.4, F_S) + E(0.8);
    if (p.icon[0]) { int ih = iw * 9 / 16; if (d_image(g, p.icon, ix, ty, iw, ih, E(0.4), 1)) ty += ih + E(0.8); }
    int left = ly + lh - E(3.4) - ty;
    if (left > d_line_h(1.2, F_R)) d_lines(g, ix, ty, iw, 1.2, F_R, C_TEXT2, p.desc[0] ? p.desc : p.sub, left / d_line_h(1.2, F_R), 0);
    const char *what = p.start <= now && now < p.stop ? tr(S_OK_WATCH_LIVE) : in_archive(&p) ? tr(S_OK_WATCH_ARCHIVE) : tr(S_OK_CARD);
    d_text(g, ix, ly + lh - E(2.6), 1.15, F_S, C_TEXT3, what, iw);
}

static void tick(layer *l, double now) { (void)l; (void)now; }

void schedule_open(channel *c, int reminders) {
    L.name = "schedule";
    L.key = key;
    L.has_long = has_long;
    L.draw = draw;
    L.tick = tick;
    memset(&S, 0, sizeof S);
    S.c = c;
    S.tab = reminders || !c ? 1 : 0;
    S.zone = Z_LIST;
    if (c) to_now();
    layer_push(&L);
}
