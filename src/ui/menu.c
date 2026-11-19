/* Меню канала (docs/controls.md, «Меню канала»): шторка справа — плитки частых действий и группы
 * «Просмотр», «Канал», «Приложение»; страницы второго уровня (категория, список каналов, плейлист). */
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
#include "player/player.h"
#include <stdio.h>
#include <string.h>

enum { A_WATCH = 1, A_LIVE, A_FROM_START, A_SCHEDULE, A_FAV, A_RECENT, A_ABOUT, A_AUDIO, A_ASPECT, A_SLEEP, A_FAV_UP, A_FAV_DOWN,
       A_HIDE, A_LOCK, A_CATEGORY, A_GUIDE, A_SEARCH, A_TELETEXT, A_SUBS, A_REMINDERS, A_LIST, A_PLAYLIST_EPG, A_HELP, A_SETTINGS,
       A_CAT_HIDE, A_CAT_LOCK, A_VIEW, A_ADAPTIVE, A_HIDDEN, A_PLAYLIST, A_REFRESH_PL, A_REFRESH_EPG };
enum { P_MAIN, P_CATEGORY, P_LIST, P_PLAYLIST };
enum { E_TILE, E_HEAD, E_ITEM };

typedef struct { int kind, id, icon, sub; char label[120], value[120]; } ent;
#define MAXE 40
static struct {
    channel *c;
    char group[160];
    int from_guide, page, focus, tile, main_focus;
    ent e[MAXE]; int n;
    int ntiles;
} M;
static layer L;

static void add(int kind, int id, int icon, int sub, const char *label, const char *value) {
    if (M.n >= MAXE) return;
    ent *e = &M.e[M.n++];
    e->kind = kind; e->id = id; e->icon = icon; e->sub = sub;
    snprintf(e->label, sizeof e->label, "%s", label);
    snprintf(e->value, sizeof e->value, "%s", value ? value : "");
}

static const int SLEEP[] = {0, 15, 30, 45, 60, 90, 120};
static const char *aspect_name(int a) { int s[] = {S_ASPECT_FIT, S_ASPECT_STRETCH, S_ASPECT_16_9, S_ASPECT_4_3}; return tr(s[a & 3]); }
static const char *sleep_name(void) { int m = tv_sleep_minutes(); return m ? trp(S_MINUTES_N, m) : tr(S_OFF); }

static void build(void) {
    M.n = 0;
    channel *c = M.c;
    int64_t now = clock_now();
    int playing = c == T.cur;
    int pi = repo_prog_at(c, now);
    if (M.page == P_MAIN) {
        int tiles[8], icons[8], labels[8], nt = 0;
#define TILE(a, i, s) do { tiles[nt] = (a); icons[nt] = (i); labels[nt] = (s); nt++; } while (0)
        if (M.from_guide || !playing) TILE(A_WATCH, I_PLAY, S_MENU_WATCH);
        if (playing && T.arc) TILE(A_LIVE, I_LIVE, S_MENU_LIVE);
        if (repo_has_archive(c) && pi >= 0) TILE(A_FROM_START, I_ARCHIVE, S_MENU_FROM_START);
        TILE(A_SCHEDULE, I_LIST, S_MENU_SCHEDULE);
        TILE(A_FAV, I_HEART, repo_is_fav(c) ? S_MENU_FAV_REMOVE : S_MENU_FAV_ADD);
        TILE(A_RECENT, I_HISTORY, S_MENU_RECENT);
#undef TILE
        M.ntiles = nt < 4 ? nt : 4;
        for (int i = 0; i < M.ntiles; i++) add(E_TILE, tiles[i], icons[i], 0, tr(labels[i]), NULL);
        add(E_HEAD, 0, -1, 0, tr(S_GROUP_VIEW), NULL);
        for (int i = M.ntiles; i < nt; i++) add(E_ITEM, tiles[i], -1, 0, tr(labels[i]), NULL);
        if (pi >= 0) add(E_ITEM, A_ABOUT, -1, 0, tr(S_ABOUT_PROGRAMME), NULL);
        if (playing && pl_audio_count() > 1) add(E_ITEM, A_AUDIO, -1, 0, tr(S_AUDIO_TRACKS), pl_audio_label(pl_audio_selected()));
        if (playing && !pl_software()) add(E_ITEM, A_ASPECT, -1, 0, tr(S_ASPECT), aspect_name(P.aspect));
        /* дорожки известны (через ретранслятор — из его разбора, напрямую — от системного плеера): нет — нет и пункта */
        if (playing && tv_has_teletext()) add(E_ITEM, A_TELETEXT, -1, 0, tr(S_TELETEXT), NULL);
        if (playing && (tv_has_subtitles() || T.subs)) add(E_ITEM, A_SUBS, -1, 1, tr(S_SUBTITLES_MENU), subs_current_label());
        add(E_ITEM, A_SLEEP, -1, 0, tr(S_SLEEP), sleep_name());
        add(E_HEAD, 0, -1, 0, tr(S_GROUP_CHANNEL), NULL);
        group *g = repo_group(M.group);
        if (g && g->kind == GK_FAV) { add(E_ITEM, A_FAV_UP, -1, 0, tr(S_MOVE_UP), NULL); add(E_ITEM, A_FAV_DOWN, -1, 0, tr(S_MOVE_DOWN), NULL); }
        add(E_ITEM, A_HIDE, -1, 0, tr(S_HIDE_CHANNEL), NULL);
        add(E_ITEM, A_LOCK, -1, 0, tr(repo_channel_locked(c) && P.pin[0] ? S_UNLOCK_CHANNEL : S_LOCK_CHANNEL), NULL);
        add(E_ITEM, A_CATEGORY, -1, 1, trf(S_CATEGORY_Q, c->group[0] ? c->group : tr(S_NO_GROUP)), NULL);
        add(E_HEAD, 0, -1, 0, tr(S_GROUP_APP), NULL);
        if (!M.from_guide) add(E_ITEM, A_GUIDE, -1, 0, tr(S_GUIDE), NULL);
        add(E_ITEM, A_SEARCH, -1, 0, tr(S_SEARCH), NULL);
        char nrem[16];
        snprintf(nrem, sizeof nrem, "%d", R.nrem);
        add(E_ITEM, A_REMINDERS, -1, 0, tr(S_REMINDERS), R.nrem ? nrem : NULL);
        add(E_ITEM, A_LIST, -1, 1, tr(S_CHANNEL_LIST), NULL);
        add(E_ITEM, A_PLAYLIST_EPG, -1, 1, tr(S_PLAYLIST_EPG), NULL);
        add(E_ITEM, A_HELP, -1, 0, tr(S_CONTROLS), NULL);
        add(E_ITEM, A_SETTINGS, -1, 0, tr(S_SETTINGS), NULL);
    } else if (M.page == P_CATEGORY) {
        M.ntiles = 0;
        add(E_HEAD, 0, -1, 0, c->group[0] ? c->group : tr(S_NO_GROUP), NULL);
        add(E_ITEM, A_CAT_HIDE, -1, 0, tr(S_HIDE_CATEGORY), NULL);
        add(E_ITEM, A_CAT_LOCK, -1, 0, tr(repo_group_locked(c->group) && P.pin[0] ? S_UNLOCK_CATEGORY : S_LOCK_CATEGORY), NULL);
    } else if (M.page == P_LIST) {
        M.ntiles = 0;
        add(E_HEAD, 0, -1, 0, tr(S_CHANNEL_LIST), NULL);
        add(E_ITEM, A_VIEW, -1, 0, tr(S_VIEW), tr(P.guide_mode == GUIDE_LIST ? S_GUIDE_LIST : S_GUIDE_GRID));
        add(E_ITEM, A_ADAPTIVE, -1, 0, tr(S_ADAPTIVE), tr(P.adaptive ? S_ON : S_OFF));
        char n[16];
        snprintf(n, sizeof n, "%d", repo_hidden_count());
        add(E_ITEM, A_HIDDEN, -1, 0, tr(S_HIDDEN), n);
    } else if (M.page == P_PLAYLIST) {
        M.ntiles = 0;
        add(E_HEAD, 0, -1, 0, tr(S_PLAYLIST_EPG), NULL);
        if (R.npl > 1) add(E_ITEM, A_PLAYLIST, -1, 0, tr(S_PLAYLIST), repo_cur() ? repo_cur()->name : "");
        add(E_ITEM, A_REFRESH_PL, -1, 0, tr(S_REFRESH_PLAYLIST), NULL);
        add(E_ITEM, A_REFRESH_EPG, -1, 0, tr(S_REFRESH_EPG), NULL);
    }
}

/* строки фокуса: плитки — одна строка (0), дальше пункты */
static int nrows(void) {
    int n = M.ntiles ? 1 : 0;
    for (int i = 0; i < M.n; i++) if (M.e[i].kind == E_ITEM) n++;
    return n;
}
static ent *row_ent(int r) {
    if (M.ntiles) { if (r == 0) return &M.e[M.tile < M.ntiles ? M.tile : 0]; r--; }
    for (int i = 0; i < M.n; i++) if (M.e[i].kind == E_ITEM && r-- == 0) return &M.e[i];
    return NULL;
}

static void close_menu(void) { layer_pop(&L); }

typedef struct { int is_group; char key[160]; } lock_ctx;
static lock_ctx lk;
static void do_lock(void *x) {
    lock_ctx *k = x;
    int on = k->is_group ? !repo_group_locked(k->key) : !repo_channel_locked(repo_by_key(k->key));
    repo_set_locked(k->is_group, k->key, on);
    tv_toast(tr(on ? S_LOCKED : S_OFF));
    R.rev++;
}
static void lock_toggle(int is_group) {
    lk.is_group = is_group;
    snprintf(lk.key, sizeof lk.key, "%s", is_group ? M.c->group : M.c->key);
    close_menu();
    if (!P.pin[0]) pin_create(do_lock, &lk);   /* первая блокировка — сначала PIN */
    else pin_ask(do_lock, NULL, &lk);
}

static void choose_playlist(int i, void *ctx) {
    (void)ctx;
    if (i == R.cur || i < 0 || i >= R.npl) return;
    loaders_cancel();
    R.cur = i;
    char path[700];
    repo_channels_path(R.pl[i].id, path, sizeof path);
    if (!file_exists(path)) { layer_pop_all(); tv_playlist_changed(0); load_playlist(i, 1); return; }
    tv_playlist_changed(0);
    load_epg(0);
}

static void activate(ent *e, int dir) {
    channel *c = M.c;
    int64_t now = clock_now();
    switch (e->id) {
    case A_WATCH: close_menu(); layer_pop_all(); tv_play(c, M.group); break;
    case A_LIVE: close_menu(); tv_go_live(); break;
    case A_FROM_START: {
        int pi = repo_prog_at(c, c == T.cur ? tv_position() : now);
        close_menu();
        layer_pop_all();
        if (pi >= 0) tv_play_archive(c, repo_prog(c, pi).start, M.group);
        break;
    }
    case A_SCHEDULE: close_menu(); tv_open_schedule(c, 0); break;
    case A_FAV: { int was = repo_is_fav(c); repo_toggle_fav(c); tv_toast(tr(was ? S_REMOVED_FAV : S_ADDED_FAV)); close_menu(); break; }
    case A_RECENT: close_menu(); tv_open_recent(); break;
    case A_ABOUT: {
        int pi = repo_prog_at(c, c == T.cur ? tv_position() : now);
        close_menu();
        if (pi >= 0) { prog p = repo_prog(c, pi); card_open(c, &p); }
        break;
    }
    case A_AUDIO: {
        int n = pl_audio_count(), cur = pl_audio_selected();
        if (n > 1) pl_audio_select(((cur + (dir ? dir : 1)) % n + n) % n);
        break;
    }
    case A_ASPECT:
        P.aspect = ((P.aspect + (dir ? dir : 1)) % 4 + 4) % 4;
        pl_set_aspect(P.aspect);
        prefs_save();
        break;
    case A_SLEEP: {
        int m = tv_sleep_minutes(), k = 0;
        for (int i = 0; i < 7; i++) if (SLEEP[i] >= m) { k = i; break; }
        if (m && SLEEP[k] != m) k--;
        k = ((k + (dir ? dir : 1)) % 7 + 7) % 7;
        tv_set_sleep(SLEEP[k]);
        break;
    }
    case A_FAV_UP: case A_FAV_DOWN: repo_move_fav(c, e->id == A_FAV_UP ? -1 : 1); break;
    case A_HIDE: tv_toast(trf(S_HIDDEN_DONE, c->name)); repo_hide_channel(c); close_menu(); break;
    case A_LOCK: lock_toggle(0); return;
    case A_CATEGORY: M.main_focus = M.focus; M.page = P_CATEGORY; M.focus = 0; break;
    case A_GUIDE: close_menu(); tv_open_guide(); break;
    case A_SEARCH: close_menu(); search_open(); break;
    case A_TELETEXT: close_menu(); tv_teletext(); break;
    case A_SUBS: close_menu(); subs_menu_open(); break;
    case A_REMINDERS: close_menu(); tv_open_schedule(c, 1); break;
    case A_LIST: M.main_focus = M.focus; M.page = P_LIST; M.focus = 0; break;
    case A_PLAYLIST_EPG: M.main_focus = M.focus; M.page = P_PLAYLIST; M.focus = 0; break;
    case A_HELP: close_menu(); help_open(); break;
    case A_SETTINGS: close_menu(); tv_open_settings(); break;
    case A_CAT_HIDE: tv_toast(trf(S_HIDDEN_DONE, c->group)); repo_hide_group(c->group); close_menu(); break;
    case A_CAT_LOCK: lock_toggle(1); return;
    case A_VIEW: P.guide_mode = P.guide_mode == GUIDE_LIST ? GUIDE_GRID : GUIDE_LIST; prefs_save(); break;
    case A_ADAPTIVE: P.adaptive = !P.adaptive; prefs_save(); repo_apply_view(); break;
    case A_HIDDEN: close_menu(); hidden_open(); break;
    case A_PLAYLIST: {
        static const char *names[32];
        int n = R.npl < 32 ? R.npl : 32;
        for (int i = 0; i < n; i++) names[i] = R.pl[i].name;
        close_menu();
        choose_open(tr(S_PLAYLIST), names, n, R.cur, choose_playlist, NULL);
        return;
    }
    case A_REFRESH_PL: load_playlist(R.cur, 1); tv_toast(tr(S_LOADING_PLAYLIST)); close_menu(); break;
    case A_REFRESH_EPG: load_epg(1); tv_toast(tr(S_EPG_STATUS_LOADING)); close_menu(); break;
    }
    if (layer_has(&L)) build();
}

static int key(layer *l, int k, int ev) {
    (void)l;
    int n = nrows();
    if (k == KEY_MENU || (k == KEY_BACK && ev == EV_SHORT)) {
        if (k == KEY_BACK && M.page != P_MAIN) { M.page = P_MAIN; build(); M.focus = M.main_focus; return 1; }
        close_menu();
        return 1;
    }
    if (k == KEY_UP || k == KEY_DOWN) { if (n) M.focus = ((M.focus + (k == KEY_DOWN ? 1 : -1)) % n + n) % n; return 1; }
    ent *e = row_ent(M.focus);
    if (!e) return 1;
    if (k == KEY_LEFT || k == KEY_RIGHT) {
        int dir = k == KEY_RIGHT ? 1 : -1;
        if (e->kind == E_TILE) { M.tile = CLAMP(M.tile + dir, 0, M.ntiles - 1); return 1; }
        if (e->sub && dir > 0) { activate(e, 0); return 1; }
        if (M.page != P_MAIN && dir < 0 && !e->value[0]) { M.page = P_MAIN; build(); M.focus = M.main_focus; return 1; }
        int cycles = e->id == A_AUDIO || e->id == A_ASPECT || e->id == A_SLEEP || e->id == A_VIEW || e->id == A_ADAPTIVE;
        if (cycles && ev != EV_REPEAT) activate(e, dir);
        return 1;
    }
    if (k == KEY_OK && ev == EV_SHORT) activate(e, 0);
    return 1;
}

static void draw(layer *l, gfx_t *g) {
    (void)l;
    static int built_lang;
    if (built_lang != lang_rev()) { built_lang = lang_rev(); build(); }   /* язык сменили, пока окно открыто */
    int w = E(35.6), x = SW - w;
    d_hgrad(g, 0, 0, x, SH, 0, BA(0.7));
    d_fill(g, x, 0, w, SH, C_POPUP);
    int px = x + E(1.5), pw = w - E(3), y = E(2.4);
    channel *c = M.c;
    d_logo(g, c, px, y, E(5.2), E(3.6), E(0.4), 0, 0);
    d_text(g, px + E(6.2), y - E(0.1), 1.4, F_B, C_WHITE, c->name, pw - E(6.2));
    int pi = repo_prog_at(c, c == T.cur ? tv_position() : clock_now());
    if (pi >= 0) d_text(g, px + E(6.2), y + E(2.0), 1.1, F_R, C_TEXT3, repo_prog(c, pi).title, pw - E(6.2));
    y += E(5);
    ent *fe = row_ent(M.focus);
    /* раскладка: если не помещается (крупный размер интерфейса) — прокрутка к пункту в фокусе */
    int th = E(5.4), ih = E(2.6), hh = E(2.6);
    int total = M.ntiles ? th + E(0.6) : 0, fy = 0, fh = th;
    for (int i = 0; i < M.n; i++) {
        ent *e = &M.e[i];
        if (e->kind == E_TILE) continue;
        if (e->kind == E_HEAD) { total += hh; continue; }
        if (e == fe) { fy = total; fh = ih; }
        total += ih + E(0.2);
    }
    int avail = SH - E(1.2) - y;
    static int scroll;
    if (M.focus == 0 || total <= avail) scroll = 0;
    else {
        if (fy - scroll < E(3)) scroll = fy - E(3);
        if (fy + fh - scroll > avail - E(1)) scroll = fy + fh - avail + E(1);
        if (scroll < 0) scroll = 0;
    }
    gfx_push_clip(g, x, y - E(0.3), w, avail + E(0.3));
    y -= scroll;
    /* плитки */
    if (M.ntiles) {
        int tw = (pw - (M.ntiles - 1) * E(0.6)) / M.ntiles;
        for (int i = 0; i < M.ntiles; i++) {
            ent *e = &M.e[i];
            int tx = px + i * (tw + E(0.6)), foc = M.focus == 0 && M.tile == i;
            d_round(g, tx, y, tw, th, E(0.4), foc ? C_WHITE : C_FILL_BTN);
            uint32_t fg = foc ? 0xff101010u : C_WHITE;
            d_icon(g, e->icon, tx + tw / 2, y + E(1.8), E(2.0), e->icon == I_HEART && repo_is_fav(c) && !foc ? C_LIVE : fg);
            d_text_c(g, tx + E(0.2), tw - E(0.4), y + E(3.3), 1.1, F_R, fg, e->label);
        }
        y += th + E(0.6);
    }
    for (int i = 0; i < M.n; i++) {
        ent *e = &M.e[i];
        if (e->kind == E_TILE) continue;
        if (e->kind == E_HEAD) {
            char up[160];
            text_upper(up, sizeof up, e->label);
            d_text(g, px + E(0.6), y + E(0.7), 1.1, F_S, WA(0.5), up, pw);
            y += hh;
            continue;
        }
        int foc = fe == e;
        if (foc) d_round(g, px, y, pw, ih, E(0.4), C_WHITE);
        uint32_t fg = foc ? 0xff101010u : C_WHITE;
        int cy = y + ih / 2;
        int vw = e->value[0] ? d_text_w(1.2, F_R, e->value) : 0;
        if (vw > pw / 2) vw = pw / 2;
        d_text_mid(g, px + E(0.8), cy, 1.3, F_R, fg, e->label, pw - E(2.4) - vw - (e->sub ? E(1.6) : 0));
        int rx = px + pw - E(0.8);
        if (e->sub) { d_icon(g, I_CHEVRON, rx - E(0.4), cy, E(1.0), foc ? fg : C_TEXT3); rx -= E(1.6); }
        if (e->value[0]) d_text_mid(g, rx - vw, cy, 1.2, F_R, foc ? 0xff404040u : C_TEXT2, e->value, vw);
        y += ih + E(0.2);
    }
    gfx_pop_clip(g);
    d_fade_edges(g, x, SH - E(1.2) - avail, w, avail, scroll > 0, total - scroll > avail, C_POPUP | 0xff000000u);
}

void menu_open(channel *c, const char *group, int from_guide) {
    L.name = "menu";
    L.key = key;
    L.draw = draw;
    memset(&M, 0, sizeof M);
    M.c = c;
    snprintf(M.group, sizeof M.group, "%s", group ? group : "");
    M.from_guide = from_guide;
    M.page = P_MAIN;
    build();
    layer_push(&L);
}
