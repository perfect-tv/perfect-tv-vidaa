#include "prefs.h"
#include "app.h"
#include "lang.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

prefs P;

typedef struct { const char *key; int *v; int def; } int_pref;

static int_pref *ints(int *n) {
    static int_pref t[] = {
        {"lang", &P.lang, LANG_AUTO},
        {"tz", &P.tz, TZ_AUTO},
        {"ui_scale", &P.ui_scale, 0},
        {"archive_days", &P.archive_days, 7},
        {"info_hide", &P.info_hide, 5},
        {"number_mode", &P.number_mode, NUM_BADGE},
        {"guide_mode", &P.guide_mode, GUIDE_GRID},
        {"guide_header", &P.guide_header, 0},
        {"guide_desc", &P.guide_desc, 0},
        {"marquee", &P.marquee, 1},
        {"up_key", &P.up_key, UP_SCHEDULE},
        {"hints", &P.hints, 1},
        {"adaptive", &P.adaptive, 0},
        {"start_last", &P.start_last, 1},
        {"epg_hours", &P.epg_hours, 12},
        {"playlist_hours", &P.playlist_hours, 24},
        {"any_cert", &P.any_cert, 1},
        {"relay_all", &P.relay_all, 1},
        {"ttx_keypad", &P.ttx_keypad, 0},
        {"ttx_mix", &P.ttx_mix, 1},
        {"logo_priority", &P.logo_priority, 0},
        {"sub_size", &P.sub_size, 100},
        {"sub_color", &P.sub_color, -1},
        {"sub_bg", &P.sub_bg, 1},
        {"sub_edge", &P.sub_edge, 0},
        {"sub_bottom", &P.sub_bottom, 0},
        {"aspect", &P.aspect, ASPECT_FIT},
        {"remind_before", &P.remind_before, 1},
        {"pin_ask", &P.pin_ask, PIN_EACH},
        {"pin_input", &P.pin_input, PIN_IN_ROWS},
        {"last_playlist", &P.last_playlist, 0},
    };
    *n = (int)(sizeof t / sizeof t[0]);
    return t;
}

void prefs_load(void) {
    int n;
    int_pref *t = ints(&n);
    for (int i = 0; i < n; i++) {
        const char *v = store_get(t[i].key);
        *t[i].v = v ? atoi(v) : t[i].def;
    }
    const char *s = store_get("pin");
    snprintf(P.pin, sizeof P.pin, "%s", s ? s : "");
    s = store_get("audio_pref");
    snprintf(P.audio_pref, sizeof P.audio_pref, "%s", s ? s : "");
    s = store_get("sub_pref");
    snprintf(P.sub_pref, sizeof P.sub_pref, "%s", s ? s : "");
    s = store_get("tmdb_key");
    snprintf(P.tmdb_key, sizeof P.tmdb_key, "%s", s ? s : "");
    s = store_get("tmdb_api");
    snprintf(P.tmdb_api, sizeof P.tmdb_api, "%s", s ? s : "");
    s = store_get("tmdb_img");
    snprintf(P.tmdb_img, sizeof P.tmdb_img, "%s", s ? s : "");
    s = store_get("last_channel");
    snprintf(P.last_channel, sizeof P.last_channel, "%s", s ? s : "");
    s = store_get("last_group");
    snprintf(P.last_group, sizeof P.last_group, "%s", s ? s : "");
    /* Копия с другого ТВ или правка settings.json не должна давать вечную загрузку/сдвиг времени. */
    if (P.lang < LANG_AUTO || P.lang > LANG_RU) P.lang = LANG_AUTO;
    if (P.tz != TZ_AUTO && (P.tz < -48 || P.tz > 56)) P.tz = TZ_AUTO;   /* UTC−12…UTC+14 */
    if (P.ui_scale < 0 || P.ui_scale > 2) P.ui_scale = 0;
    if (P.archive_days < 1 || P.archive_days > 30) P.archive_days = 7;
    if (P.info_hide < 2 || P.info_hide > 60) P.info_hide = 5;
    if (P.number_mode < NUM_NONE || P.number_mode > NUM_BADGE) P.number_mode = NUM_BADGE;
    if (P.guide_mode < GUIDE_GRID || P.guide_mode > GUIDE_CHANNELS) P.guide_mode = GUIDE_GRID;
    if (P.guide_header < 0 || P.guide_header > 2) P.guide_header = 0;
    if (P.up_key < UP_SCHEDULE || P.up_key > UP_RECENT) P.up_key = UP_SCHEDULE;
    P.hints = !!P.hints;
    P.adaptive = !!P.adaptive;
    P.start_last = !!P.start_last;
    P.any_cert = !!P.any_cert;
    P.relay_all = !!P.relay_all;
    P.ttx_keypad = !!P.ttx_keypad;
    P.ttx_mix = !!P.ttx_mix;
    P.logo_priority = !!P.logo_priority;
    if (P.sub_size < 75 || P.sub_size > 200) P.sub_size = 100;
    if (P.sub_color != -1 && (P.sub_color < 0 || P.sub_color > 0xffffff)) P.sub_color = -1;
    if (P.sub_bg < 0 || P.sub_bg > 2) P.sub_bg = 1;
    if (P.sub_edge < 0 || P.sub_edge > 2) P.sub_edge = 0;
    if (P.sub_bottom < 0 || P.sub_bottom > 18) P.sub_bottom = 0;
    if (P.aspect < ASPECT_FIT || P.aspect > ASPECT_4_3) P.aspect = ASPECT_FIT;
    if (P.remind_before < 0 || P.remind_before > 10) P.remind_before = 1;
    if (P.pin_ask < PIN_EACH || P.pin_ask > PIN_ONCE) P.pin_ask = PIN_EACH;
    if (P.pin_input < PIN_IN_RING || P.pin_input > PIN_IN_KEYS) P.pin_input = PIN_IN_ROWS;
    if (P.epg_hours < 1 || P.epg_hours > 24) P.epg_hours = 12;
    if (P.playlist_hours < 1 || P.playlist_hours > 48) P.playlist_hours = 24;
    size_t pl = strlen(P.pin);
    if (pl < 4 || pl > 6) P.pin[0] = 0;
    for (const char *p = P.pin; *p; p++) if (!isdigit((unsigned char)*p)) { P.pin[0] = 0; break; }
    lang_set(P.lang);
}

void prefs_save(void) {
    int n;
    int_pref *t = ints(&n);
    char b[24];
    for (int i = 0; i < n; i++) {
        snprintf(b, sizeof b, "%d", *t[i].v);
        store_set(t[i].key, b);
    }
    store_set("pin", P.pin[0] ? P.pin : NULL);
    store_set("audio_pref", P.audio_pref[0] ? P.audio_pref : NULL);
    store_set("sub_pref", P.sub_pref[0] ? P.sub_pref : NULL);
    store_set("tmdb_key", P.tmdb_key[0] ? P.tmdb_key : NULL);
    store_set("tmdb_api", P.tmdb_api[0] ? P.tmdb_api : NULL);
    store_set("tmdb_img", P.tmdb_img[0] ? P.tmdb_img : NULL);
    store_set("last_channel", P.last_channel[0] ? P.last_channel : NULL);
    store_set("last_group", P.last_group);
}

double prefs_screen_dp(void) { return P.ui_scale == 1 ? 540 : P.ui_scale == 2 ? 432 : 480; }
