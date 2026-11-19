/* Окна: карточка передачи, недавние каналы, «Управление», скрытое, мастер первого запуска, напоминания,
 * подтверждение и выбор из списка (docs/controls.md). */
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

/* рамка окна по центру; возвращает внутренний x */
static void window(gfx_t *g, int x, int y, int w, int h) {
    d_fill(g, 0, 0, SW, SH, C_OVERLAY);
    d_round(g, x, y, w, h, E(1), C_POPUP);
}
static int button(gfx_t *g, int x, int y, const char *label, int focused) {
    int w = d_text_w(1.25, F_S, label) + E(3), h = E(3);
    d_round(g, x, y, w, h, E(0.4), focused ? C_WHITE : C_FILL_BTN);
    d_text_c(g, x, w, y + (h - d_line_h(1.25, F_S)) / 2, 1.25, F_S, focused ? 0xff101010u : C_WHITE, label);
    return w;
}

/* ================= карточка передачи ================= */
enum { B_WATCH = 1, B_FROM_START, B_WATCH_LIVE, B_REMIND, B_UNREMIND, B_CLOSE };
static struct {
    channel *c;
    int64_t start, stop;
    char title[300], sub[300], cat[200], icon[600];
    char desc[8192];
    int btn[4], nb, focus, scroll, max_scroll;
} CD;
static layer CL;

static void card_buttons(void) {
    int64_t now = clock_now();
    CD.nb = 0;
    channel *c = CD.c;
    int arc_ok = repo_has_archive(c) && CD.start >= now - (int64_t)c->archive_days * 86400;
    if (CD.stop <= now) { if (arc_ok) CD.btn[CD.nb++] = B_WATCH; }
    else if (CD.start <= now) {
        if (arc_ok) CD.btn[CD.nb++] = B_FROM_START;
        CD.btn[CD.nb++] = c == T.cur && T.arc ? B_WATCH_LIVE : c == T.cur ? 0 : B_WATCH;
        if (!CD.btn[CD.nb - 1]) CD.nb--;
    } else CD.btn[CD.nb++] = repo_reminder_find(c->key, CD.start) >= 0 ? B_UNREMIND : B_REMIND;
    if (!CD.nb) CD.btn[CD.nb++] = B_CLOSE;
    if (CD.focus >= CD.nb) CD.focus = 0;
}
static const char *btn_label(int b) {
    switch (b) {
    case B_WATCH: return tr(S_WATCH);
    case B_FROM_START: return tr(S_WATCH_FROM_START);
    case B_WATCH_LIVE: return tr(S_WATCH_LIVE);
    case B_REMIND: return tr(S_REMIND);
    case B_UNREMIND: return tr(S_REMIND_REMOVE);
    default: return tr(S_CLOSE);
    }
}
static int card_key(layer *l, int k, int ev) {
    (void)l;
    if (k == KEY_BACK && ev == EV_SHORT) { layer_pop(&CL); return 1; }
    if (k == KEY_LEFT || k == KEY_RIGHT) CD.focus = CLAMP(CD.focus + (k == KEY_RIGHT ? 1 : -1), 0, CD.nb - 1);
    else if (k == KEY_UP || k == KEY_DOWN) CD.scroll = CLAMP(CD.scroll + (k == KEY_DOWN ? 1 : -1), 0, CD.max_scroll);
    else if (k == KEY_OK && ev == EV_SHORT) {
        int b = CD.btn[CD.focus];
        channel *c = CD.c;
        prog p = {.start = CD.start, .stop = CD.stop, .title = CD.title};
        switch (b) {
        case B_WATCH:
            layer_pop_all();
            if (CD.stop <= clock_now()) tv_play_archive(c, CD.start, NULL);
            else tv_play(c, NULL);
            break;
        case B_FROM_START: layer_pop_all(); tv_play_archive(c, CD.start, NULL); break;
        case B_WATCH_LIVE: layer_pop_all(); tv_go_live(); break;
        case B_REMIND: repo_reminder_add(c, &p); tv_toast(tr(S_REMINDER_SET)); card_buttons(); break;
        case B_UNREMIND: repo_reminder_remove(repo_reminder_find(c->key, CD.start)); tv_toast(tr(S_REMINDER_REMOVED)); card_buttons(); break;
        default: layer_pop(&CL); break;
        }
    }
    return 1;
}
static void card_draw(layer *l, gfx_t *g) {
    (void)l;
    int w = E(62), h = E(36), x = (SW - w) / 2, y = (SH - h) / 2;
    window(g, x, y, w, h);
    int ix = x + E(2), iw = w - E(4), iy = y + E(1.8);
    d_logo(g, CD.c, ix, iy, E(5.8), E(4), E(0.4), 0, 0);
    char d[32], a1[8], a2[8], line[200];
    fmt_day(CD.start, d, sizeof d);
    fmt_hm(CD.start, a1);
    fmt_hm(CD.stop, a2);
    snprintf(line, sizeof line, "%s  ·  %s, %s – %s", CD.c->name, d, a1, a2);
    d_text(g, ix + E(7), iy + E(0.2), 1.2, F_R, C_TEXT3, line, iw - E(7));
    int64_t now = clock_now();
    if (CD.start <= now && now < CD.stop) d_badge(g, ix + E(7), iy + E(3.0), 1.05, C_LIVE, C_WHITE, -1, tr(S_LIVE));
    iy += E(5.4);
    int tl = d_lines(g, ix, iy, iw, 1.8, F_B, C_WHITE, CD.title, 2, 0);
    iy += tl * d_line_h(1.8, F_B) + E(0.4);
    char tags[600];
    snprintf(tags, sizeof tags, "%s%s%s", CD.sub, CD.sub[0] && CD.cat[0] ? "  ·  " : "", CD.cat);
    if (tags[0]) { d_text(g, ix, iy, 1.15, F_S, C_TEXT3, tags, iw); iy += E(2.2); }
    int bottom = y + h - E(5.6);
    int img_w = 0;
    if (CD.icon[0]) { img_w = E(16); if (!d_image(g, CD.icon, ix + iw - img_w, iy, img_w, img_w * 9 / 16, E(0.4), 1)) img_w = 0; }
    int tw = iw - (img_w ? img_w + E(1.5) : 0);
    int lh = d_line_h(1.25, F_R), rows = (bottom - iy) / lh;
    /* прокрутка описания по строкам: рисуем со смещением и обрезкой */
    int total = text_count_lines(tw, E(1.25) > 6 ? E(1.25) : 6, F_R, CD.desc, 400);
    CD.max_scroll = total > rows ? total - rows : 0;
    gfx_push_clip(g, ix, iy, tw, rows * lh);
    d_lines(g, ix, iy - CD.scroll * lh, tw, 1.25, F_R, C_TEXT2, CD.desc, rows + CD.scroll, 0);
    gfx_pop_clip(g);
    d_fade_edges(g, ix, iy, tw, rows * lh, CD.scroll > 0, CD.scroll < CD.max_scroll, C_POPUP);
    int bx = ix;
    for (int i = 0; i < CD.nb; i++) bx += button(g, bx, y + h - E(4.6), btn_label(CD.btn[i]), i == CD.focus) + E(1);
}
void card_open(channel *c, const prog *p) {
    if (!c || !p) return;
    CL.name = "card";
    CL.key = card_key;
    CL.draw = card_draw;
    memset(&CD, 0, sizeof CD);
    CD.c = c;
    CD.start = p->start;
    CD.stop = p->stop;
    snprintf(CD.title, sizeof CD.title, "%s", p->title ? p->title : "");
    snprintf(CD.sub, sizeof CD.sub, "%s", p->sub ? p->sub : "");
    snprintf(CD.cat, sizeof CD.cat, "%s", p->cat ? p->cat : "");
    snprintf(CD.icon, sizeof CD.icon, "%s", p->icon ? p->icon : "");
    snprintf(CD.desc, sizeof CD.desc, "%s", p->desc ? p->desc : "");
    card_buttons();
    layer_push(&CL);
}

/* ================= недавние каналы ================= */
static struct { channel *ch[HISTORY_SIZE]; int n, focus, top; } RC;
static layer RL;
static int recent_has_long(layer *l, int k) { (void)l; return k == KEY_OK; }
static int recent_key(layer *l, int k, int ev) {
    (void)l;
    if ((k == KEY_BACK && ev == EV_SHORT) || k == KEY_LIST) { layer_pop(&RL); return 1; }
    if (!RC.n) return 1;
    if (k == KEY_UP || k == KEY_DOWN) RC.focus = (RC.focus + (k == KEY_DOWN ? 1 : -1) + RC.n) % RC.n;
    else if (k == KEY_OK && ev == EV_LONG) { channel *c = RC.ch[RC.focus]; layer_pop(&RL); tv_open_menu(c, NULL, 1); }
    else if (k == KEY_OK) { channel *c = RC.ch[RC.focus]; layer_pop(&RL); tv_play(c, NULL); }
    else if (k == KEY_MENU) { channel *c = RC.ch[RC.focus]; layer_pop(&RL); tv_open_menu(c, NULL, 1); }
    return 1;
}
/* Сколько строк влезает между верхним и нижним полем (окно не выше экрана). */
static int recent_vis(void) {
    int row = E(5.0), v = (SH - E(4.8) - E(6)) / row;
    return v < 1 ? 1 : v;
}
static void recent_draw(layer *l, gfx_t *g) {
    (void)l;
    int vis = recent_vis();
    if (RC.n > vis) {
        if (RC.focus < RC.top) RC.top = RC.focus;
        if (RC.focus >= RC.top + vis) RC.top = RC.focus - vis + 1;
        if (RC.top > RC.n - vis) RC.top = RC.n - vis;
        if (RC.top < 0) RC.top = 0;
    } else RC.top = 0;
    int rows = RC.n ? RC.n : 1;
    if (rows > vis) rows = vis;
    int w = E(44), row = E(5.0), h = E(6) + rows * row, x = E(4), y = SH - E(2.4) - h;
    d_hgrad(g, 0, 0, SW / 2, SH, BA(0.6), 0);
    d_round(g, x, y, w, h, E(1), C_POPUP);
    d_text(g, x + E(1.5), y + E(1.2), 1.5, F_B, C_WHITE, tr(S_RECENT_CHANNELS), w - E(3));
    int ly = y + E(4.4);
    if (!RC.n) d_text(g, x + E(1.5), ly, 1.25, F_R, C_TEXT3, tr(S_NO_RECENT), w - E(3));
    int64_t now = clock_now();
    for (int k = 0; k < rows && RC.top + k < RC.n; k++) {
        int i = RC.top + k;
        channel *c = RC.ch[i];
        int ry = ly + k * row, foc = i == RC.focus;
        if (foc) d_round(g, x + E(0.8), ry, w - E(1.6), row - E(0.4), E(0.5), C_WHITE);
        uint32_t fg = foc ? 0xff101010u : C_WHITE, fg2 = foc ? 0xff404040u : C_TEXT3;
        int lh = row - E(1.2), lw = lh * 96 / 66;
        d_logo(g, c, x + E(1.2), ry + E(0.4), lw, lh, E(0.35), 1, foc);
        int tx = x + E(1.2) + lw + E(1);
        d_text(g, tx, ry + E(0.4), 1.3, F_S, fg, c->name, w - (tx - x) - E(2));
        int pi = repo_prog_at(c, now);
        if (pi >= 0) {
            prog p = repo_prog(c, pi);
            d_text(g, tx, ry + E(2.3), 1.1, F_R, fg2, p.title, w - (tx - x) - E(7));
            int bw = E(4), bx = x + w - E(1.6) - bw;
            double frac = (double)(now - p.start) / (double)(p.stop - p.start);
            d_round(g, bx, ry + E(2.9), bw, E(0.3), E(0.15), foc ? 0x40000000u : C_FILL_MARKED);
            d_round(g, bx, ry + E(2.9), (int)(bw * frac), E(0.3), E(0.15), fg);
        }
    }
}
void recent_open(void) {
    RL.name = "recent";
    RL.key = recent_key;
    RL.has_long = recent_has_long;
    RL.draw = recent_draw;
    RC.n = repo_recent(T.cur, RC.ch, HISTORY_SIZE - 1);
    RC.focus = RC.top = 0;
    layer_push(&RL);
}

/* ================= «Управление» ================= */
static int help_scroll;
static layer HL;
static const int HELP[] = {-S_HELP_PLAYER, S_HELP_P1, S_HELP_P2, S_HELP_P3, S_HELP_P4, S_HELP_P5, S_HELP_P6, S_HELP_P7, S_HELP_P8,
                           -S_HELP_GUIDE, S_HELP_G1, S_HELP_G2, S_HELP_G3, -S_HELP_WINDOWS, S_HELP_W1, S_HELP_W3, S_HELP_W4, S_HELP_W5, S_HELP_W2};
#define NHELP ((int)(sizeof HELP / sizeof HELP[0]))
static int help_key(layer *l, int k, int ev) {
    (void)l;
    if (k == KEY_OK || (k == KEY_BACK && ev == EV_SHORT)) layer_pop(&HL);
    else if (k == KEY_UP || k == KEY_DOWN) help_scroll = CLAMP(help_scroll + (k == KEY_DOWN ? 1 : -1), 0, NHELP - 1);
    else if (k == KEY_CH_UP || k == KEY_CH_DOWN) help_scroll = CLAMP(help_scroll + (k == KEY_CH_DOWN ? 8 : -8), 0, NHELP - 1);
    return 1;
}
static void help_draw(layer *l, gfx_t *g) {
    (void)l;
    int w = E(62), h = SH - E(6), x = (SW - w) / 2, y = E(3);
    window(g, x, y, w, h);
    d_text(g, x + E(2), y + E(1.6), 1.5, F_B, C_WHITE, tr(S_CONTROLS), w - E(4));
    int ly = y + E(5), bottom = y + h - E(1.6);
    gfx_push_clip(g, x, ly, w, bottom - ly);
    for (int i = help_scroll; i < NHELP && ly < bottom; i++) {
        int id = HELP[i];
        if (id < 0) { ly += E(0.6); char up[120]; text_upper(up, sizeof up, tr(-id)); d_text(g, x + E(2), ly, 1.1, F_S, WA(0.5), up, w - E(4)); ly += E(2.4); continue; }
        int n = d_lines(g, x + E(2), ly, w - E(4), 1.25, F_R, C_TEXT2, tr(id), 3, 0);
        ly += n * d_line_h(1.25, F_R) + E(0.6);
    }
    gfx_pop_clip(g);
}
void help_open(void) {
    HL.name = "help";
    HL.key = help_key;
    HL.draw = help_draw;
    help_scroll = 0;
    layer_push(&HL);
}

/* ================= скрытое ================= */
typedef struct { int is_group; char key[160], title[200]; } hid;
static struct { hid v[200]; int n, focus, top; } HD;
static layer HDL;
static void hidden_build(void) {
    HD.n = 0;
    for (int pass = 0; pass < 2; pass++) {
        smap *m = pass ? &R.hidden_ch : &R.hidden_gr;
        for (int i = 0; i < m->cap && HD.n < 200; i++) {
            if (!m->keys[i] || !m->vals[i]) continue;
            hid *h = &HD.v[HD.n++];
            h->is_group = !pass;
            snprintf(h->key, sizeof h->key, "%s", m->keys[i]);
            char k[200];
            snprintf(k, sizeof k, "hname.%s", m->keys[i]);
            const char *nm = pass ? store_get(k) : NULL;
            if (pass) snprintf(h->title, sizeof h->title, "%s", nm ? nm : m->keys[i]);
            else snprintf(h->title, sizeof h->title, tr(S_CATEGORY_Q), m->keys[i][0] ? m->keys[i] : tr(S_NO_GROUP));
        }
    }
    if (HD.focus >= HD.n) HD.focus = HD.n - 1;
    if (HD.focus < 0) HD.focus = 0;
}
static int hidden_key(layer *l, int k, int ev) {
    (void)l;
    if (k == KEY_BACK && ev == EV_SHORT) { layer_pop(&HDL); return 1; }
    if (!HD.n) return 1;
    if (k == KEY_UP || k == KEY_DOWN) HD.focus = (HD.focus + (k == KEY_DOWN ? 1 : -1) + HD.n) % HD.n;
    else if (k == KEY_OK && ev == EV_SHORT) { repo_unhide(HD.v[HD.focus].is_group, HD.v[HD.focus].key); hidden_build(); }
    return 1;
}
static void hidden_draw(layer *l, gfx_t *g) {
    (void)l;
    int w = E(46), row = E(3.2), rows = 12, h = E(8) + rows * row, x = (SW - w) / 2, y = (SH - h) / 2;
    window(g, x, y, w, h);
    d_text(g, x + E(2), y + E(1.6), 1.5, F_B, C_WHITE, tr(S_HIDDEN_ITEMS), w - E(4));
    int ly = y + E(4.8);
    if (!HD.n) d_text(g, x + E(2), ly, 1.25, F_R, C_TEXT3, tr(S_NOTHING_HIDDEN), w - E(4));
    if (HD.top > HD.focus) HD.top = HD.focus;
    if (HD.focus >= HD.top + rows) HD.top = HD.focus - rows + 1;
    for (int r = 0; r < rows && HD.top + r < HD.n; r++) {
        int i = HD.top + r, foc = i == HD.focus;
        if (foc) d_round(g, x + E(1), ly + r * row, w - E(2), row - E(0.3), E(0.4), C_WHITE);
        d_text_mid(g, x + E(2), ly + r * row + (row - E(0.3)) / 2, 1.25, F_R, foc ? 0xff101010u : C_WHITE, HD.v[i].title, w - E(4));
    }
    if (HD.n) d_text(g, x + E(2), y + h - E(2.6), 1.1, F_R, C_TEXT3, tr(S_UNHIDE_HINT), w - E(4));
}
void hidden_open(void) {
    HDL.name = "hidden";
    HDL.key = hidden_key;
    HDL.draw = hidden_draw;
    HD.focus = HD.top = 0;
    hidden_build();
    layer_push(&HDL);
}

/* ================= мастер ================= */
static int welcome_focus;
static layer WL;
static int welcome_key(layer *l, int k, int ev) {
    (void)l;
    if (k == KEY_LEFT || k == KEY_UP) welcome_focus = (welcome_focus + 2) % 3;
    else if (k == KEY_RIGHT || k == KEY_DOWN) welcome_focus = (welcome_focus + 1) % 3;
    else if (k == KEY_OK && ev == EV_SHORT) {
        if (welcome_focus == 2) app_quit();
        else if (welcome_focus == 1) form_playlist_phone();
        else form_playlist(-1);
    }
    else if (k == KEY_BACK && ev == EV_SHORT) app_quit();
    return 1;
}
static void welcome_draw(layer *l, gfx_t *g) {
    (void)l;
    d_clear(g, C_SETTINGS_BG);
    int cy = SH / 2 - E(6);
    d_text_c(g, 0, SW, cy - E(4), 2.6, F_B, C_WHITE, tr(S_WELCOME));
    d_lines(g, SW / 2 - E(26), cy + E(0.4), E(52), 1.35, F_R, C_TEXT2, tr(S_WELCOME_HINT), 3, 0.2);
    const char *b[3] = {tr(S_ADD_PLAYLIST), tr(S_PHONE), tr(S_EXIT)};
    int tw = E(2);
    for (int i = 0; i < 3; i++) tw += d_text_w(1.25, F_S, b[i]) + E(3);
    int bx = (SW - tw) / 2;
    for (int i = 0; i < 3; i++) bx += button(g, bx, cy + E(6), b[i], welcome_focus == i) + E(1);
}
void welcome_open(void) {
    WL.name = "welcome";
    WL.key = welcome_key;
    WL.draw = welcome_draw;
    WL.covers = 1;
    welcome_focus = 0;
    layer_push(&WL);
}

/* ================= напоминания ================= */
static struct { int idx[32], n, cur, focus, user_moved; } RM;
static layer RML;
static int rem_missed(const reminder *r) { return r->start <= clock_now() - 60 && r->stop <= clock_now(); }
static channel *rem_channel(const reminder *r) {
    playlist *p = repo_cur();
    /* чужой плейлист: канал ищется только в нём, а не по одноимённому в текущем */
    return p && p->id == r->playlist ? repo_by_key(r->key) : NULL;
}
static int rem_archive(const reminder *r) {
    /* глубина архива записана при создании: у чужого плейлиста канала здесь нет */
    int days = r->archive_days;
    if (days <= 0) { channel *c = rem_channel(r); days = c && repo_has_archive(c) ? c->archive_days : 0; }
    return days > 0 && r->start >= clock_now() - (int64_t)days * 86400;
}
static void rem_rebuild(void) {
    int64_t now = clock_now();
    RM.n = 0;
    for (int i = 0; i < R.nrem && RM.n < 32; i++) {
        reminder *r = &R.rem[i];
        if (r->snooze > now) continue;
        int due = r->start - (int64_t)P.remind_before * 60 <= now && now < r->stop;
        int missed = r->stop <= now && !r->shown && rem_archive(r);
        if (due && !r->shown) RM.idx[RM.n++] = i;
        else if (missed) RM.idx[RM.n++] = i;
    }
    if (RM.cur >= RM.n) RM.cur = RM.n - 1;
    if (RM.cur < 0) RM.cur = 0;
}
static void rem_done(int remove) {
    if (!RM.n) return;
    int i = RM.idx[RM.cur];
    if (remove) repo_reminder_remove(i);
    else { R.rem[i].shown = 1; repo_reminders_save(); }
    rem_rebuild();
    RM.focus = 0;
    if (!RM.n) layer_pop(&RML);
}
static int rem_key(layer *l, int k, int ev) {
    (void)l;
    if (!RM.n) { layer_pop(&RML); return 1; }
    reminder *r = &R.rem[RM.idx[RM.cur]];
    int missed = rem_missed(r);
    int nb = 3 + (RM.n > 1);
    if (k == KEY_UP || k == KEY_DOWN) { RM.cur = (RM.cur + (k == KEY_DOWN ? 1 : -1) + RM.n) % RM.n; RM.user_moved = 1; RM.focus = 0; }
    else if (k == KEY_LEFT || k == KEY_RIGHT) RM.focus = CLAMP(RM.focus + (k == KEY_RIGHT ? 1 : -1), 0, nb - 1);
    else if (k == KEY_BACK && ev == EV_SHORT) { for (int i = 0; i < RM.n; i++) R.rem[RM.idx[i]].shown = 1; repo_reminders_save(); layer_pop(&RML); }
    else if (k == KEY_OK && ev == EV_SHORT) {
        channel *c = rem_channel(r);
        int64_t start = r->start;
        switch (RM.focus) {
        case 0:
            if (!c && r->playlist != (repo_cur() ? repo_cur()->id : 0)) {
                /* напоминание другого плейлиста: перейти в него, затем включить канал */
                int idx = -1;
                for (int i2 = 0; i2 < R.npl; i2++) if (R.pl[i2].id == r->playlist) { idx = i2; break; }
                char key[160], path[700];
                snprintf(key, sizeof key, "%s", r->key);
                if (idx >= 0) repo_channels_path(R.pl[idx].id, path, sizeof path);
                if (idx >= 0 && file_exists(path)) {
                    rem_done(1);
                    layer_pop_all();
                    tv_preview_playlist(idx);
                    channel *nc = repo_by_key(key);
                    if (nc) { if (missed) tv_play_archive(nc, start, NULL); else tv_play(nc, NULL); }
                } else tv_toast(tr(S_LOADING_PLAYLIST));
                break;
            }
            rem_done(!missed);
            layer_pop_all();
            if (c) { if (missed) tv_play_archive(c, start, NULL); else tv_play(c, NULL); }
            break;
        case 1:
            if (missed) rem_done(0);   /* «Позже»: остаётся в списке */
            else { r->snooze = clock_now() + 300; repo_reminders_save(); rem_rebuild(); if (!RM.n) layer_pop(&RML); }
            break;
        case 2: rem_done(1); break;
        default:   /* «Закрыть все» */
            for (int k2 = 0; k2 < RM.n; k2++) R.rem[RM.idx[k2]].shown = 1;
            repo_reminders_save();
            layer_pop(&RML);
            break;
        }
    }
    return 1;
}
static void rem_draw(layer *l, gfx_t *g) {
    (void)l;
    if (!RM.n) return;
    reminder *r = &R.rem[RM.idx[RM.cur]];
    int missed = rem_missed(r);
    int w = E(50), h = E(19), x = SW - E(4) - w, y = E(2.4);
    d_round(g, x, y, w, h, E(1), C_POPUP);
    int ix = x + E(1.8), iw = w - E(3.6), iy = y + E(1.4);
    int64_t now = clock_now();
    char head[200], left[32];
    if (missed) snprintf(head, sizeof head, "%s", tr(S_MISSED));
    else if (r->start <= now) snprintf(head, sizeof head, "%s", tr(S_REMINDER_STARTED));
    else { snprintf(left, sizeof left, "%s", trp(S_MINUTES_N, (long)((r->start - now + 59) / 60))); snprintf(head, sizeof head, tr(S_REMINDER_IN), left); }
    d_text(g, ix, iy, 1.15, F_S, C_ARCHIVE, head, iw);
    if (RM.n > 1) { char cnt[16]; snprintf(cnt, sizeof cnt, "%d / %d", RM.cur + 1, RM.n); d_text_r(g, x + w - E(1.8), iy, 1.15, F_R, C_TEXT3, cnt); }
    char hm[8], line[300];
    fmt_hm(r->start, hm);
    snprintf(line, sizeof line, "%s  ·  %s", r->name, hm);
    d_text(g, ix, iy + E(2.4), 1.2, F_R, C_TEXT2, line, iw);
    d_lines(g, ix, iy + E(4.6), iw, 1.6, F_B, C_WHITE, r->title, 2, 0);
    const char *b[4] = {missed ? tr(S_WATCH_ARCHIVE) : tr(S_WATCH), missed ? tr(S_LATER) : tr(S_SNOOZE), missed ? tr(S_DELETE) : tr(S_CLOSE), tr(S_CLOSE_ALL)};
    int bx = ix, nb = 3 + (RM.n > 1);
    for (int i = 0; i < nb; i++) bx += button(g, bx, y + h - E(4.4), b[i], i == RM.focus) + E(0.8);
}
void reminders_check(void) {
    if (layer_has(&RML)) { int cur = RM.idx[RM.cur]; rem_rebuild(); if (!RM.user_moved) for (int i = 0; i < RM.n; i++) if (RM.idx[i] == cur) RM.cur = i; if (!RM.n) layer_pop(&RML); return; }
    /* пропущенные без архива — убрать; напоминания другого плейлиста не трогаем:
     * их канал здесь не загружен, а при переходе в тот плейлист они ещё нужны */
    int64_t now = clock_now();
    playlist *cp = repo_cur();
    int curid = cp ? cp->id : 0;
    for (int i = R.nrem - 1; i >= 0; i--) {
        reminder *r = &R.rem[i];
        if (r->playlist == curid && r->stop <= now && (!rem_archive(r) || r->start < now - 30 * 86400)) repo_reminder_remove(i);
    }
    rem_rebuild();
    if (!RM.n) return;
    layer *t = layer_top();
    if (t && t->name && (!strcmp(t->name, "pin") || !strcmp(t->name, "form") || !strcmp(t->name, "welcome"))) return;
    RML.name = "reminder";
    RML.key = rem_key;
    RML.draw = rem_draw;
    RM.cur = 0;
    RM.focus = 0;
    RM.user_moved = 0;
    layer_push(&RML);
}

/* ================= подтверждение ================= */
static struct { char text[400], yes[80]; confirm_fn fn; void *ctx; int focus; } CF;
static layer CFL;
static int confirm_key(layer *l, int k, int ev) {
    (void)l;
    if (k == KEY_LEFT || k == KEY_RIGHT) CF.focus = !CF.focus;
    else if (k == KEY_BACK && ev == EV_SHORT) layer_pop(&CFL);
    else if (k == KEY_OK && ev == EV_SHORT) {
        confirm_fn fn = CF.fn;
        void *ctx = CF.ctx;
        int yes = CF.focus == 1;
        layer_pop(&CFL);
        if (yes && fn) fn(ctx);
    }
    return 1;
}
static void confirm_draw(layer *l, gfx_t *g) {
    (void)l;
    int w = E(44), h = E(16), x = (SW - w) / 2, y = (SH - h) / 2;
    window(g, x, y, w, h);
    d_lines(g, x + E(2), y + E(2), w - E(4), 1.4, F_S, C_WHITE, CF.text, 4, 0.2);
    int bx = x + E(2);
    bx += button(g, bx, y + h - E(4.6), tr(S_CANCEL), CF.focus == 0) + E(1);
    button(g, bx, y + h - E(4.6), CF.yes, CF.focus == 1);
}
void confirm_open(const char *text, const char *yes, confirm_fn fn, void *ctx) {
    CFL.name = "confirm";
    CFL.key = confirm_key;
    CFL.draw = confirm_draw;
    snprintf(CF.text, sizeof CF.text, "%s", text);
    snprintf(CF.yes, sizeof CF.yes, "%s", yes);
    CF.fn = fn;
    CF.ctx = ctx;
    CF.focus = 0;
    layer_push(&CFL);
}

/* ================= выбор из списка ================= */
static struct { char title[160]; char items[40][200]; int n, focus, top; choose_fn fn; void *ctx; } CH;
static layer CHL;
static int choose_key(layer *l, int k, int ev) {
    (void)l;
    if (k == KEY_BACK && ev == EV_SHORT) { layer_pop(&CHL); return 1; }
    if (!CH.n) return 1;
    if (k == KEY_UP || k == KEY_DOWN) CH.focus = (CH.focus + (k == KEY_DOWN ? 1 : -1) + CH.n) % CH.n;
    else if (k == KEY_OK && ev == EV_SHORT) {
        choose_fn fn = CH.fn;
        void *ctx = CH.ctx;
        int i = CH.focus;
        layer_pop(&CHL);
        if (fn) fn(i, ctx);
    }
    return 1;
}
static void choose_draw(layer *l, gfx_t *g) {
    (void)l;
    int rows = CH.n < 12 ? CH.n : 12, row = E(3.2);
    int w = E(40), h = E(7) + rows * row, x = (SW - w) / 2, y = (SH - h) / 2;
    window(g, x, y, w, h);
    d_text(g, x + E(2), y + E(1.6), 1.5, F_B, C_WHITE, CH.title, w - E(4));
    if (CH.top > CH.focus) CH.top = CH.focus;
    if (CH.focus >= CH.top + rows) CH.top = CH.focus - rows + 1;
    int ly = y + E(4.8);
    for (int r = 0; r < rows; r++) {
        int i = CH.top + r, foc = i == CH.focus;
        if (foc) d_round(g, x + E(1), ly + r * row, w - E(2), row - E(0.3), E(0.4), C_WHITE);
        d_text_mid(g, x + E(2), ly + r * row + (row - E(0.3)) / 2, 1.25, F_R, foc ? 0xff101010u : C_WHITE, CH.items[i], w - E(4));
    }
}
void choose_open(const char *title, const char *const *items, int n, int sel, choose_fn fn, void *ctx) {
    CHL.name = "choose";
    CHL.key = choose_key;
    CHL.draw = choose_draw;
    snprintf(CH.title, sizeof CH.title, "%s", title);
    CH.n = n < 40 ? n : 40;
    for (int i = 0; i < CH.n; i++) snprintf(CH.items[i], sizeof CH.items[i], "%s", items[i]);
    CH.focus = sel >= 0 && sel < CH.n ? sel : 0;
    CH.top = 0;
    CH.fn = fn;
    CH.ctx = ctx;
    layer_push(&CHL);
}
