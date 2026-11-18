/* Cookie провайдера (core/cookies.h). */
#define _GNU_SOURCE
#include "cookies.h"
#include "clock.h"
#include "http.h"
#include "util.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_COOKIES 100

typedef struct {
    char *name, *value;
    char domain[256], path[512];
    int host_only, secure;
    int64_t exp;        /* unix; 0 — до конца сеанса */
    unsigned seq;       /* порядок создания */
} ck;

static struct {
    pthread_mutex_t mu;
    ck c[MAX_COOKIES];
    int n;
    unsigned seq;
} J = {.mu = PTHREAD_MUTEX_INITIALIZER};

static void drop(int i) {
    free(J.c[i].name);
    free(J.c[i].value);
    J.c[i] = J.c[--J.n];
}

/* хост, путь (без запроса) и https ли — из адреса; -1 — не http(s) */
static int split(const char *url, char *host, size_t hc, char *path, size_t pc, int *https) {
    http_url u;
    if (http_split_url(url, &u)) return -1;
    u8_lower_str(host, hc, u.host);
    size_t l = strcspn(u.path, "?#");
    if (l >= pc) l = pc - 1;
    memcpy(path, u.path, l);
    path[l] = 0;
    if (!path[0]) snprintf(path, pc, "/");
    *https = !strcasecmp(u.scheme, "https");
    return 0;
}

static int is_ip(const char *h) { return strchr(h, ':') || strspn(h, "0123456789.") == strlen(h); }

/* host принадлежит домену d (сам d или его поддомен) */
static int domain_match(const char *host, const char *d) {
    if (!strcmp(host, d)) return 1;
    size_t hl = strlen(host), dl = strlen(d);
    return hl > dl && host[hl - dl - 1] == '.' && !strcmp(host + hl - dl, d) && !is_ip(host);
}

/* путь запроса p подходит под путь cookie c (RFC 6265, 5.1.4) */
static int path_match(const char *p, const char *c) {
    size_t cl = strlen(c);
    if (strncmp(p, c, cl)) return 0;
    return p[cl] == 0 || c[cl - 1] == '/' || p[cl] == '/';
}

/* Expires: «Wed, 21 Oct 2026 07:28:00 GMT», «Wed, 21-Oct-26 07:28:00 GMT» и т. п.; 0 — не разобрано */
static int64_t expires_date(const char *v) {
    char s[128], m[4];
    snprintf(s, sizeof s, "%s", v);
    for (char *p = s; *p; p++) if (*p == '-') *p = ' ';
    int d, y, H, M, S;
    const char *c = strchr(s, ',');
    if (!c || sscanf(c + 1, " %d %3s %d %d:%d:%d", &d, m, &y, &H, &M, &S) != 6) return 0;
    if (y < 70) y += 2000;
    else if (y < 100) y += 1900;
    char norm[80];
    snprintf(norm, sizeof norm, "X, %02d %s %04d %02d:%02d:%02d GMT", d, m, y, H, M, S);
    return http_date(norm);
}

void cookies_set(const char *url, const char *sc, int64_t date) {
    char host[256], rpath[1024];
    int https;
    if (!sc || split(url, host, sizeof host, rpath, sizeof rpath, &https)) return;
    /* имя=значение */
    size_t pl = strcspn(sc, ";");
    char *pair = xstrndup(sc, pl);
    if (!pair) return;
    char *eq = strchr(pair, '=');
    if (!eq) { free(pair); return; }
    *eq = 0;
    trim(pair);
    char *val = eq + 1;
    trim(val);
    if (!*pair) { free(pair); return; }
    char domain[256] = "", path[512] = "";
    int secure = 0, have_age = 0;
    int64_t now = clock_now(), exp = 0;
    for (const char *p = sc + pl; *p == ';';) {
        p++;
        size_t l = strcspn(p, ";");
        char a[600];
        snprintf(a, sizeof a, "%.*s", (int)(l < sizeof a - 1 ? l : sizeof a - 1), p);
        p += l;
        char *v = strchr(a, '=');
        if (v) *v++ = 0;
        trim(a);
        if (v) trim(v);
        if (!strcasecmp(a, "Secure")) secure = 1;
        else if (!v) continue;
        else if (!strcasecmp(a, "Max-Age")) {
            char *e;
            long long s = strtoll(v, &e, 10);
            if (e != v && !*e) { have_age = 1; exp = s <= 0 ? 1 : now + s; }
        } else if (!strcasecmp(a, "Expires") && !have_age) {
            int64_t t = expires_date(v);
            if (t) { exp = date ? now + (t - date) : t; if (exp <= 0) exp = 1; }
        } else if (!strcasecmp(a, "Domain")) {
            const char *d = v;
            while (*d == '.') d++;
            u8_lower_str(domain, sizeof domain, d);
        } else if (!strcasecmp(a, "Path")) {
            if (*v == '/') snprintf(path, sizeof path, "%s", v);
        }
    }
    if (secure && !https) { free(pair); return; }   /* Secure — только из ответа по https (как в браузерах, RFC 6265bis) */
    int host_only = !domain[0];
    if (host_only) snprintf(domain, sizeof domain, "%s", host);
    else if (!domain_match(host, domain) || (is_ip(host) && strcmp(host, domain))) { free(pair); return; }   /* чужой домен */
    if (!path[0]) {   /* каталог адреса */
        char *slash = strrchr(rpath, '/');
        if (!slash || slash == rpath) snprintf(path, sizeof path, "/");
        else snprintf(path, sizeof path, "%.*s", (int)(slash - rpath), rpath);
    }
    pthread_mutex_lock(&J.mu);
    int at = -1;
    for (int i = 0; i < J.n; i++)
        if (!strcmp(J.c[i].name, pair) && !strcmp(J.c[i].domain, domain) && !strcmp(J.c[i].path, path) && J.c[i].host_only == host_only) { at = i; break; }
    unsigned seq = at >= 0 ? J.c[at].seq : ++J.seq;
    if (at >= 0) drop(at);
    if (exp && exp <= now) { pthread_mutex_unlock(&J.mu); free(pair); return; }   /* удаление */
    if (J.n == MAX_COOKIES) {   /* места нет: сначала просроченные, иначе самая старая */
        int old = 0;
        for (int i = 0; i < J.n; i++) {
            if (J.c[i].exp && J.c[i].exp <= now) { old = i; break; }
            if (J.c[i].seq < J.c[old].seq) old = i;
        }
        drop(old);
    }
    ck *c = &J.c[J.n];
    memset(c, 0, sizeof *c);
    c->name = pair;
    c->value = strdup(val);
    if (!c->value) { free(pair); pthread_mutex_unlock(&J.mu); return; }
    snprintf(c->domain, sizeof c->domain, "%s", domain);
    snprintf(c->path, sizeof c->path, "%s", path);
    c->host_only = host_only;
    c->secure = secure;
    c->exp = exp;
    c->seq = seq;
    J.n++;
    pthread_mutex_unlock(&J.mu);
}

size_t cookies_header(const char *url, char *out, size_t cap) {
    char host[256], rpath[1024];
    int https;
    if (cap) out[0] = 0;
    if (!cap || split(url, host, sizeof host, rpath, sizeof rpath, &https)) return 0;
    int64_t now = clock_now();
    pthread_mutex_lock(&J.mu);
    int pick[MAX_COOKIES], n = 0;
    for (int i = 0; i < J.n; i++) {
        ck *c = &J.c[i];
        if (c->exp && c->exp <= now) continue;
        if (c->secure && !https) continue;
        if (c->host_only ? strcmp(host, c->domain) : !domain_match(host, c->domain)) continue;
        if (!path_match(rpath, c->path)) continue;
        int k = n++;   /* вставка: длиннее путь — раньше, при равных — старше */
        while (k > 0) {
            ck *p = &J.c[pick[k - 1]];
            size_t a = strlen(p->path), b = strlen(c->path);
            if (a > b || (a == b && p->seq < c->seq)) break;
            pick[k] = pick[k - 1];
            k--;
        }
        pick[k] = i;
    }
    size_t o = 0;
    for (int k = 0; k < n; k++) {
        ck *c = &J.c[pick[k]];
        size_t need = strlen(c->name) + 1 + strlen(c->value) + (o ? 2 : 0);
        if (o + need + 1 > cap) break;
        o += (size_t)snprintf(out + o, cap - o, "%s%s=%s", o ? "; " : "", c->name, c->value);
    }
    pthread_mutex_unlock(&J.mu);
    return o;
}

void cookies_from_headers(const char *url, const char *headers) {
    if (!headers || !strcasestr(headers, "set-cookie:")) return;
    int64_t date = 0;
    for (const char *p = headers; *p;) {
        const char *e = strstr(p, "\r\n");
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l > 5 && !strncasecmp(p, "Date:", 5)) { char *v = xstrndup(p + 5, l - 5); if (v) { trim(v); date = http_date(v); free(v); } }
        p = e ? e + 2 : p + l;
    }
    for (const char *p = headers; *p;) {
        const char *e = strstr(p, "\r\n");
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l > 11 && !strncasecmp(p, "Set-Cookie:", 11)) { char *v = xstrndup(p + 11, l - 11); if (v) { trim(v); cookies_set(url, v, date); free(v); } }
        p = e ? e + 2 : p + l;
    }
}

void cookies_clear(void) {
    pthread_mutex_lock(&J.mu);
    while (J.n) drop(J.n - 1);
    pthread_mutex_unlock(&J.mu);
}

int cookies_count(void) {
    pthread_mutex_lock(&J.mu);
    int n = J.n;
    pthread_mutex_unlock(&J.mu);
    return n;
}

void cookies_merge(sbuf *out, const char *headers, const char *url) {
    char given[4096] = "";
    for (const char *p = headers ? headers : ""; *p;) {
        const char *e = strstr(p, "\r\n");
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l > 7 && !strncasecmp(p, "Cookie:", 7)) {   /* cookie из заголовков канала — первыми */
            const char *v = p + 7;
            while (*v == ' ') v++;
            size_t gl = strlen(given);
            snprintf(given + gl, sizeof given - gl, "%s%.*s", gl ? "; " : "", (int)(l - (size_t)(v - p)), v);
        } else if (l) { sb_addn(out, p, l); sb_add(out, "\r\n"); }
        p = e ? e + 2 : p + l;
    }
    char jar[4096];
    size_t jl = cookies_header(url, jar, sizeof jar);
    if (!given[0] && !jl) return;
    sb_printf(out, "Cookie: %s%s%s\r\n", given, given[0] && jl ? "; " : "", jl ? jar : "");
}
