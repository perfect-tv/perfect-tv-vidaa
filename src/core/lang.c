#include "lang.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const EN[S_COUNT] = {
#define S(key, en, ru) en,
#include "strings.def"
#undef S
};
static const char *const RU[S_COUNT] = {
#define S(key, en, ru) ru,
#include "strings.def"
#undef S
};

static int ru = 1;

static int rev;
int lang_rev(void) { return rev * 2 + ru; }   /* меняется при смене языка */
void lang_set(int mode) {
    rev++;
    if (mode == LANG_EN) { ru = 0; return; }
    if (mode == LANG_RU) { ru = 1; return; }
    const char *l = getenv("LC_ALL");
    if (!l || !*l) l = getenv("LANG");
    /* на ТВ переменных обычно нет: приложение прежде всего для русскоязычных плейлистов */
    if (!l || !*l || !strncmp(l, "C", 1) || !strncmp(l, "POSIX", 5)) { ru = 1; return; }
    ru = !strncmp(l, "ru", 2) || !strncmp(l, "uk", 2) || !strncmp(l, "be", 2) || !strncmp(l, "kk", 2);
}
int lang_is_ru(void) { return ru; }
const char *tr(int id) { return id >= 0 && id < S_COUNT ? (ru ? RU[id] : EN[id]) : ""; }

static int form_of(long n) {
    long a = n < 0 ? -n : n;
    if (!ru) return a == 1 ? 0 : 1;
    if (a % 10 == 1 && a % 100 != 11) return 0;
    if (a % 10 >= 2 && a % 10 <= 4 && (a % 100 < 12 || a % 100 > 14)) return 1;
    return 2;
}

const char *trp(int id, long n) {
    static __thread char out[4][160];
    static __thread int k;
    char *o = out[k++ & 3];
    const char *s = tr(id);
    int f = form_of(n);
    for (int i = 0; i < f; i++) { const char *b = strchr(s, '|'); if (!b) break; s = b + 1; }
    const char *e = strchr(s, '|');
    char fmt[160];
    size_t len = e ? (size_t)(e - s) : strlen(s);
    if (len >= sizeof fmt) len = sizeof fmt - 1;
    memcpy(fmt, s, len);
    fmt[len] = 0;
    /* в форме может не быть числа («one hour») — тогда без подстановки */
    char *p = strstr(fmt, "%d");
    if (p) {
        p[1] = 's';
        char num[24];
        snprintf(num, sizeof num, "%ld", n);
        snprintf(o, sizeof out[0], fmt, num);
    } else snprintf(o, sizeof out[0], "%s", fmt);
    return o;
}

const char *trf(int id, ...) {
    static __thread char out[4][400];
    static __thread int k;
    char *o = out[k++ & 3];
    va_list ap;
    va_start(ap, id);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    vsnprintf(o, sizeof out[0], tr(id), ap);
#pragma GCC diagnostic pop
    va_end(ap);
    return o;
}
