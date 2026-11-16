#include "prefs.h"
#include "app.h"
#include "lang.h"
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
        {"up_key", &P.up_key, UP_SCHEDULE},
        {"hints", &P.hints, 1},
        {"adaptive", &P.adaptive, 0},
        {"start_last", &P.start_last, 1},
        {"epg_hours", &P.epg_hours, 12},
        {"playlist_hours", &P.playlist_hours, 24},
        {"any_cert", &P.any_cert, 1},
        {"logo_priority", &P.logo_priority, 0},
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
    s = store_get("last_channel");
    snprintf(P.last_channel, sizeof P.last_channel, "%s", s ? s : "");
    s = store_get("last_group");
    snprintf(P.last_group, sizeof P.last_group, "%s", s ? s : "");
    if (P.archive_days < 1 || P.archive_days > 30) P.archive_days = 7;
    if (P.info_hide < 2) P.info_hide = 5;
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
    store_set("last_channel", P.last_channel[0] ? P.last_channel : NULL);
    store_set("last_group", P.last_group);
}

double prefs_screen_dp(void) { return P.ui_scale == 1 ? 540 : P.ui_scale == 2 ? 432 : 480; }
