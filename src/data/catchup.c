#include "catchup.h"
#include "core/util.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* заменить первое вхождение [at, at+len) на rep */
static void splice(char *s, size_t cap, size_t at, size_t len, const char *rep) {
    size_t n = strlen(s), r = strlen(rep);
    if (n - len + r + 1 > cap) return;
    memmove(s + at + r, s + at + len, n - at - len + 1);
    memcpy(s + at, rep, r);
}

/* /(video|mono)\.(m3u8|ts) -> /$1-${start}-${duration}.$2 ; /(index|playlist)\.(m3u8|ts) -> /archive-…; /mpegts -> /timeshift_abs-${start}.ts */
static void flussonic(char *s, size_t cap) {
    static const char *names[] = {"video", "mono", "index", "playlist"};
    static const char *exts[] = {"m3u8", "ts"};
    for (int pass = 0; pass < 2; pass++) {
        /* первый проход — video|mono, второй — index|playlist (как два replace подряд) */
        char *best = NULL; int bn = -1, be = -1;
        for (int k = pass * 2; k < pass * 2 + 2; k++)
            for (int e = 0; e < 2; e++) {
                char pat[32];
                snprintf(pat, sizeof pat, "/%s.%s", names[k], exts[e]);
                char *p = strstr(s, pat);
                if (p && (!best || p < best)) { best = p; bn = k; be = e; }
            }
        if (best) {
            char rep[64];
            if (bn < 2) snprintf(rep, sizeof rep, "/%s-${start}-${duration}.%s", names[bn], exts[be]);
            else snprintf(rep, sizeof rep, "/archive-${start}-${duration}.%s", exts[be]);
            splice(s, cap, (size_t)(best - s), strlen(names[bn]) + strlen(exts[be]) + 2, rep);
        }
    }
    char *m = strstr(s, "/mpegts");
    if (m) splice(s, cap, (size_t)(m - s), 7, "/timeshift_abs-${start}.ts");
}

/* ^(https?://[^/]+)(/live)?(/[^/]+/[^/]+/)([^/.]+)(\.m3u8?|\.ts|)$ */
static int xc(const char *url, char *out, size_t cap) {
    if (!starts_with_ci(url, "http://") && !starts_with_ci(url, "https://")) return -1;
    const char *h = strstr(url, "://") + 3;
    const char *p = strchr(h, '/');
    if (!p || p == h) return -1;
    const char *rest = p;
    if (!strncmp(rest, "/live/", 6)) rest += 5;
    /* /u/p/name.ext */
    const char *a = rest;
    if (*a != '/') return -1;
    const char *b = strchr(a + 1, '/');
    if (!b || b == a + 1) return -1;
    const char *c = strchr(b + 1, '/');
    if (!c || c == b + 1) return -1;
    const char *name = c + 1;
    if (strchr(name, '/')) {
        /* (/live) не было, а «live» — первый сегмент без префикса: перепробовать без пропуска */
        return -1;
    }
    const char *dot = strchr(name, '.');
    size_t nl = dot ? (size_t)(dot - name) : strlen(name);
    if (!nl) return -1;
    const char *ext = dot ? dot : "";
    const char *oe;
    if (!*ext || !strcmp(ext, ".ts")) oe = ".ts";
    else if (!strcmp(ext, ".m3u8") || !strcmp(ext, ".m3u")) oe = ".m3u8";
    else return -1;
    snprintf(out, cap, "%.*s/timeshift%.*s${(d)M}/${(b)yyyy-MM-dd:HH-mm}/%.*s%s", (int)(p - url), url, (int)(c - a + 1), a, (int)nl, name, oe);
    return 0;
}

int catchup_template(const char *url, const char *type0, const char *source, char *out, size_t cap) {
    char type[32];
    u8_lower_str(type, sizeof type, type0 ? type0 : "");
    if (!source) source = "";
    if (!type[0]) {
        if (*source) snprintf(type, sizeof type, "%s", (strchr("?&/", source[0]) && source[1] && source[1] != '/') ? "append" : "default");
        else snprintf(type, sizeof type, "%s", strstr(url, "${") ? "default" : "shift");
    }
    char tmp[4096];
    if (!strcmp(type, "append") || !strcmp(type, "timeshift") || !strcmp(type, "shift")) {
        if (!strcmp(type, "append") && *source) {
            if (starts_with_ci(source, "http://") || starts_with_ci(source, "https://")) snprintf(tmp, sizeof tmp, "%s", source);
            else snprintf(tmp, sizeof tmp, "%s%s", url, source);
        } else {
            const char *base = *source ? source : url;
            snprintf(out, cap, "%s%sutc=${start}&lutc=${timestamp}", base, strchr(base, '?') ? "&" : "?");
            return 0;
        }
    } else if (!strcmp(type, "flussonic") || !strcmp(type, "flussonic-hls") || !strcmp(type, "flussonic-ts") || !strcmp(type, "fs")) {
        snprintf(out, cap, "%s", url);
        flussonic(out, cap);
        return 0;
    } else if (!strcmp(type, "xc")) {
        if (xc(url, tmp, sizeof tmp)) snprintf(tmp, sizeof tmp, "%s", url);
    } else if (!strcmp(type, "default")) {
        snprintf(tmp, sizeof tmp, "%s", *source ? source : url);
    } else return -1;
    if (!strstr(tmp, "${")) return catchup_template(tmp, "shift", "", out, cap);
    snprintf(out, cap, "%s", tmp);
    return 0;
}

int catchup_flussonic_hls(const char *type, const char *tpl) {
    return type && (starts_with_ci(type, "flussonic") || starts_with_ci(type, "fs")) && strstr(tpl, "-${start}-${duration}.m3u8");
}

void catchup_flussonic_now(const char *tpl, char *out, size_t cap) {
    snprintf(out, cap, "%s", tpl);
    char *p = strstr(out, "-${start}-${duration}.m3u8");
    if (p) splice(out, cap, (size_t)(p - out), strlen("-${start}-${duration}.m3u8"), "-${start}-now.m3u8");
}

static void enc(sbuf *o, const char *s) {   /* encodeURIComponent */
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || strchr("-_.!~*'()", c)) sb_addc(o, (char)c);
        else sb_printf(o, "%%%02X", c);
    }
}

static void repl_all(char *s, size_t cap, const char *from, const char *to) {
    size_t fl = strlen(from), tl = strlen(to);
    char *p = s;
    while ((p = strstr(p, from)) != NULL) { splice(s, cap, (size_t)(p - s), fl, to); p += tl; }
}

static void fmt_time(int64_t secs, const char *format, int utc, int shift_h, tz_fn tz, char *out, size_t cap) {
    int64_t sh = (int64_t)shift_h * 3600;
    if (!utc && tz) sh += tz(secs);
    int64_t t = secs + sh;
    int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    int64_t sec = t - days * 86400;
    /* civil from days */
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1), m = (int)(mp < 10 ? mp + 3 : mp - 9);
    int y = (int)(yoe + era * 400 + (m <= 2));
    char b[32];
    snprintf(out, cap, "%s", format);
    snprintf(b, sizeof b, "%d", y); repl_all(out, cap, "yyyy", b);
    snprintf(b, sizeof b, "%02d", m); repl_all(out, cap, "MM", b);
    snprintf(b, sizeof b, "%02d", d); repl_all(out, cap, "dd", b);
    snprintf(b, sizeof b, "%02d", (int)(sec / 3600)); repl_all(out, cap, "HH", b);
    snprintf(b, sizeof b, "%02d", (int)(sec / 60 % 60)); repl_all(out, cap, "mm", b);
    snprintf(b, sizeof b, "%02d", (int)(sec % 60)); repl_all(out, cap, "ss", b);
    snprintf(b, sizeof b, "%lld", (long long)secs * 1000); repl_all(out, cap, "UTF", b);
}

void catchup_prepare(const char *tpl, int64_t start, int64_t duration, int64_t now, tz_fn tz, char *out, size_t cap) {
    sbuf o = {0};
    const char *s = tpl;
    int64_t dur = duration < 60 ? 60 : duration, dmin = (dur + 30) / 60;
    while (*s) {
        const char *v = strstr(s, "${");
        if (!v) { sb_add(&o, s); break; }
        sb_addn(&o, s, (size_t)(v - s));
        const char *e = strchr(v + 2, '}');
        if (!e) { sb_add(&o, v); break; }
        /* ${(fn u? ±N)body} или ${name} */
        char fn[16] = "", body[200] = "";
        int utc = 0, shift = 0;
        const char *p = v + 2;
        if (*p == '(') {
            const char *q = strchr(p, ')');
            if (!q || q > e) { sb_addn(&o, v, (size_t)(e - v + 1)); s = e + 1; continue; }
            size_t k = 0;
            const char *r = p + 1;
            while (r < q && isalnum((unsigned char)*r) && k + 1 < sizeof fn) fn[k++] = *r++;
            fn[k] = 0;
            if (k > 1 && fn[k - 1] == 'u') { utc = 1; fn[k - 1] = 0; }
            if (r < q && (*r == '+' || *r == '-')) shift = atoi(r);
            p = q + 1;
        }
        size_t bl = (size_t)(e - p) < sizeof body - 1 ? (size_t)(e - p) : sizeof body - 1;
        memcpy(body, p, bl);
        body[bl] = 0;
        char val[256];
        int ok = 1;
        const char *key = fn[0] ? fn : body;
        if (start < 0) {
            if (!strcmp(key, "start")) snprintf(val, sizeof val, "%lld", (long long)now);
            else if (!strcmp(key, "offset")) snprintf(val, sizeof val, "0");
            else if (!strcmp(key, "timestamp")) snprintf(val, sizeof val, "%lld", (long long)now);
            else ok = 0;
        } else if (!strcmp(key, "start") || !strcmp(key, "utc")) snprintf(val, sizeof val, "%lld", (long long)start);
        else if (!strcmp(key, "end") || !strcmp(key, "utcend")) snprintf(val, sizeof val, "%lld", (long long)(start + dur));
        else if (!strcmp(key, "offset")) snprintf(val, sizeof val, "%lld", (long long)(now - start));
        else if (!strcmp(key, "duration")) snprintf(val, sizeof val, "%lld", (long long)dur);
        else if (!strcmp(key, "now") || !strcmp(key, "lutc") || !strcmp(key, "timestamp")) snprintf(val, sizeof val, "%lld", (long long)now);
        else if (!strcmp(key, "d") && fn[0]) {
            char b[24];
            snprintf(val, sizeof val, "%s", body);
            snprintf(b, sizeof b, "%lld", (long long)dmin); repl_all(val, sizeof val, "M", b);
            snprintf(b, sizeof b, "%lld", (long long)dur); repl_all(val, sizeof val, "S", b);
            snprintf(b, sizeof b, "%lld", (long long)(dmin / 60)); repl_all(val, sizeof val, "h", b);
            snprintf(b, sizeof b, "%02lld", (long long)(dmin % 60)); repl_all(val, sizeof val, "m", b);
            repl_all(val, sizeof val, "s", "00");
        } else if (!strcmp(key, "b") && fn[0]) fmt_time(start / 60 * 60 + start % 60, body, utc, shift, tz, val, sizeof val);
        else if (!strcmp(key, "e") && fn[0]) fmt_time(start + dmin * 60, body, utc, shift, tz, val, sizeof val);
        else if (!strcmp(key, "n") && fn[0]) fmt_time(now, body, utc, shift, tz, val, sizeof val);
        else ok = 0;
        if (!ok) {
            /* неизвестная переменная: «(…)» как есть, иначе пусто */
            if (fn[0]) { const char *q = strchr(v + 2, ')'); snprintf(val, sizeof val, "%.*s", (int)(q - v - 1), v + 2); }
            else val[0] = 0;
        }
        enc(&o, val);
        s = e + 1;
    }
    u8_copy(out, cap, o.s ? o.s : "");
    sb_free(&o);
}
