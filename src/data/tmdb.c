#define _GNU_SOURCE
#include "tmdb.h"
#include "app.h"
#include "net.h"
#include "json.h"
#include "epg.h"
#include "core/http.h"
#include "core/lang.h"
#include "core/prefs.h"
#include "core/util.h"
#include <math.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIN_SIMILARITY 0.72

/* ================= очистка названия =================
 * Регулярки — POSIX ERE по байтам (на ТВ локаль «C»): строка сначала в нижнем регистре, кириллица — буквальными
 * альтернативами, без диапазонов. Те же правила, что у Android-версии (Tmdb.searchQuery). */
#define SP "[[:space:]]"
#define SEASON "([0-9]+(-й)?" SP "+(с-н|сезон)|(с-н|сезон)" SP "+[0-9]+|s[-.[:space:]]*[0-9]+(-й)?)"
#define EPW "(серия|серии|серию|серий|эпизод|эпизоды|episode|c|с|ep|e)"
#define EPIS "([0-9]+(-[0-9]+|-я)?" SP "*" EPW "\\.?|" EPW "[-.]?" SP "*[0-9]+)"
enum { RX_AGE, RX_YEAR, RX_TAIL, RX_EPISODE, RX_DIGEST, RX_BRACKETS, RX_PART, RX_COLLECTION, RX_EMPTY, RX_N };
static const char *const RX_SRC[RX_N] = {
    SP "*[[(]?([0-9]|1[0-9]|2[0-5])\\+[])]?[.[:space:]]*$",
    SP "*\\((19[0-9][0-9]|20[0-4][0-9])\\)" SP "*(\\*" SP ".+)?$",
    "[.,]" SP "*" SEASON "?[-.,[:space:]]*" EPIS ".*$",
    SP "*" SEASON "?[-.,[:space:]]*" EPIS "\\.?",
    "\\." SP "+дайджест" SP "*$",
    "\\.?" SP "*\\(((c|с|e)(ерия|pisode)?[-.]?" SP "*[0-9]+|[0-9]+(-[^)[:space:]]+)?" SP "+серия)\\)",
    "\\.[^.:]+:" SP "*ч(асть|\\.)" SP "+[0-9]+[^[:space:]]*$",
    "\\." SP "*сборник" SP "+[0-9]+[^[:space:]]*" SP "*$",
    SP "*(\\(\\)|\\[\\])",
};
static regex_t RX[RX_N];
static int rx_ready;

static void rx_init(void) {
    if (rx_ready) return;
    for (int i = 0; i < RX_N; i++)
        if (regcomp(&RX[i], RX_SRC[i], REG_EXTENDED)) fprintf(stderr, "tmdb_regex=%d failed\n", i);
    rx_ready = 1;
}

/* вырезать первое совпадение; 1 — было */
static int cut(char *s, int rx) {
    regmatch_t m;
    if (regexec(&RX[rx], s, 1, &m, 0) || m.rm_so < 0) return 0;
    memmove(s + m.rm_so, s + m.rm_eo, strlen(s + m.rm_eo) + 1);
    return 1;
}

/* «…» / "…" в начале — только то, что в кавычках */
static void quoted(char *s) {
    const char *open = "«", *close = "»";
    size_t ol = strlen(open);
    if (!strncmp(s, open, ol)) {
        char *e = strstr(s + ol, close);
        if (e && e > s + ol) { size_t n = (size_t)(e - (s + ol)); memmove(s, s + ol, n); s[n] = 0; }
        return;
    }
    if (s[0] == '"') {
        char *e = strchr(s + 1, '"');
        if (e && e > s + 1) { size_t n = (size_t)(e - (s + 1)); memmove(s, s + 1, n); s[n] = 0; }
    }
}

void tmdb_query(const char *title, char *out, size_t cap) {
    rx_init();
    char s[600];
    u8_lower_str(s, sizeof s, title ? title : "");
    for (char *p = s; *p; p++) if ((unsigned char)*p < 0x20) *p = ' ';
    cut(s, RX_AGE);
    quoted(s);
    cut(s, RX_YEAR);
    cut(s, RX_TAIL);
    cut(s, RX_EPISODE);
    cut(s, RX_DIGEST);
    cut(s, RX_BRACKETS);
    cut(s, RX_PART);
    cut(s, RX_COLLECTION);
    cut(s, RX_AGE);
    cut(s, RX_EMPTY);
    trim(s);
    snprintf(out, cap, "%s", s);
}

/* ================= год ================= */
static int year_at(const char *p) {
    if (!((p[0] == '1' && p[1] == '9') || (p[0] == '2' && p[1] == '0'))) return 0;
    for (int i = 0; i < 4; i++) if (p[i] < '0' || p[i] > '9') return 0;
    return (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
}
static int year_end_ok(const char *q) { return !*q || *q == ' ' || *q == '\t' || *q == '\n' || *q == ',' || *q == '.' || *q == ')'; }

int tmdb_year(const char *title, const char *desc) {
    for (const char *p = title ? strchr(title, '(') : NULL; p; p = strchr(p + 1, '(')) {
        int y = year_at(p + 1);
        if (y && p[5] == ')') return y;
    }
    if (!desc) return 0;
    char d[400];
    u8_copy(d, sizeof d, desc);
    for (char *p = d; *p; p++) {
        if (p != d && !(p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == ',' || p[-1] == '(')) continue;
        int y = year_at(p);
        if (!y) continue;
        const char *q = p + 4;
        const char *r = q;
        while (*r == ' ') r++;
        if (!strncmp(r, "г", strlen("г"))) {   /* «1997 г.» */
            const char *t = r + strlen("г");
            if (*t == '.') t++;
            if (year_end_ok(t)) return y;
        }
        if (year_end_ok(q)) return y;
    }
    return 0;
}

/* ================= сходство ================= */
static int to_cps(const char *s, uint32_t *out, int max) {
    int n = 0;
    uint32_t c;
    while (n < max && (c = u8_next(&s))) out[n++] = c;
    return n;
}
double tmdb_similarity(const char *a0, const char *b0) {
    char a[400], b[400];
    search_fold(a0, a, sizeof a);
    search_fold(b0, b, sizeof b);
    uint32_t x[200], y[200];
    int n = to_cps(a, x, 200), m = to_cps(b, y, 200);
    if (!n || !m) return 0;
    int prev[201], cur[201];
    for (int j = 0; j <= m; j++) prev[j] = j;
    for (int i = 1; i <= n; i++) {
        cur[0] = i;
        for (int j = 1; j <= m; j++) {
            int v = prev[j] + 1;
            if (cur[j - 1] + 1 < v) v = cur[j - 1] + 1;
            if (prev[j - 1] + (x[i - 1] != y[j - 1]) < v) v = prev[j - 1] + (x[i - 1] != y[j - 1]);
            cur[j] = v;
        }
        memcpy(prev, cur, sizeof(int) * (size_t)(m + 1));
    }
    return 1.0 - (double)prev[m] / (n > m ? n : m);
}

static double sim(const tmdb_hit *h, const char *q) {
    double a = tmdb_similarity(q, h->title), b = tmdb_similarity(q, h->original);
    return a > b ? a : b;
}
static int kind_ok(const tmdb_hit *h) { return !h->type[0] || !strcmp(h->type, "movie") || !strcmp(h->type, "tv"); }

int tmdb_pick(const tmdb_hit *h, int n, const char *query, int year) {
    int cnt = 0, only = -1;
    for (int i = 0; i < n; i++) if (kind_ok(&h[i])) { cnt++; only = i; }
    if (!year) return cnt == 1 && sim(&h[only], query) >= 0.9 ? only : -1;
    int best = -1;
    double best_score = 0;
    for (int i = 0; i < n; i++) {
        if (!kind_ok(&h[i])) continue;
        double s = sim(&h[i], query);
        int ry = year_at(h[i].date) ? atoi(h[i].date) : 0;
        double bonus = !ry ? 0 : ry == year ? 0.15 : abs(ry - year) <= 1 ? 0.05 : -0.1;
        if (s < MIN_SIMILARITY && !(s >= 0.6 && ry == year)) continue;
        double score = s + bonus + (h[i].backdrop[0] ? 0.02 : 0);
        if (score > best_score) { best = i; best_score = score; }
    }
    return best;
}

int tmdb_parse(const char *text, size_t len, tmdb_hit **out) {
    *out = NULL;
    json_doc *d = json_parse(text, len);
    if (!d) return -1;
    json *arr = json_get(json_root(d), "results");
    int n = 0, cap = json_len(arr);
    tmdb_hit *v = calloc((size_t)(cap > 0 ? cap : 1), sizeof *v);
    for (json *o = json_is(arr, J_ARR) ? arr->child : NULL; o && v && n < cap; o = o->next) {
        const char *type = json_str(json_get(o, "media_type"), "");
        if (!strcmp(type, "person")) continue;
        tmdb_hit *h = &v[n++];
        snprintf(h->type, sizeof h->type, "%s", type);
#define S2(dst, a, b) do { const char *s_ = json_str(json_get(o, a), ""); if (!*s_) s_ = json_str(json_get(o, b), ""); u8_copy(dst, sizeof dst, s_); } while (0)
        S2(h->title, "title", "name");
        S2(h->original, "original_title", "original_name");
        S2(h->date, "release_date", "first_air_date");
#undef S2
        u8_copy(h->backdrop, sizeof h->backdrop, json_str(json_get(o, "backdrop_path"), ""));
        u8_copy(h->poster, sizeof h->poster, json_str(json_get(o, "poster_path"), ""));
        u8_copy(h->overview, sizeof h->overview, json_str(json_get(o, "overview"), ""));
        h->vote = json_num(json_get(o, "vote_average"), 0);
    }
    json_free(d);
    *out = v;
    return n;
}

/* ================= сеть (рабочий поток) ================= */
typedef struct {
    char query[300], ck[320], key[200], api[300], img[300], lang[8];
    int year;
    int state;                 /* TMDB_FOUND / TMDB_NONE */
    char err[100];
    tmdb_result r;
    void (*test_done)(const char *err);
} tjob;

static void urlenc(sbuf *b, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~') sb_addc(b, (char)*p);
        else sb_printf(b, "%%%02X", *p);
    }
}

static void trim_slash(char *s) { size_t n = strlen(s); while (n && s[n - 1] == '/') s[--n] = 0; }

static void search_work(void *p) {
    tjob *j = p;
    int v4 = strlen(j->key) > 40;
    sbuf url = {0}, hdr = {0};
    sb_printf(&url, "%s/search/multi?query=", j->api);
    urlenc(&url, j->query);
    sb_printf(&url, "&language=%s&include_adult=false", j->lang);
    if (!v4) { sb_add(&url, "&api_key="); urlenc(&url, j->key); }
    sb_add(&hdr, "Accept: application/json\r\n");
    if (v4) sb_printf(&hdr, "Authorization: Bearer %s\r\n", j->key);
    hs_req rq = {.url = url.s, .headers = hdr.s, .timeout_ms = 10000, .keepalive = 1};   /* поиски подряд — одно соединение */
    hs *h = hs_open(&rq, j->err, sizeof j->err);
    sbuf body = {0};
    if (h) {
        char buf[16384];
        long n;
        while ((n = hs_read(h, buf, sizeof buf)) > 0 && body.n < (2u << 20)) sb_addn(&body, buf, (size_t)n);
        if (n < 0) snprintf(j->err, sizeof j->err, "%s", hs_error(h));
        hs_close(h);
    }
    j->state = TMDB_NONE;
    if (!j->err[0]) {
        tmdb_hit *hits;
        int n = tmdb_parse(body.s ? body.s : "", body.n, &hits);
        if (n < 0) snprintf(j->err, sizeof j->err, "format");
        int k = n > 0 ? tmdb_pick(hits, n, j->query, j->year) : -1;
        if (k >= 0) {
            tmdb_hit *t = &hits[k];
            j->state = TMDB_FOUND;
            snprintf(j->r.title, sizeof j->r.title, "%s", t->title);
            snprintf(j->r.year, sizeof j->r.year, "%.4s", t->date);
            snprintf(j->r.overview, sizeof j->r.overview, "%s", t->overview);
            if (t->backdrop[0]) snprintf(j->r.backdrop, sizeof j->r.backdrop, "%s/t/p/w780%s", j->img, t->backdrop);
            if (t->poster[0]) snprintf(j->r.poster, sizeof j->r.poster, "%s/t/p/w300%s", j->img, t->poster);
            j->r.vote = round(t->vote * 10) / 10;
            j->r.unique = n == 1;
        }
        free(hits);
    }
    sb_free(&body);
    sb_free(&url);
    sb_free(&hdr);
}

static tjob *make_job(const char *query, int year) {
    tjob *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    snprintf(j->query, sizeof j->query, "%s", query);
    j->year = year;
    snprintf(j->key, sizeof j->key, "%s", P.tmdb_key);
    trim(j->key);
    snprintf(j->api, sizeof j->api, "%s", P.tmdb_api[0] ? P.tmdb_api : TMDB_API_DEFAULT);
    snprintf(j->img, sizeof j->img, "%s", P.tmdb_img[0] ? P.tmdb_img : TMDB_IMG_DEFAULT);
    trim_slash(j->api);
    trim_slash(j->img);
    snprintf(j->lang, sizeof j->lang, "%s", lang_is_ru() ? "ru-RU" : "en-US");
    return j;
}

/* ================= кэш (главный поток) ================= */
#define CACHE 48
typedef struct { char ck[320]; int state; tmdb_result r; } centry;
static centry C[CACHE];
static int cnext;
static tjob *busy;
static char want_title[400], want_desc[600];

int tmdb_enabled(void) {
    for (const char *p = P.tmdb_key; *p; p++) if (*p != ' ') return 1;
    return 0;
}

static centry *find(const char *ck) {
    for (int i = 0; i < CACHE; i++) if (C[i].ck[0] && !strcmp(C[i].ck, ck)) return &C[i];
    return NULL;
}

static void start(const char *title, const char *desc);

static void search_done(void *p) {
    tjob *j = p;
    busy = NULL;
    if (j->err[0]) fprintf(stderr, "tmdb_error=%s\n", strncmp(j->err, "http_", 5) ? "network" : j->err);
    /* ошибка сети — не запоминать (повторится при следующем раскрытии) */
    if (!j->err[0] || !strncmp(j->err, "http_404", 8)) {
        centry *e = &C[cnext];
        cnext = (cnext + 1) % CACHE;
        snprintf(e->ck, sizeof e->ck, "%s", j->ck);
        e->state = j->state;
        e->r = j->r;
    }
    free(j);
    app_dirty();
    if (want_title[0]) start(want_title, want_desc);
}

static void make_key(const char *query, int year, char *ck, size_t cap) {
    char f[300];
    search_fold(query, f, sizeof f);
    snprintf(ck, cap, "%s|%d", f, year);
}

static void start(const char *title, const char *desc) {
    char q[300], ck[320];
    tmdb_query(title, q, sizeof q);
    int y = tmdb_year(title, desc);
    make_key(q, y, ck, sizeof ck);
    if (find(ck)) { want_title[0] = 0; return; }
    if (busy) {   /* одна задача: эту — после текущей */
        snprintf(want_title, sizeof want_title, "%s", title);
        snprintf(want_desc, sizeof want_desc, "%s", desc ? desc : "");
        return;
    }
    want_title[0] = 0;
    tjob *j = make_job(q, y);
    if (!j) return;
    snprintf(j->ck, sizeof j->ck, "%s", ck);
    busy = j;
    pool_submit(search_work, search_done, j);
}

const tmdb_result *tmdb_get(const char *title, const char *desc, int *state) {
    *state = TMDB_OFF;
    if (!tmdb_enabled() || !title || !*title) return NULL;
    char q[300], ck[320], f[300];
    tmdb_query(title, q, sizeof q);
    search_fold(q, f, sizeof f);
    if (strlen(f) < 3) { *state = TMDB_NONE; return NULL; }
    make_key(q, tmdb_year(title, desc), ck, sizeof ck);
    centry *e = find(ck);
    if (e) { *state = e->state; return e->state == TMDB_FOUND ? &e->r : NULL; }
    *state = TMDB_LOADING;
    if (!busy || strcmp(busy->ck, ck)) start(title, desc);
    return NULL;
}

void tmdb_cache_clear(void) { memset(C, 0, sizeof C); cnext = 0; }

static void test_done(void *p) {
    tjob *j = p;
    char msg[160] = "";
    if (!strncmp(j->err, "http_401", 8)) snprintf(msg, sizeof msg, "%s", tr(S_TMDB_BAD_KEY));
    else if (!strncmp(j->err, "http_", 5)) snprintf(msg, sizeof msg, "%s", trf(S_TMDB_HTTP, j->err + 5));
    else if (j->err[0]) snprintf(msg, sizeof msg, "%s", trf(S_TMDB_NO_NET, j->err));
    void (*done)(const char *) = j->test_done;
    free(j);
    if (done) done(msg[0] ? msg : NULL);
}

void tmdb_test(void (*done)(const char *err)) {
    if (!tmdb_enabled()) { if (done) done(tr(S_TMDB_NO_KEY)); return; }
    tjob *j = make_job("ирония судьбы", 1975);
    if (!j) return;
    j->test_done = done;
    tmdb_cache_clear();
    pool_submit(search_work, test_done, j);
}
