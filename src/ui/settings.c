/* Настройки (docs/TZ.md, §11): слева разделы, справа пункты (название, подпись, значение «‹ … ›»).
 * В разделах: ↑/↓ — раздел, → / OK — к пунктам, «Назад» — выйти. В пунктах: ↑/↓ — пункт, ← / → — значение
 * по кругу (у пункта без значений ← — к разделам), OK — следующее значение или действие, «Назад» — к разделам. */
#include "screens.h"
#include "layer.h"
#include "keys.h"
#include "tv.h"
#include "u.h"
#include "app.h"
#include "core/clock.h"
#include "core/lang.h"
#include "core/prefs.h"
#include "data/epg.h"
#include "data/loaders.h"
#include "data/backup.h"
#include "data/tmdb.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef PTV_VERSION
#define PTV_VERSION "dev"
#endif

enum { SEC_PLAYLISTS, SEC_VIEW, SEC_NET, SEC_LOOK, SEC_LIST, SEC_LOCK, SEC_TMDB, SEC_DATA, SEC_ABOUT, NSEC };
static const int SEC_NAMES[NSEC] = {S_SEC_PLAYLISTS, S_SEC_VIEW, S_SEC_NET, S_SEC_LOOK, S_SEC_LIST, S_SEC_LOCK, S_SEC_TMDB, S_SEC_DATA, S_SEC_ABOUT};

enum {
    I_PL = 1000,   /* I_PL + индекс плейлиста */
    I_PL_ADD = 1, I_EPG_SRC, I_EPG_REFRESH,
    I_ARCHIVE_DAYS, I_INFO_HIDE, I_UPKEY, I_START_LAST, I_REMIND, I_ANYCERT, I_ASPECT_P, I_EPG_HOURS, I_PL_HOURS,
    I_LANG, I_TZ, I_SCALE, I_NUMBER, I_GUIDE_VIEW, I_GUIDE_HEADER, I_HINTS_P, I_LOGO_PRIO,
    I_ADAPTIVE_P, I_RESET_SCORES, I_HIDDEN_P,
    I_PIN_SET, I_PIN_CLEAR, I_PIN_ASK, I_PIN_INPUT,
    I_RELAY_ALL, I_RELAY_DIRECT,
    I_TMDB_KEY, I_TMDB_PHONE, I_TMDB_API, I_TMDB_IMG, I_TMDB_TEST, I_TMDB_REMOVE,
    I_BACKUP_SAVE, I_BACKUP_RESTORE, I_CLEAR_LOGOS, I_CLEAR_EPG, I_DATA_DIR,
    I_VERSION, I_SOURCE, I_LICENSE,
};
typedef struct { int id; char label[200], sub[300], value[200]; int choice; } item;

static struct { int sec, zone, focus, top; item it[64]; int n; } ST;
static layer L;

static int sel_of(const int *v, int n, int cur) { for (int i = 0; i < n; i++) if (v[i] == cur) return i; return 0; }
static const int ARCH[] = {1, 2, 3, 4, 5, 7, 10, 14};
static const int HIDE[] = {3, 5, 7, 10};
static const int REM[] = {0, 1, 2, 3, 4, 5, 10};
static const int EPGH[] = {6, 12, 24};
static const int PLH[] = {12, 24, 48};

static void add(int id, const char *label, const char *sub, const char *value, int choice) {
    if (ST.n >= 64) return;
    item *i = &ST.it[ST.n++];
    i->id = id;
    snprintf(i->label, sizeof i->label, "%s", label);
    snprintf(i->sub, sizeof i->sub, "%s", sub ? sub : "");
    snprintf(i->value, sizeof i->value, "%s", value ? value : "");
    i->choice = choice;
}

static const char *tz_label(void) {
    static char b[64];
    int s = P.tz == TZ_AUTO ? clock_tz(clock_now()) : P.tz * 900;
    char o[16];
    snprintf(o, sizeof o, "%c%d:%02d", s < 0 ? '-' : '+', (s < 0 ? -s : s) / 3600, (s < 0 ? -s : s) / 60 % 60);
    if (P.tz == TZ_AUTO) snprintf(b, sizeof b, tr(S_TZ_AUTO), o);
    else snprintf(b, sizeof b, "UTC%s", o);
    return b;
}

static void build(void) {
    ST.n = 0;
    char v[200], sub[300];
    switch (ST.sec) {
    case SEC_PLAYLISTS:
        for (int i = 0; i < R.npl; i++) {
            playlist *p = &R.pl[i];
            char when[40] = "";
            if (p->loaded_at) { char d[32], hm[8]; fmt_day(p->loaded_at, d, sizeof d); fmt_hm(p->loaded_at, hm); snprintf(when, sizeof when, "%s %s", d, hm); }
            if (p->error[0]) snprintf(sub, sizeof sub, "%s", loaders_error_text(p->error));
            else snprintf(sub, sizeof sub, tr(S_PLAYLIST_STATUS), trp(S_CHANNELS_N, p->count), when[0] ? when : tr(S_NOT_LOADED));
            add(I_PL + i, p->name, sub, i == R.cur ? tr(S_CURRENT) : "", 0);
        }
        add(I_PL_ADD, tr(S_ADD_PLAYLIST), NULL, NULL, 0);
        for (int i = 0; i < R.nepg; i++) {
            epg_src *s = &R.epg[i];
            char d[32] = "", hm[8] = "";
            if (s->loaded_at) { fmt_day(s->loaded_at, d, sizeof d); fmt_hm(s->loaded_at, hm); }
            char when[48];
            snprintf(when, sizeof when, "%s %s", d, hm);
            if (s->loading) snprintf(sub, sizeof sub, "%s", tr(S_EPG_STATUS_LOADING));
            else if (s->error[0]) snprintf(sub, sizeof sub, "%s", loaders_error_text(s->error));
            else if (epg_has_data(s)) snprintf(sub, sizeof sub, tr(S_EPG_STATUS_OK), trp(S_PROGRAMMES_N, s->programmes), when);
            else snprintf(sub, sizeof sub, "%s", tr(S_EPG_STATUS_NONE));
            add(I_EPG_SRC, s->url, sub, NULL, 0);
        }
        if (R.nepg) add(I_EPG_REFRESH, tr(S_REFRESH_EPG), NULL, NULL, 0);
        break;
    case SEC_VIEW:
        add(I_ARCHIVE_DAYS, tr(S_ARCHIVE_DEPTH), NULL, trp(S_DAYS, P.archive_days), 1);
        add(I_INFO_HIDE, tr(S_INFO_HIDE), NULL, trp(S_SECONDS_N, P.info_hide), 1);
        add(I_UPKEY, tr(S_UP_KEY), NULL, tr(P.up_key == UP_RECENT ? S_UP_RECENT : S_UP_SCHEDULE), 1);
        add(I_START_LAST, tr(S_START_LAST), NULL, tr(P.start_last ? S_ON : S_OFF), 1);
        if (P.remind_before) snprintf(v, sizeof v, tr(S_REMIND_MIN), P.remind_before); else snprintf(v, sizeof v, "%s", tr(S_REMIND_AT_START));
        add(I_REMIND, tr(S_REMIND_BEFORE), NULL, v, 1);
        { int a[] = {S_ASPECT_FIT, S_ASPECT_STRETCH, S_ASPECT_16_9, S_ASPECT_4_3}; add(I_ASPECT_P, tr(S_ASPECT), NULL, tr(a[P.aspect & 3]), 1); }
        add(I_EPG_HOURS, tr(S_EPG_EVERY), NULL, trf(S_HOURS_EVERY, P.epg_hours), 1);
        add(I_PL_HOURS, tr(S_PLAYLIST_EVERY), NULL, trf(S_HOURS_EVERY, P.playlist_hours), 1);
        break;
    case SEC_LOOK:
        add(I_LANG, tr(S_LANGUAGE), NULL, P.lang == LANG_EN ? "English" : P.lang == LANG_RU ? "Русский" : tr(S_LANG_AUTO), 1);
        add(I_TZ, tr(S_TIMEZONE), NULL, tz_label(), 1);
        add(I_SCALE, tr(S_UI_SIZE), NULL, tr(P.ui_scale == 1 ? S_SIZE_SMALL : P.ui_scale == 2 ? S_SIZE_LARGE : S_SIZE_NORMAL), 1);
        add(I_NUMBER, tr(S_NUMBER_MODE), NULL, tr(P.number_mode == NUM_NONE ? S_NUM_NONE : P.number_mode == NUM_NEAR_NAME ? S_NUM_NEAR : S_NUM_BADGE), 1);
        add(I_GUIDE_VIEW, tr(S_GUIDE_VIEW), NULL, tr(P.guide_mode == GUIDE_LIST ? S_GUIDE_LIST : S_GUIDE_GRID), 1);
        add(I_GUIDE_HEADER, tr(S_GUIDE_HEADER), NULL, tr(P.guide_header == 1 ? S_HEADER_CHANNEL : P.guide_header == 2 ? S_HEADER_HIDE : S_HEADER_FULL), 1);
        add(I_HINTS_P, tr(S_HINTS), NULL, tr(P.hints ? S_ON : S_OFF), 1);
        add(I_LOGO_PRIO, tr(S_LOGO_PRIORITY), NULL, tr(P.logo_priority ? S_LOGO_EPG : S_LOGO_PLAYLIST), 1);
        break;
    case SEC_LIST:
        add(I_ADAPTIVE_P, tr(S_ADAPTIVE), NULL, tr(P.adaptive ? S_ON : S_OFF), 1);
        add(I_RESET_SCORES, tr(S_RESET_SCORES), NULL, NULL, 0);
        snprintf(v, sizeof v, "%d", repo_hidden_count());
        add(I_HIDDEN_P, tr(S_HIDDEN_ITEMS), NULL, v, 0);
        break;
    case SEC_LOCK:
        add(I_PIN_SET, tr(S_PIN_CHANGE), NULL, tr(P.pin[0] ? S_PIN_IS_SET : S_PIN_NOT_SET), 0);
        if (P.pin[0]) add(I_PIN_CLEAR, tr(S_PIN_CLEAR), NULL, NULL, 0);
        add(I_PIN_ASK, tr(S_PIN_ASK), NULL, tr(P.pin_ask == PIN_ONCE ? S_PIN_ONCE : S_PIN_EACH), 1);
        add(I_PIN_INPUT, tr(S_PIN_INPUT), NULL, tr(P.pin_input == PIN_IN_RING ? S_PIN_RING : P.pin_input == PIN_IN_KEYS ? S_PIN_KEYS : S_PIN_ROWS), 1);
        break;
    case SEC_NET:
        add(I_RELAY_ALL, tr(S_RELAY_ALL), tr(S_RELAY_ALL_SUB), tr(P.relay_all ? S_ON : S_OFF), 1);
        if (tv_direct_count()) { snprintf(v, sizeof v, "%d", tv_direct_count()); add(I_RELAY_DIRECT, tr(S_RELAY_DIRECT), tr(S_RELAY_DIRECT_SUB), v, 0); }
        add(I_ANYCERT, tr(S_ANY_CERT), tr(S_ANY_CERT_SUB), tr(P.any_cert ? S_ON : S_OFF), 1);
        break;
    case SEC_TMDB:
        add(I_TMDB_KEY, tr(S_TMDB_KEY), tr(S_TMDB_KEY_SUB), tr(tmdb_enabled() ? S_TMDB_SET : S_TMDB_NOT_SET), 0);
        add(I_TMDB_PHONE, tr(S_TMDB_FROM_PHONE), NULL, NULL, 0);
        add(I_TMDB_API, tr(S_TMDB_API), P.tmdb_api[0] ? P.tmdb_api : TMDB_API_DEFAULT, P.tmdb_api[0] ? NULL : tr(S_TMDB_DEFAULT), 0);
        add(I_TMDB_IMG, tr(S_TMDB_IMG), P.tmdb_img[0] ? P.tmdb_img : TMDB_IMG_DEFAULT, P.tmdb_img[0] ? NULL : tr(S_TMDB_DEFAULT), 0);
        if (tmdb_enabled()) {
            add(I_TMDB_TEST, tr(S_TMDB_TEST), NULL, NULL, 0);
            add(I_TMDB_REMOVE, tr(S_TMDB_REMOVE), NULL, NULL, 0);
        }
        break;
    case SEC_DATA:
        add(I_BACKUP_SAVE, tr(S_BACKUP_SAVE), tr(S_BACKUP_SAVE_SUB), NULL, 0);
        add(I_BACKUP_RESTORE, tr(S_BACKUP_RESTORE), NULL, NULL, 0);
        add(I_CLEAR_LOGOS, tr(S_CLEAR_LOGOS), NULL, NULL, 0);
        add(I_CLEAR_EPG, tr(S_CLEAR_EPG), NULL, NULL, 0);
        add(I_DATA_DIR, tr(S_DATA_DIR), app_platform_get()->data_dir, NULL, 0);
        break;
    case SEC_ABOUT:
        add(I_VERSION, tr(S_VERSION), NULL, PTV_VERSION, 0);
        add(I_SOURCE, tr(S_SOURCE_CODE), "https://github.com/perfect-tv", NULL, 0);
        add(I_LICENSE, tr(S_LICENSE), tr(S_LICENSE_TEXT), NULL, 0);
        break;
    }
    if (ST.focus >= ST.n) ST.focus = ST.n - 1;
    if (ST.focus < 0) ST.focus = 0;
}

static void after_pin_clear(void *c) { (void)c; P.pin[0] = 0; prefs_save(); tv_toast(tr(S_PIN_REMOVED)); build(); }
static void after_pin_ok(void *c) { (void)c; pin_create(NULL, NULL); }
static void clear_epg_yes(void *c) {
    (void)c;
    loaders_cancel();
    for (int i = 0; i < R.nepg; i++) epg_remove_files(R.epg[i].hash);
    repo_open_epg();
    load_epg(1);
    tv_toast(tr(S_EPG_CLEARED));
}

/* ---- резервная копия ---- */
static backup_item BK[16];
static int nbk, bk_pick;
static void restore_go(void *c) {
    (void)c;
    char err[100] = "";
    if (backup_restore(BK[bk_pick].path, err, sizeof err)) { tv_toast(!strcmp(err, "format") ? tr(S_BACKUP_BAD) : trf(S_BACKUP_FAILED, err)); return; }
    tv_restart();
    tv_toast(tr(S_BACKUP_RESTORED));
}
static void restore_yes(void *c) {
    /* копия заменит и PIN: с заданным PIN — сначала спросить его */
    if (P.pin[0]) pin_ask(restore_go, NULL, c);
    else restore_go(c);
}
static void bk_when(int64_t t, char *out, size_t cap) {
    char hm[8];
    fmt_hm(t, hm);
    ltm m = local_tm(t);
    snprintf(out, cap, "%02d.%02d.%04d %s", m.d, m.mo, m.y, hm);
}
static void restore_chosen(int i, void *c) {
    (void)c;
    if (i < 0 || i >= nbk) return;
    bk_pick = i;
    char w[80];
    bk_when(BK[i].created, w, sizeof w);
    confirm_open(trf(S_BACKUP_CONFIRM, w), tr(S_BACKUP_DO), restore_yes, NULL);
}
static void restore_open(void) {
    nbk = backup_list(BK, 16);
    if (!nbk) { tv_toast(tr(S_BACKUP_NONE)); return; }
    static char labels[16][160];
    static const char *items[16];
    for (int i = 0; i < nbk; i++) {
        char w[80];
        bk_when(BK[i].created, w, sizeof w);
        snprintf(labels[i], sizeof labels[i], "%s  ·  %s", w, tr(BK[i].usb ? S_BACKUP_ON_USB : S_BACKUP_ON_TV));
        items[i] = labels[i];
    }
    choose_open(tr(S_BACKUP_CHOOSE), items, nbk, 0, restore_chosen, NULL);
}
static void save_backup(void) {
    char where[600] = "", err[100] = "";
    if (backup_save(where, sizeof where, err, sizeof err)) tv_toast(trf(S_BACKUP_FAILED, err));
    else tv_toast(trf(S_BACKUP_SAVED, where));
}

/* ---- TMDB ---- */
static int is_default(const char *v, const char *def) {
    char a[300], b[300];
    snprintf(a, sizeof a, "%s", v);
    snprintf(b, sizeof b, "%s", def);
    size_t n = strlen(a);
    while (n && a[n - 1] == '/') a[--n] = 0;
    return !strcmp(a, b);
}
static void tmdb_changed(void) { tmdb_cache_clear(); prefs_save(); build(); app_dirty(); }
static void set_tmdb_key(const char *t, void *c) { (void)c; snprintf(P.tmdb_key, sizeof P.tmdb_key, "%s", t); trim(P.tmdb_key); tmdb_changed(); }
static void set_tmdb_api(const char *t, void *c) { (void)c; snprintf(P.tmdb_api, sizeof P.tmdb_api, "%s", is_default(t, TMDB_API_DEFAULT) ? "" : t); tmdb_changed(); }
static void set_tmdb_img(const char *t, void *c) { (void)c; snprintf(P.tmdb_img, sizeof P.tmdb_img, "%s", is_default(t, TMDB_IMG_DEFAULT) ? "" : t); tmdb_changed(); }
static void tmdb_from_phone(const phone_result *r, void *c) {
    (void)c;
    if (r->kind != PHONE_TMDB) return;
    snprintf(P.tmdb_key, sizeof P.tmdb_key, "%s", r->key);
    if (r->api[0]) set_tmdb_api(r->api, NULL);
    if (r->img[0]) set_tmdb_img(r->img, NULL);
    tmdb_changed();
}
static void tmdb_tested(const char *err) { tv_toast(err ? err : tr(S_TMDB_OK)); }
static void tmdb_remove_yes(void *c) { (void)c; P.tmdb_key[0] = 0; tmdb_changed(); }

static void change(item *it, int dir) {
    int d = dir ? dir : 1;
#define CYCLE(arr, field) do { int n_ = (int)(sizeof arr / sizeof arr[0]); P.field = arr[((sel_of(arr, n_, P.field) + d) % n_ + n_) % n_]; } while (0)
    switch (it->id) {
    case I_ARCHIVE_DAYS: CYCLE(ARCH, archive_days); break;
    case I_INFO_HIDE: CYCLE(HIDE, info_hide); break;
    case I_UPKEY: P.up_key = !P.up_key; break;
    case I_START_LAST: P.start_last = !P.start_last; break;
    case I_REMIND: CYCLE(REM, remind_before); break;
    case I_ANYCERT: P.any_cert = !P.any_cert; break;
    case I_RELAY_ALL: P.relay_all = !P.relay_all; break;   /* действует со следующего открытия потока */
    case I_ASPECT_P: P.aspect = ((P.aspect + d) % 4 + 4) % 4; break;
    case I_EPG_HOURS: CYCLE(EPGH, epg_hours); break;
    case I_PL_HOURS: CYCLE(PLH, playlist_hours); break;
    case I_LANG: P.lang = ((P.lang + d) % 3 + 3) % 3; lang_set(P.lang); break;
    case I_TZ: {   /* системный, затем UTC−12 … UTC+14 по получасу (P.tz — в четвертях часа) */
        int n = 1 + 53, idx = P.tz == TZ_AUTO ? 0 : 1 + (P.tz + 48) / 2;
        idx = ((idx + d) % n + n) % n;
        P.tz = idx == 0 ? TZ_AUTO : -48 + (idx - 1) * 2;
        break;
    }
    case I_SCALE: P.ui_scale = ((P.ui_scale + (d > 0 ? 2 : 1)) % 3); u_init(); break;
    case I_NUMBER: P.number_mode = ((P.number_mode + d) % 3 + 3) % 3; break;
    case I_GUIDE_VIEW: P.guide_mode = !P.guide_mode; break;
    case I_GUIDE_HEADER: P.guide_header = ((P.guide_header + d) % 3 + 3) % 3; break;
    case I_HINTS_P: P.hints = !P.hints; break;
    case I_LOGO_PRIO: P.logo_priority = !P.logo_priority; break;
    case I_ADAPTIVE_P: P.adaptive = !P.adaptive; repo_apply_view(); break;
    case I_PIN_ASK: P.pin_ask = !P.pin_ask; break;
    case I_PIN_INPUT: P.pin_input = ((P.pin_input + d) % 3 + 3) % 3; break;
    default: return;
    }
#undef CYCLE
    prefs_save();
    build();
}

static void action(item *it) {
    if (it->choice) { change(it, 1); return; }
    if (it->id >= I_PL) { form_playlist(it->id - I_PL); return; }
    switch (it->id) {
    case I_PL_ADD: form_playlist(-1); break;
    case I_EPG_REFRESH: case I_EPG_SRC: load_epg(1); tv_toast(tr(S_EPG_STATUS_LOADING)); break;
    case I_RESET_SCORES: repo_reset_scores(); tv_toast(tr(S_SCORES_RESET)); break;
    case I_HIDDEN_P: hidden_open(); break;
    case I_PIN_SET: if (P.pin[0]) pin_ask(after_pin_ok, NULL, NULL); else pin_create(NULL, NULL); break;
    case I_PIN_CLEAR: pin_ask(after_pin_clear, NULL, NULL); break;
    case I_RELAY_DIRECT: tv_direct_reset(); tv_toast(tr(S_RELAY_DIRECT_RESET)); build(); break;
    case I_TMDB_KEY: form_text(tr(S_TMDB_KEY), P.tmdb_key, 0, set_tmdb_key, NULL); break;
    case I_TMDB_PHONE: phone_open_kind(PHONE_TMDB, tmdb_from_phone, NULL); break;
    case I_TMDB_API: form_text(tr(S_TMDB_API), P.tmdb_api[0] ? P.tmdb_api : TMDB_API_DEFAULT, 0, set_tmdb_api, NULL); break;
    case I_TMDB_IMG: form_text(tr(S_TMDB_IMG), P.tmdb_img[0] ? P.tmdb_img : TMDB_IMG_DEFAULT, 0, set_tmdb_img, NULL); break;
    case I_TMDB_TEST: tv_toast(tr(S_TMDB_CHECKING)); tmdb_test(tmdb_tested); break;
    case I_TMDB_REMOVE: confirm_open(tr(S_TMDB_REMOVE), tr(S_DELETE), tmdb_remove_yes, NULL); break;
    case I_BACKUP_SAVE: save_backup(); break;
    case I_BACKUP_RESTORE: restore_open(); break;
    case I_CLEAR_LOGOS: img_cache_clear(); tv_toast(tr(S_LOGOS_CLEARED)); break;
    case I_CLEAR_EPG: confirm_open(tr(S_CLEAR_EPG), tr(S_DELETE), clear_epg_yes, NULL); break;
    default: break;
    }
}

static int key(layer *l, int k, int ev) {
    (void)l;
    if (ST.zone == 0) {
        if (k == KEY_UP || k == KEY_DOWN) { ST.sec = (ST.sec + (k == KEY_DOWN ? 1 : -1) + NSEC) % NSEC; ST.focus = ST.top = 0; build(); }
        else if (k == KEY_RIGHT || (k == KEY_OK && ev == EV_SHORT)) { if (ST.n) ST.zone = 1; }
        else if (k == KEY_BACK && ev == EV_SHORT) layer_pop(&L);
        return 1;
    }
    if (k == KEY_UP || k == KEY_DOWN) { if (ST.n) ST.focus = CLAMP(ST.focus + (k == KEY_DOWN ? 1 : -1), 0, ST.n - 1); return 1; }
    if (k == KEY_CH_UP || k == KEY_CH_DOWN) { ST.focus = CLAMP(ST.focus + (k == KEY_CH_DOWN ? 8 : -8), 0, ST.n - 1); return 1; }
    if (k == KEY_BACK && ev == EV_SHORT) { ST.zone = 0; return 1; }
    item *it = ST.n ? &ST.it[ST.focus] : NULL;
    if (!it) return 1;
    if (k == KEY_LEFT) { if (it->choice) change(it, -1); else ST.zone = 0; return 1; }
    if (k == KEY_RIGHT) { if (it->choice) change(it, 1); return 1; }
    if (k == KEY_OK && ev == EV_SHORT) action(it);
    return 1;
}

static void tick(layer *l, double now) {
    (void)l;
    static double next;
    if (now >= next) { next = now + 1; if (ST.sec == SEC_PLAYLISTS) build(); }
}

static void draw(layer *l, gfx_t *g) {
    (void)l;
    static int built_lang;
    if (built_lang != lang_rev()) { built_lang = lang_rev(); build(); }   /* язык сменили, пока окно открыто */
    d_clear(g, C_SETTINGS_BG);
    int lw = E(25);
    d_fill(g, 0, 0, lw, SH, C_FILL);
    d_text(g, E(4), E(3), 1.8, F_B, C_WHITE, tr(S_SETTINGS), lw - E(5));
    int sy = E(8);
    for (int i = 0; i < NSEC; i++) {
        int foc = i == ST.sec, ry = sy + i * E(3.6);
        uint32_t bg = foc ? (ST.zone == 0 ? C_WHITE : C_FILL_ACTIVE) : 0;
        if (bg) d_round(g, E(2), ry, lw - E(3.5), E(3.2), E(0.4), bg);
        d_text_mid(g, E(3.2), ry + E(1.6), 1.35, foc ? F_S : F_R, foc && ST.zone == 0 ? 0xff101010u : C_WHITE, tr(SEC_NAMES[i]), lw - E(6));
    }
    int x = lw + E(3), w = SW - x - E(4);
    d_text(g, x, E(3), 1.7, F_B, C_WHITE, tr(SEC_NAMES[ST.sec]), w);
    int ly = E(7.4), bottom = SH - E(5.4);
    int row = E(4.6);
    int rows = (bottom - ly) / row;
    if (ST.top > ST.focus) ST.top = ST.focus;
    if (ST.focus >= ST.top + rows) ST.top = ST.focus - rows + 1;
    if (ST.top < 0) ST.top = 0;
    for (int r = 0; r < rows && ST.top + r < ST.n; r++) {
        int i = ST.top + r;
        item *it = &ST.it[i];
        int ry = ly + r * row, foc = ST.zone == 1 && i == ST.focus;
        d_round(g, x, ry, w, row - E(0.5), E(0.4), foc ? C_WHITE : C_FILL);
        uint32_t fg = foc ? 0xff101010u : C_WHITE, fg2 = foc ? 0xff404040u : C_TEXT3;
        char val[220] = "";
        if (it->value[0]) snprintf(val, sizeof val, it->choice ? "‹  %s  ›" : "%s", it->value);
        int vw = val[0] ? d_text_w(1.2, F_R, val) : 0;
        int cy = ry + (row - E(0.5)) / 2;
        if (it->sub[0]) {
            d_text(g, x + E(1.2), ry + E(0.45), 1.35, F_R, fg, it->label, w - E(3) - vw);
            d_text(g, x + E(1.2), ry + E(2.3), 1.1, F_R, fg2, it->sub, w - E(3) - vw);
        } else d_text_mid(g, x + E(1.2), cy, 1.35, F_R, fg, it->label, w - E(3) - vw);
        if (val[0]) d_text_mid(g, x + w - E(1.2) - vw, cy, 1.2, F_R, foc ? 0xff303030u : C_TEXT2, val, 0);
    }
    d_fade_edges(g, x, ly, w, rows * row, ST.top > 0, ST.top + rows < ST.n, C_SETTINGS_BG);
    if (P.hints) d_text(g, x, SH - E(3.4), 1.15, F_R, C_HINT, tr(ST.zone ? S_SETTINGS_HINT_ITEMS : S_SETTINGS_HINT_SECTIONS), w);
}

void settings_open(void) {
    L.name = "settings";
    L.key = key;
    L.draw = draw;
    L.tick = tick;
    L.covers = 1;
    ST.zone = 0;
    ST.focus = ST.top = 0;
    build();
    layer_push(&L);
}
