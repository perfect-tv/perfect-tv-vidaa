#define _GNU_SOURCE
#include "tv.h"
#include "app.h"
#include "keys.h"
#include "layer.h"
#include "screens.h"
#include "u.h"
#include "core/relay.h"
#include "core/ttx.h"
#include "core/clock.h"
#include "core/lang.h"
#include "core/prefs.h"
#include "data/catchup.h"
#include "data/loaders.h"
#include "player/player.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

tv_t T;

#define ZAP_DELAY 0.7
#define SEEK_APPLY 0.9
#define CAROUSEL_SECONDS 1.5
#define DIGIT_SECONDS 2.0

static void preview_commit(void);
static int preview_active;
static channel *preview_hold;

static char toast_msg[300];
static double toast_until;
static int pin_session;          /* «один раз до выхода»: PIN уже вводили */
static double reminders_at, seek_last_rep;

void tv_toast(const char *m) {
    snprintf(toast_msg, sizeof toast_msg, "%s", m);
    toast_until = app_now() + 3.5;
    app_dirty();
}

static int info_visible(void) { return T.info_pinned || app_now() < T.info_until || T.seeking || T.dig[0]; }
void tv_show_info(int pin) {
    T.info_until = app_now() + P.info_hide;
    if (pin) T.info_pinned = 1;
    app_dirty();
}

int64_t tv_position(void) {
    int64_t now = clock_now();
    if (!T.arc) return T.paused ? T.pause_at : now;
    /* PL_ENDED: картинки уже нет, но плеер ещё жив — позиция в ссылке верна. Иначе конец
     * shift-ссылки брал старый arc_target и открывал тот же участок заново. */
    double pos = pl_position();
    int at_end = pl_state() == PL_ENDED && pos > 0;
    int64_t p = (pl_has_picture() || at_end) ? T.arc_url_start + (int64_t)pos : T.arc_target;
    return p > now ? now : p;
}
int64_t tv_behind(void) { return clock_now() - tv_position(); }

/* ---------- открытие потока ---------- */
/* Каналы, которые через ретранслятор не открылись: ключи через \n в хранилище. */
#define DIRECT_KEY "relay.direct"
static int is_direct(const channel *c) {
    strlist l = {0};
    sl_from_lines(&l, store_get(DIRECT_KEY));
    int r = sl_find(&l, c->key) >= 0;
    sl_clear(&l);
    return r;
}
static void set_direct(const channel *c) {
    strlist l = {0};
    sl_from_lines(&l, store_get(DIRECT_KEY));
    if (sl_find(&l, c->key) < 0) sl_add(&l, c->key);
    char *s = sl_join(&l, "\n");
    store_set(DIRECT_KEY, s);
    free(s);
    sl_clear(&l);
}
int tv_direct_count(void) {
    strlist l = {0};
    sl_from_lines(&l, store_get(DIRECT_KEY));
    int n = l.n;
    sl_clear(&l);
    return n;
}
void tv_direct_reset(void) { store_set(DIRECT_KEY, NULL); T.direct = 0; }

/* Через ретранслятор (дорожки телетекста и субтитров, которые системный плеер отбрасывает): по настройке «Поток
 * через приложение» — все каналы HTTP / HLS, UDP / RTP и RTSP, кроме тех, что так не открылись; телетекст /
 * субтитры — всегда. RTSP в архиве — напрямую: плеер перематывает внутри ссылки, а поток от ретранслятора — без
 * перемотки. */
static void open_url(const char *url, const char *hdr, double start) {
    T.relayed = 0;
    int rtsp_arc = T.arc && starts_with_ci(url, "rtsp://");
    if (rtsp_arc && T.tap) { T.tap = T.subs = 0; tv_toast(tr(S_TTX_NOT_HTTP)); }
    if (!rtsp_arc && (T.tap || (P.relay_all && !T.direct))) {
        static char r[16384];
        if (!relay_url(url, hdr, r, sizeof r)) {
            relay_reset();
            subs_stream_changed();
            T.relayed = 1;
            fprintf(stderr, "stream via relay\n");
            pl_open(r, NULL, start, T.arc);
            return;
        }
        if (T.tap) { T.tap = T.subs = 0; tv_toast(tr(S_TTX_NOT_HTTP)); }
    }
    pl_open(url, hdr, start, T.arc);
}

static void start_stream(void);
/* через ретранслятор не открылось — сразу напрямую (попыткой не считается) */
static void relay_fallback(void) {
    fprintf(stderr, "relay_fallback explicit=%d\n", T.tap);
    if (T.tap) { T.tap = T.subs = 0; tv_toast(tr(S_TTX_RELAY_FAILED)); }
    T.direct = 1;
    T.direct_pending = !T.tap;   /* в список — только если напрямую заиграет (иначе канал просто недоступен) */
    if (T.arc) T.arc_target = tv_position();
    start_stream();
}

static void start_stream(void) {
    channel *c = T.cur;
    if (!c) return;
    char hdr[1024];
    repo_headers(c, hdr, sizeof hdr);
    T.failed = 0;
    T.why[0] = 0;
    T.opening_since = app_now();
    T.playing_since = 0;
    if (!T.arc) { open_url(c->url, hdr, 0); return; }
    int64_t now = clock_now(), t = T.arc_target;
    char tpl[4096], url[4096];
    if (catchup_template(c->url, c->cu_type, c->cu_source, tpl, sizeof tpl)) { tv_go_live(); return; }
    T.arc_range = strstr(tpl, "${duration}") || strstr(tpl, "${end}") || strstr(tpl, "${utcend}") || strstr(tpl, "${(d)") || strstr(tpl, "${(e)");
    double start = 0;
    if (T.arc_range) {
        /* окно передачи: перемотка внутри ссылки, конец — не позже «сейчас» */
        int i = repo_prog_at(c, t);
        int64_t ps = t, pe = t + 3600;
        if (i >= 0) { prog p = repo_prog(c, i); ps = p.start; pe = p.stop; }
        if (pe > now) pe = now;
        if (pe - ps < 60) pe = ps + 60;
        T.arc_url_start = ps;
        T.arc_url_end = pe;
        catchup_prepare(tpl, ps, pe - ps, now, clock_tz, url, sizeof url);
        start = (double)(t - ps);
    } else {
        T.arc_url_start = t;
        T.arc_url_end = now;
        catchup_prepare(tpl, t, now - t > 60 ? now - t : 60, now, clock_tz, url, sizeof url);
    }
    fprintf(stderr, "archive start=%lld at=%lld range=%d\n", (long long)T.arc_url_start, (long long)t, T.arc_range);
    open_url(url, hdr, start);
}

static void request_open(double delay) {
    T.open_at = app_now() + delay;
    T.attempts = 0;
    T.failed = 0;
    T.paused = 0;
    app_dirty();
}

static void remember(void) {
    if (!T.cur) return;
    snprintf(P.last_channel, sizeof P.last_channel, "%s", T.cur->key);
    snprintf(P.last_group, sizeof P.last_group, "%s", T.group);
    prefs_save();
}

static void really_play(channel *c, const char *gkey, int arc, int64_t t, double delay) {
    if (!c) return;
    if (preview_active && c != T.cur) preview_commit();
    if (c != T.cur) {
        T.scored = 0;
        T.audio_pref_done = 0;
        repo_add_history(c);
        /* субтитры включали на этом канале — поток сразу через ретранслятор */
        T.cur = c;
        subs_load();   /* субтитры, выбранные на этом канале, — поток сразу через ретранслятор */
        T.tap = T.subs;
        T.direct = is_direct(c);
        T.direct_pending = 0;
    }
    T.cur = c;
    group *g = repo_group_of(c, gkey ? gkey : T.group[0] ? T.group : NULL);
    snprintf(T.group, sizeof T.group, "%s", g ? g->key : ALL_KEY);
    T.arc = arc;
    T.arc_target = t;
    T.seeking = 0;
    T.dig[0] = 0;
    if (!repo_is_locked(c)) { T.pin_passed = 0; snprintf(T.last_unlocked, sizeof T.last_unlocked, "%s", c->key); }
    tv_show_info(0);
    request_open(delay);
    remember();
}

typedef struct { channel *c; char group[160]; int arc; int64_t t; int dir; } pin_ctx;
static pin_ctx pc;
static void pin_ok(void *x) {
    pin_ctx *p = x;
    T.pin_passed = 1;
    pin_session = 1;
    really_play(p->c, p->group, p->arc, p->t, 0.1);
}
static void pin_cancel(void *x) {
    pin_ctx *p = x;
    if (!p->dir) return;
    /* листание: следующий в ту же сторону без PIN */
    group *g = repo_group(p->group);
    int n = g ? g->n : 0, i = repo_index_in(g, p->c);
    for (int k = 1; k < n && i >= 0; k++) {
        channel *c = g->ch[((i + p->dir * k) % n + n) % n];
        if (!repo_is_locked(c)) { really_play(c, p->group, 0, 0, ZAP_DELAY); return; }
    }
}
static int need_pin(const channel *c) {
    if (!repo_is_locked(c)) return 0;
    if (T.pin_passed && T.cur && repo_is_locked(T.cur)) return 0;   /* из закрытого, открытого по PIN, — в другой закрытый */
    if (P.pin_ask == PIN_ONCE && pin_session) return 0;
    return 1;
}
static void play_checked(channel *c, const char *gkey, int arc, int64_t t, int dir, double delay) {
    if (!c) return;
    if (need_pin(c)) {
        pc.c = c;
        snprintf(pc.group, sizeof pc.group, "%s", gkey ? gkey : T.group);
        pc.arc = arc;
        pc.t = t;
        pc.dir = dir;
        pin_ask(pin_ok, pin_cancel, &pc);
        return;
    }
    really_play(c, gkey, arc, t, delay);
}

void tv_play(channel *c, const char *gkey) {
    if (c == T.cur && !T.arc && !T.failed && pl_state() != PL_IDLE) { if (gkey) snprintf(T.group, sizeof T.group, "%s", gkey); tv_show_info(0); return; }
    play_checked(c, gkey, 0, 0, 0, 0.05);
}

void tv_play_archive(channel *c, int64_t t, const char *gkey) {
    if (!c) return;
    int64_t now = clock_now();
    if (!repo_has_archive(c)) { tv_toast(tr(S_NO_ARCHIVE)); return; }
    if (t >= now - 15) { if (c == T.cur && !T.arc) return; play_checked(c, gkey, 0, 0, 0, 0.05); return; }
    if (t < now - (int64_t)c->archive_days * 86400 - 3600) { tv_toast(tr(S_ARCHIVE_TOO_OLD)); return; }
    play_checked(c, gkey, 1, t, 0, 0.05);
}

void tv_go_live(void) {
    if (!T.cur) return;
    T.arc = 0;
    T.seeking = 0;
    T.paused = 0;
    tv_show_info(0);
    request_open(0.05);
}

channel *tv_neighbour(int dir) {
    group *g = repo_group(T.group);
    if (!g || !g->n) g = R.ngroups ? &R.groups[0] : NULL;
    if (!g || !g->n) return NULL;
    int i = repo_index_in(g, T.cur);
    if (i < 0) return g->ch[0];
    return g->ch[((i + dir) % g->n + g->n) % g->n];
}

static void zap(int dir) {
    channel *c = tv_neighbour(dir);
    if (!c) return;
    T.carousel_until = app_now() + CAROUSEL_SECONDS;
    play_checked(c, T.group, 0, 0, dir, ZAP_DELAY);
}

/* ---------- перемотка ---------- */
static void seek_by(int dir, int repeat) {
    channel *c = T.cur;
    if (!c || !repo_has_archive(c)) return;
    double now = app_now();
    int64_t t = clock_now();
    if (!T.seeking) { T.seeking = 1; T.seek_to = tv_position(); T.seek_started = now; seek_last_rep = now; }
    double step = 30;
    if (repeat) {
        double held = now - T.seek_started, dt = now - seek_last_rep;
        if (dt > 0.25) dt = 0.25;
        double speed = 30 * pow(20, (held > 4 ? 4 : held) / 4);   /* от 30 с до 10 мин эфира в секунду */
        step = speed * dt;
        if (step < 5) step = 5;
    }
    seek_last_rep = now;
    T.seek_to += (int64_t)(dir * step);
    int64_t lo = t - (int64_t)c->archive_days * 86400;
    if (T.seek_to < lo) T.seek_to = lo;
    if (T.seek_to > t) T.seek_to = t;
    T.seek_apply_at = now + SEEK_APPLY;
    tv_show_info(0);
}

static void seek_apply(void) {
    if (!T.seeking) return;
    T.seeking = 0;
    int64_t t = T.seek_to, now = clock_now();
    if (t >= now - 15) { if (T.arc || T.paused) tv_go_live(); return; }
    if (T.arc && T.arc_range && pl_has_picture() && t >= T.arc_url_start && t < T.arc_url_end - 5 && !pl_seek((double)(t - T.arc_url_start))) {
        T.arc_target = t;
        tv_show_info(0);
        return;
    }
    tv_play_archive(T.cur, t, T.group);
}

/* ---------- пауза ---------- */
static void toggle_pause(void) {
    if (!T.cur || !pl_has_picture()) return;
    if (!T.paused) {
        T.pause_at = tv_position();
        if (pl_pause(1) == 0 || !T.arc) T.paused = 1;
        tv_show_info(0);
        return;
    }
    T.paused = 0;
    if (!T.arc && repo_has_archive(T.cur) && clock_now() - T.pause_at > 3) {
        tv_play_archive(T.cur, T.pause_at, T.group);   /* эфир после паузы — со сдвигом */
        return;
    }
    if (!T.arc) { request_open(0.05); return; }       /* без архива — снова эфир */
    pl_pause(0);
    tv_show_info(0);
}

/* ---------- набор номера ---------- */
static int max_num(void) {
    int m = 0;
    for (int i = 0; i < R.nch; i++) if (R.ch[i].num > m) m = R.ch[i].num;
    return m;
}
static void digits_apply(void) {
    if (!T.dig[0]) return;
    int n = atoi(T.dig);
    T.dig[0] = 0;
    channel *c = repo_by_num(n);
    if (!c) { tv_toast(tr(S_NO_SUCH_NUMBER)); return; }
    play_checked(c, NULL, 0, 0, 0, 0.05);
}
static void digit(int d) {
    size_t n = strlen(T.dig);
    if (n >= 5) return;
    T.dig[n] = (char)('0' + d);
    T.dig[n + 1] = 0;
    T.dig_at = app_now();
    /* следующая цифра невозможна — сразу */
    if ((long)atoi(T.dig) * 10 > max_num()) digits_apply();
    app_dirty();
}

/* ---------- клавиши ---------- */
int tv_has_long(int k) { return k == KEY_OK || k == KEY_UP || k == KEY_DOWN; }

static void back(void) {
    double now = app_now();
    if (T.dig[0]) { T.dig[0] = 0; return; }
    if (T.seeking) { T.seeking = 0; return; }
    if (T.info_expanded) { T.info_expanded = 0; return; }
    if (T.info_pinned) { T.info_pinned = 0; T.info_until = 0; return; }
    int locked = T.cur && repo_is_locked(T.cur);
    if (now - T.back_at < 2.0) {
        T.back_at = 0;
        toast_msg[0] = 0;
        if (locked) {
            channel *c = repo_by_key(T.last_unlocked);
            if (!c || repo_is_locked(c)) {
                c = NULL;
                for (int i = 0; i < R.history.n && !c; i++) { channel *h = repo_by_key(R.history.v[i]); if (h && !repo_is_locked(h)) c = h; }
            }
            T.pin_passed = 0;
            pin_session = 0;
            if (c) { really_play(c, NULL, 0, 0, 0.05); return; }
        }
        fprintf(stderr, "exit_key=back_twice\n");
        app_quit();
        return;
    }
    T.back_at = now;
    tv_toast(tr(locked ? S_PRESS_BACK_UNLOCKED : S_PRESS_BACK_AGAIN));
}

int tv_key(int k, int ev) {
    if (!R.nch) {   /* нет каналов: загрузка или ошибка плейлиста */
        if (k == KEY_BACK && ev == EV_SHORT) back();
        else if (k == KEY_OK && ev == EV_SHORT && R.cur >= 0) load_playlist(R.cur, 1);
        else if (k == KEY_MENU || (k == KEY_OK && ev == EV_LONG)) settings_open();
        return 1;
    }
    if (IS_DIGIT(k)) { if (ev == EV_SHORT) digit(k - KEY_D0); return 1; }
    switch (k) {
    case KEY_OK:
        if (ev == EV_LONG) { tv_open_menu(T.cur, T.group, 0); return 1; }
        if (T.dig[0]) digits_apply();
        else if (T.seeking) { T.seek_apply_at = 0; seek_apply(); }
        else if (T.failed) { T.attempts = 0; T.open_at = app_now(); }
        else if (info_visible() && pl_has_picture()) toggle_pause();
        else tv_open_guide();
        return 1;
    case KEY_MENU: tv_open_menu(T.cur, T.group, 0); return 1;
    case KEY_UP:
        if (ev == EV_LONG) { tv_open_schedule(T.cur, 1); return 1; }
        if (ev != EV_SHORT) return 1;
        if (T.info_expanded) T.info_expanded = 0;
        else if (info_visible()) { T.info_pinned = 0; T.info_until = 0; }
        else if (P.up_key == UP_RECENT) tv_open_recent();
        else tv_open_schedule(T.cur, 0);
        return 1;
    case KEY_DOWN:
        if (ev == EV_LONG) { tv_open_recent(); return 1; }
        if (ev != EV_SHORT) return 1;
        if (!info_visible()) tv_show_info(1);
        else if (!T.info_pinned) T.info_pinned = 1;   /* ↓ ↓: закрепить, затем раскрыть описание */
        else T.info_expanded = 1;
        return 1;
    case KEY_LEFT: case KEY_RIGHT: {
        int dir = k == KEY_RIGHT ? 1 : -1;
        if (T.arc || (T.paused && repo_has_archive(T.cur))) seek_by(dir, ev == EV_REPEAT);
        else zap(dir);
        return 1;
    }
    case KEY_CH_UP: zap(1); return 1;
    case KEY_CH_DOWN: zap(-1); return 1;
    case KEY_REW: case KEY_FF: seek_by(k == KEY_FF ? 1 : -1, ev == EV_REPEAT); return 1;
    case KEY_PLAYPAUSE: toggle_pause(); return 1;
    case KEY_PLAY: if (T.paused) toggle_pause(); return 1;
    case KEY_PAUSE: if (!T.paused) toggle_pause(); return 1;
    case KEY_STOP: if (T.arc) tv_go_live(); else back(); return 1;
    case KEY_BACK: if (ev == EV_SHORT) back(); return 1;
    case KEY_GUIDE: case KEY_GREEN: tv_open_guide(); return 1;
    case KEY_SEARCH: search_open(); return 1;
    case KEY_TEXT: tv_teletext(); return 1;
    case KEY_SUBTITLE: subs_toggle(); return 1;
    case KEY_INFO: case KEY_BLUE: if (info_visible() && T.info_pinned) T.info_expanded = !T.info_expanded; else tv_show_info(1); return 1;
    case KEY_LIST: tv_open_recent(); return 1;
    case KEY_RED: tv_open_schedule(T.cur, 0); return 1;
    case KEY_YELLOW: case KEY_FAV:
        if (T.cur) { int was = repo_is_fav(T.cur); repo_toggle_fav(T.cur); tv_toast(tr(was ? S_REMOVED_FAV : S_ADDED_FAV)); }
        return 1;
    default: return 0;
    }
}

/* ---------- окна ---------- */
void tv_open_guide(void) { guide_open(T.arc); }
void tv_open_schedule(channel *c, int reminders) { if (c || reminders) schedule_open(c ? c : T.cur, reminders); }
void tv_open_menu(channel *c, const char *group, int from_guide) { if (c) menu_open(c, group, from_guide); }
void tv_open_recent(void) { recent_open(); }
void tv_open_settings(void) { settings_open(); }

void tv_set_sleep(int minutes) {
    T.sleep_until = minutes > 0 ? app_now() + minutes * 60.0 : 0;
    T.sleep_warned = 0;
}
int tv_sleep_minutes(void) {
    if (!T.sleep_until) return 0;
    int m = (int)((T.sleep_until - app_now()) / 60 + 0.99);
    return m > 0 ? m : 0;
}

/* ---------- телетекст и субтитры ---------- */
/* телетекст: через ретранслятор — по его разбору; напрямую — по дорожкам системного плеера */
int tv_has_teletext(void) {
    if (T.relayed && relay_ts_bytes() > 0) return relay_has_teletext();
    for (int i = 0; i < pl_sub_count(); i++) if (!strcmp(pl_sub_codec(i), "dvb_teletext")) return 1;
    return 0;
}
int tv_has_subtitles(void) { return subs_available() > 0; }
static void reopen_with_tap(void) {
    if (T.tap || !T.cur) return;
    T.tap = 1;
    if (T.relayed) return;   /* поток уже идёт через ретранслятор — телетекст есть сразу */
    if (T.arc) T.arc_target = tv_position();
    request_open(0.05);
}
void tv_teletext(void) {
    if (!T.cur) return;
    /* поток уже разобран, а дорожки нет — так и сказать (PMT приходит в первые доли секунды) */
    if (T.relayed && !relay_has_teletext() && relay_ts_bytes() > 512 * 1024) { tv_toast(tr(relay_has_dvbsub() ? S_TTX_DVB_ONLY : S_TTX_NONE)); return; }
    reopen_with_tap();
    teletext_open();
}
void tv_set_subtitles(int on) {
    if (!T.cur) return;
    if (on != T.subs) subs_toggle();
}
void tv_subtitles_need_relay(void) { if (T.subs) reopen_with_tap(); }

/* время кадра в метках потока: первая метка, что видел ретранслятор, + позиция плеера */
int64_t tv_frame_pts(void) {
    int64_t base = relay_base_pts();
    return base < 0 ? -1 : base + (int64_t)(pl_position() * 90000.0);
}

/* ---------- плейлист ---------- */
/* ---------- предпросмотр плейлиста без остановки канала ---------- */
static int preview_from = -1, preview_last;

static channel *chan_copy(const channel *s) {
    if (!s) return NULL;
    channel *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    *d = *s;
#define CP(f) do { if (s->f) { d->f = strdup(s->f); if (!d->f) { free(d->key); free(d->name); free(d->url); free(d->group); free(d->logo); \
    free(d->tvg_id); free(d->tvg_name); free(d->mkey); free(d->tkey); free(d->cu_type); free(d->cu_source); free(d->ua); free(d->ref); \
    free(d->headers_json); free(d); return NULL; } } } while (0)
    CP(key); CP(name); CP(url); CP(group); CP(logo); CP(tvg_id); CP(tvg_name); CP(mkey); CP(tkey);
    CP(cu_type); CP(cu_source); CP(ua); CP(ref); CP(headers_json);
#undef CP
    d->epg_src = d->epg_ch = -1;   /* индексы старого плейлиста к новому не относятся */
    return d;
}
static void chan_free(channel *c) {
    if (!c) return;
    free(c->key); free(c->name); free(c->url); free(c->group); free(c->logo); free(c->tvg_id); free(c->tvg_name);
    free(c->mkey); free(c->tkey); free(c->cu_type); free(c->cu_source); free(c->ua); free(c->ref); free(c->headers_json);
    free(c);
}

int tv_is_preview(void) { return preview_active; }

int tv_preview_playlist(int index) {
    if (index < 0 || index >= R.npl || index == R.cur) return 0;
    if (!preview_active) {
        preview_from = R.cur;
        preview_last = P.last_playlist;
        if (T.cur) {
            preview_hold = chan_copy(T.cur);
            T.cur = preview_hold;   /* NULL при нехватке памяти: видео продолжает играть, инфопанель скрыта */
        }
        preview_active = 1;
    }
    repo_select(index);
    P.last_playlist = preview_last;   /* выбранный канал нового плейлиста запишет его сам */
    app_dirty();
    return 1;
}

void tv_preview_back(void) {
    if (!preview_active) return;
    int from = preview_from, last = preview_last;
    char key[160] = "";
    if (preview_hold) snprintf(key, sizeof key, "%s", preview_hold->key);
    else if (T.cur) snprintf(key, sizeof key, "%s", T.cur->key);
    preview_active = 0;
    T.cur = NULL;
    repo_select(from);
    P.last_playlist = last;
    channel *c = key[0] ? repo_by_key(key) : NULL;
    chan_free(preview_hold);
    preview_hold = NULL;
    T.cur = c;
    if (c) {
        group *g = repo_group_of(c, NULL);
        snprintf(T.group, sizeof T.group, "%s", g ? g->key : ALL_KEY);
    } else T.group[0] = 0;
    app_dirty();
}

static void preview_commit(void) {
    if (!preview_active) return;
    preview_active = 0;
    chan_free(preview_hold);
    preview_hold = NULL;
    playlist *p = repo_cur();
    P.last_playlist = p ? p->id : 0;
}

static int keep_layer(const layer *l) {
    return l->name && (!strcmp(l->name, "settings") || !strcmp(l->name, "form") || !strcmp(l->name, "choose") || !strcmp(l->name, "confirm"));
}
void tv_playlist_changed(int keep) {
    if (preview_active) { preview_active = 0; chan_free(preview_hold); preview_hold = NULL; T.cur = NULL; }
    char key[160], grp[160];
    snprintf(key, sizeof key, "%s", T.cur && keep ? T.cur->key : P.last_channel);
    snprintf(grp, sizeof grp, "%s", T.group[0] ? T.group : P.last_group);
    /* слои с указателями на каналы старого списка — закрыть */
    for (int i = layer_count() - 1; i >= 0; i--) {
        layer *l = layer_at(i);
        if (l && !keep_layer(l)) { layer_pop(l); i = layer_count(); }
    }
    T.cur = NULL;
    repo_select(R.cur);
    channel *c = key[0] ? repo_by_key(key) : NULL;
    if (!R.nch) { pl_close(); app_dirty(); return; }
    if (c && keep) {
        /* тот же канал, видео не перезапускаем */
        T.cur = c;
        group *g = repo_group_of(c, grp);
        snprintf(T.group, sizeof T.group, "%s", g ? g->key : ALL_KEY);
        if (pl_state() == PL_IDLE) request_open(0.05);
        app_dirty();
        return;
    }
    if (c && P.start_last) { really_play(c, grp, 0, 0, 0.05); return; }
    T.group[0] = 0;
    channel *first = R.ngroups && R.groups[0].n ? R.groups[0].ch[0] : &R.ch[0];
    if (!repo_is_locked(first)) really_play(first, NULL, 0, 0, 0.05);
    else T.cur = first;
    if (!c || !P.start_last) guide_open(0);
}

static void on_playlist(int idx, int ok, const char *err) {
    if (idx != R.cur) return;
    if (ok) {
        int had = R.nch > 0;
        tv_playlist_changed(had);
        playlist *p = repo_cur();
        if (had && p) tv_toast(trf(S_PLAYLIST_UPDATED, trp(S_CHANNELS_N, p->count)));
        load_epg(0);
    } else if (R.nch) tv_toast(loaders_error_text(err));
    app_dirty();
}
static void on_epg(int ok, const char *err) {
    if (!ok && err && *err && strcmp(err, "cancelled")) tv_toast(trf(S_EPG_FAILED, loaders_error_text(err)));
    app_dirty();
}

static void begin(void);
void tv_start(void) {
    u_init();
    loaders_on_playlist = on_playlist;
    loaders_on_epg = on_epg;
    begin();
}

/* Восстановили копию: настройки, плейлисты и данные — заново, как при запуске. */
void tv_restart(void) {
    loaders_cancel();
    layer_pop_all();
    pl_close();
    if (preview_active) { preview_active = 0; chan_free(preview_hold); preview_hold = NULL; }
    T.cur = NULL;
    T.group[0] = 0;
    T.arc = T.paused = T.seeking = 0;
    T.pin_passed = 0;
    T.audio_pref_done = 0;
    pin_session = 0;                   /* «один раз до выхода» не переживает смену базы настроек */
    T.info_pinned = T.info_expanded = 0;
    T.dig[0] = 0;
    T.failed = 0;
    prefs_load();
    lang_set(P.lang);
    u_init();
    repo_reload();
    app_dirty();
    begin();
}

static void begin(void) {
    if (!R.npl) { welcome_open(); return; }
    if (!R.nch) { if (R.cur < 0) repo_select(0); load_playlist(R.cur, 1); return; }
    channel *c = P.last_channel[0] ? repo_by_key(P.last_channel) : NULL;
    if (c && P.start_last) play_checked(c, P.last_group, 0, 0, 0, 0.01);
    else {
        channel *first = R.ngroups && R.groups[0].n ? R.groups[0].ch[0] : &R.ch[0];
        T.cur = c ? c : first;
        snprintf(T.group, sizeof T.group, "%s", P.last_group);
        if (!repo_is_locked(T.cur)) really_play(T.cur, P.last_group, 0, 0, 0.01);
        guide_open(0);
    }
}

/* ---------- такт ---------- */
void tv_tick(double now) {
    keys_tick(now);
    pl_tick();
    loaders_tick();
    layer_tick_all(now);
    if (T.open_at && now >= T.open_at) { T.open_at = 0; start_stream(); }
    if (T.dig[0] && now - T.dig_at > DIGIT_SECONDS) digits_apply();
    subs_apply();
    if (T.subs && T.relayed) {   /* субтитры меняются по времени кадра */
        static double next;
        if (now >= next) { next = now + 0.25; app_dirty(); }
    }
    if (T.seeking && T.seek_apply_at && now >= T.seek_apply_at) seek_apply();
    int st = pl_state();
    if (T.cur && !T.open_at) {
        if (st == PL_PLAYING) {
            if (!T.playing_since) {
                T.playing_since = now;
                if (T.direct_pending && !T.relayed && T.cur) { set_direct(T.cur); fprintf(stderr, "relay_direct_saved\n"); }
                T.direct_pending = 0;
                app_dirty();
            }
            if (!T.audio_pref_done) {
                int na = pl_audio_count();
                if (!P.audio_pref[0]) T.audio_pref_done = 1;
                else if (na > 0) {
                    T.audio_pref_done = 1;
                    for (int i = 0; i < na; i++)
                        if (!strcmp(pl_audio_label(i), P.audio_pref)) { pl_audio_select(i); break; }
                }
            }
            if (now - T.playing_since > 30) T.attempts = 0;
            if (!T.scored && now - T.playing_since > 300) { T.scored = 1; repo_add_score(T.cur); }
        }
        if (st == PL_ENDED && T.arc) {
            /* архив дошёл до конца ссылки: дальше — следующая передача или эфир */
            int64_t t = T.arc_range ? T.arc_url_end : tv_position();
            if (t >= clock_now() - 30) tv_go_live();
            else { T.arc_target = t; request_open(0.05); }
        } else if (st == PL_FAILED && !T.failed && T.relayed && !T.playing_since) {
            relay_fallback();
        } else if ((st == PL_FAILED || st == PL_ENDED) && !T.failed) {
            static const double delays[] = {2, 5, 10};
            if (!T.retry_at) {
                if (T.attempts < 3) { T.retry_at = now + delays[T.attempts]; T.attempts++; }
                else { T.failed = 1; snprintf(T.why, sizeof T.why, "%s", pl_why()); app_dirty(); }
            } else if (now >= T.retry_at) {
                T.retry_at = 0;
                if (T.arc) T.arc_target = tv_position();
                start_stream();
            }
        } else T.retry_at = 0;
    }
    /* анимации и часы */
    static double next_sec;
    if (now >= next_sec) { next_sec = now + 1; app_dirty(); }
    if (st == PL_OPENING || pl_rebuffering() || (toast_msg[0] && now > toast_until)) app_dirty();
    if (toast_msg[0] && now > toast_until) toast_msg[0] = 0;
    if (T.info_until && now > T.info_until) { T.info_until = 0; app_dirty(); }
    if (T.carousel_until && now > T.carousel_until) { T.carousel_until = 0; app_dirty(); }
    /* таймер сна */
    if (T.sleep_until) {
        if (!T.sleep_warned && now > T.sleep_until - 60) { T.sleep_warned = 1; tv_toast(tr(S_SLEEP_SOON)); }
        if (now > T.sleep_until) { fprintf(stderr, "exit_reason=sleep_timer\n"); app_quit(); }
    }
    if (now >= reminders_at) { reminders_at = now + 5; reminders_check(); }
}

/* ---------- рисование ---------- */
static void draw_carousel(gfx_t *g) {
    group *gr = repo_group(T.group);
    if (!gr || !gr->n || !T.cur) return;
    int cur = repo_index_in(gr, T.cur);
    if (cur < 0) return;
    int cw = E(9.6), ch = E(6.6), gap = E(1.2), lab = E(2.2);
    int y = info_top() - ch - lab - E(1.5);
    int count = SW / (cw + gap) + 2;
    if (count % 2 == 0) count++;
    int half = count / 2;
    int cx = SW / 2 - cw / 2;
    for (int k = -half; k <= half; k++) {
        if (gr->n == 1 && k) continue;
        channel *c = gr->ch[((cur + k) % gr->n + gr->n) % gr->n];
        int x = cx + k * (cw + gap);
        if (x + cw < 0 || x > SW) continue;
        d_round(g, x - E(0.3), y - E(0.3), cw + E(0.6), ch + lab + E(0.6), E(0.8), k ? C_PANEL : 0xf51a1b1cu);
        d_logo(g, c, x, y, cw, ch, E(0.6), 1, 0);
        d_text_c(g, x, cw, y + ch + E(0.4), 1.15, k ? F_R : F_B, k ? C_TEXT2 : C_WHITE, c->name);
        if (!k) d_stroke(g, x - E(0.5), y - E(0.5), cw + E(1.0), ch + lab + E(1.0), E(0.9), E(0.25), C_WHITE);
    }
}

static void draw_digits(gfx_t *g) {
    int x = E(4), y = E(2.4), w = E(30), h = E(8.4);
    d_round(g, x, y, w, h, E(1), C_PANEL);
    d_text(g, x + E(1.5), y + E(0.9), 2.6, F_B, C_WHITE, T.dig, 0);
    channel *c = repo_by_num(atoi(T.dig));
    if (c) {
        d_logo(g, c, x + E(1.5), y + E(4.6), E(4.8), E(3.3), E(0.4), 0, 0);
        d_text_mid(g, x + E(7.2), y + E(6.25), 1.35, F_S, C_WHITE, c->name, w - E(8.5));
    }
    /* подсказки: каналы, которые выберутся следующей цифрой */
    int base = atoi(T.dig), shown = 0, ly = y + h + E(0.6);
    for (int d = 0; d <= 9 && shown < 5; d++) {
        channel *n = repo_by_num(base * 10 + d);
        if (!n) continue;
        char b[260];
        snprintf(b, sizeof b, "%d  %s", n->num, n->name);
        d_round(g, x, ly, w, E(2.6), E(0.6), C_PANEL);
        d_text_mid(g, x + E(1.2), ly + E(1.3), 1.2, F_R, C_TEXT2, b, w - E(2.4));
        ly += E(2.9);
        shown++;
    }
}

static void draw_toast(gfx_t *g) {
    if (!toast_msg[0]) return;
    int tw = d_text_w(1.3, F_R, toast_msg) + E(3.2);
    if (tw > SW - E(16)) tw = SW - E(16);
    int x = (SW - tw) / 2, h = E(3.6), y = info_visible() && !layer_count() ? info_top() - E(5.6) - h - E(1) : SH - E(2.4) - h;
    if (y < E(2.4)) y = E(2.4);
    d_round(g, x, y, tw, h, E(0.6), C_PANEL);
    d_text_mid(g, x + E(1.6), y + h / 2, 1.3, F_R, C_WHITE, toast_msg, tw - E(3.2));
}

static const char *reason(const char *why) {
    if (!strncmp(why, "audio_codec=", 12)) return trf(S_ERR_ACODEC, why + 12);
    if (!strncmp(why, "video_codec=", 12)) return trf(S_ERR_VCODEC, why + 12);
    if (!strncmp(why, "open_input", 10) || !strncmp(why, "stream_info", 11) || !strncmp(why, "player_read_", 12)) return tr(S_ERR_CONNECT);
    if (!strcmp(why, "no_video")) return tr(S_ERR_NO_VIDEO);
    if (!strcmp(why, "dlopen") || !strcmp(why, "symbols") || !strncmp(why, "manager", 7) || !strcmp(why, "create_player") ||
        !strncmp(why, "player_native_", 14) || !strncmp(why, "player_feed_", 12)) return tr(S_ERR_PLAYER);
    if (!strcmp(why, "stream_ended")) return tr(S_ERR_ENDED);
    if (!strcmp(why, "archive_software")) return tr(S_ERR_ARCHIVE_SOFTWARE);
    return tr(S_ERR_GENERIC);
}

static void draw_no_channels(gfx_t *g) {
    d_clear(g, C_SETTINGS_BG);
    playlist *p = repo_cur();
    int cy = SH / 2 - E(4);
    if (loaders_playlist_busy()) {
        int n = 0;
        loaders_playlist_progress(&n);
        d_spinner(g, SW / 2, cy - E(3), E(2), app_now());
        d_text_c(g, 0, SW, cy + E(1), 1.6, F_S, C_WHITE, n ? trf(S_LOADING_PLAYLIST_N, trp(S_CHANNELS_N, n)) : tr(S_LOADING_PLAYLIST));
        if (p) d_text_c(g, 0, SW, cy + E(4), 1.2, F_R, C_TEXT3, p->name);
        app_dirty();
        return;
    }
    d_text_c(g, 0, SW, cy, 1.8, F_B, C_WHITE, tr(S_PLAYLIST_ERROR));
    if (p) d_text_c(g, 0, SW, cy + E(3.4), 1.3, F_R, C_LIVE, loaders_error_text(p->error[0] ? p->error : "empty"));
    if (p) d_text_c(g, 0, SW, cy + E(6), 1.15, F_R, C_TEXT3, p->name);
    hint h[] = {{-1, tr(S_K_OK), tr(S_RETRY)}, {-1, tr(S_K_MENU), tr(S_SETTINGS)}, {-1, tr(S_K_BACK), tr(S_EXIT)}};
    int w = d_hints_w(h, 3);
    d_hints(g, (SW - w) / 2, cy + E(10), h, 3);
}

void tv_draw(gfx_t *g) {
    double now = app_now();
    if (!R.nch && !layer_count()) { draw_no_channels(g); draw_toast(g); return; }
    int covered = 0;
    for (int i = 0; i < layer_count(); i++) if (layer_at(i)->covers) covered = 1;
    if (covered) d_clear(g, C_BLACK);
    else {
        if (pl_has_picture() && !T.failed) pl_draw_video(g);
        else {
            d_clear(g, C_BLACK);
            if (T.failed && T.cur) {
                d_text_c(g, 0, SW, SH / 2 - E(6), 1.9, F_B, C_WHITE, T.cur->name);
                d_text_c(g, 0, SW, SH / 2 - E(2.6), 1.4, F_R, C_LIVE, reason(T.why));
                d_text_c(g, 0, SW, SH / 2 + E(0.2), 1.1, F_R, C_TEXT3, trf(S_ERR_CODE, T.why));
            } else if (T.cur && (T.open_at || pl_state() == PL_OPENING || T.retry_at) && now - T.opening_since > 0.6) {
                d_spinner(g, SW / 2, SH / 2 - E(2), E(1.8), now);
                d_text_c(g, 0, SW, SH / 2 + E(1.2), 1.4, F_S, C_TEXT2, T.attempts ? tr(S_RECONNECTING) : tr(S_CONNECTING));
            }
        }
        if (pl_rebuffering()) {
            d_round(g, SW - E(4) - E(5), E(2.4), E(5), E(5), E(1), C_PANEL);
            d_spinner(g, SW - E(4) - E(2.5), E(4.9), E(1.4), now);
        }
        subs_draw(g, info_visible() ? info_top() - E(1.2) : SH - E(4));
        if (!layer_count() || (layer_top() && layer_top()->name && !strcmp(layer_top()->name, "menu"))) {
            if (info_visible()) info_draw(g);
            if (T.carousel_until && !layer_count()) draw_carousel(g);
            if (T.dig[0] && !layer_count()) draw_digits(g);
        }
    }
    layer_draw_all(g);
    draw_toast(g);
}
