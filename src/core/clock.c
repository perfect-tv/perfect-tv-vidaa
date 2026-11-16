#define _GNU_SOURCE
#include "clock.h"
#include "lang.h"
#include "prefs.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static atomic_llong offset, fake;

int64_t clock_now(void) {
    long long f = atomic_load(&fake);
    if (f) return f;
    return (int64_t)time(NULL) + atomic_load(&offset);
}
void clock_fake(int64_t t) { atomic_store(&fake, (long long)t); }

void clock_server(int64_t s) {
    if (s <= 1500000000) return;
    int64_t d = s - (int64_t)time(NULL);
    if (d > 30 || d < -30) {
        if (atomic_exchange(&offset, (long long)d) != d) fprintf(stderr, "clock_offset=%lld\n", (long long)d);
    } else atomic_store(&offset, 0);
}

int clock_tz(int64_t t) {
    int tz = P.tz;   /* -99 — системный; иначе часы, со знаком (15 мин шаги: в четвертях) */
    if (tz != TZ_AUTO) return tz * 900;
    time_t tt = (time_t)t;
    struct tm tm;
    if (!localtime_r(&tt, &tm)) return 0;
    return (int)tm.tm_gmtoff;
}

static void civil(int64_t days, int *y, int *m, int *d) {
    /* алгоритм Howard Hinnant */
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

ltm local_tm(int64_t t) {
    ltm r;
    int64_t l = t + clock_tz(t);
    int64_t days = l >= 0 ? l / 86400 : (l - 86399) / 86400;
    int64_t sec = l - days * 86400;
    r.h = (int)(sec / 3600);
    r.m = (int)(sec / 60 % 60);
    r.s = (int)(sec % 60);
    r.wd = (int)((days % 7 + 7 + 3) % 7);   /* 1970-01-01 — четверг */
    civil(days, &r.y, &r.mo, &r.d);
    return r;
}

int64_t day_start(int64_t t) {
    int64_t l = t + clock_tz(t);
    int64_t days = l >= 0 ? l / 86400 : (l - 86399) / 86400;
    int64_t s = days * 86400;
    return s - clock_tz(s);
}

void fmt_hm(int64_t t, char out[8]) {
    ltm m = local_tm(t);
    snprintf(out, 8, "%02d:%02d", m.h % 24, m.m % 60);
}

static const int WD[7] = {S_WD_MON, S_WD_TUE, S_WD_WED, S_WD_THU, S_WD_FRI, S_WD_SAT, S_WD_SUN};
static const int MON[12] = {S_MON_1, S_MON_2, S_MON_3, S_MON_4, S_MON_5, S_MON_6, S_MON_7, S_MON_8, S_MON_9, S_MON_10, S_MON_11, S_MON_12};

void fmt_day(int64_t t, char *out, int cap) {
    int64_t today = day_start(clock_now()), d = day_start(t);
    long diff = (long)((d - today + 43200) / 86400);
    if (diff == 0) { snprintf(out, (size_t)cap, "%s", tr(S_TODAY)); return; }
    if (diff == -1) { snprintf(out, (size_t)cap, "%s", tr(S_YESTERDAY)); return; }
    if (diff == 1) { snprintf(out, (size_t)cap, "%s", tr(S_TOMORROW)); return; }
    ltm m = local_tm(t);
    snprintf(out, (size_t)cap, "%s, %02d.%02d", tr(WD[m.wd]), m.d, m.mo);
}

void fmt_day_short(int64_t t, char *wd, int wcap, char *dm, int dcap) {
    ltm m = local_tm(t);
    char w[16];
    snprintf(w, sizeof w, "%s", tr(WD[m.wd]));
    /* «Пн» -> «пн» (ASCII и кириллица в 2 байта) */
    if ((unsigned char)w[0] == 0xd0 && (unsigned char)w[1] >= 0x90 && (unsigned char)w[1] <= 0x9f) w[1] = (char)(w[1] + 0x20);
    else if ((unsigned char)w[0] == 0xd0 && (unsigned char)w[1] >= 0xa0 && (unsigned char)w[1] <= 0xaf) { w[0] = (char)0xd1; w[1] = (char)(w[1] - 0x20); }
    else if (w[0] >= 'A' && w[0] <= 'Z') w[0] = (char)(w[0] + 32);
    snprintf(wd, (size_t)wcap, "%s", w);
    snprintf(dm, (size_t)dcap, "%d %s", m.d, tr(MON[(m.mo + 11) % 12]));
}

void fmt_dur(int64_t s, char *out, int cap) {
    if (s < 0) s = -s;
    if (s >= 3600) snprintf(out, (size_t)cap, "%d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
    else snprintf(out, (size_t)cap, "%d:%02d", (int)(s / 60), (int)(s % 60));
}
