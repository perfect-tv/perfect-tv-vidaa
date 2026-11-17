#define _GNU_SOURCE
#include "webvtt.h"
#include "http.h"
#include "util.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define MAX_CUES 3000
#define MAX_SEEN 2000

typedef struct { int64_t s, e; char *t; } cue;
typedef struct job { char url[4096], hdr[2048]; volatile int cancel; int gen; } job;

static struct {
    pthread_mutex_t mu;
    job *j;                 /* текущая задача (NULL — нет) */
    int gen;
    cue *c;
    int n, cap;
    int64_t frame_pts;
    double pos;
} W = {.mu = PTHREAD_MUTEX_INITIALIZER, .frame_pts = -1};

/* ---------- разбор .vtt ---------- */
/* «[чч:]мм:сс.ттт» -> секунды; -1 — не время */
static double vtime(const char *s, const char **end) {
    int part[3] = {0, 0, 0}, np = 0;
    const char *p = s;
    while (np < 3) {
        if (*p < '0' || *p > '9') return -1;
        int v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        part[np++] = v;
        if (*p == ':') { p++; continue; }
        break;
    }
    if (*p != '.' && *p != ',') return -1;
    p++;
    int ms = 0, k = 0;
    while (*p >= '0' && *p <= '9' && k < 3) { ms = ms * 10 + (*p++ - '0'); k++; }
    while (k++ < 3) ms *= 10;
    while (*p >= '0' && *p <= '9') p++;
    if (end) *end = p;
    double h = np == 3 ? part[0] : 0, m = np == 3 ? part[1] : part[0], sec = np == 3 ? part[2] : part[1];
    return h * 3600 + m * 60 + sec + ms / 1000.0;
}

/* теги <…> прочь, сущности — знаками */
static void clean(char *s) {
    char *o = s;
    for (char *p = s; *p;) {
        if (*p == '<') { char *e = strchr(p, '>'); if (e) { p = e + 1; continue; } }
        if (*p == '&') {
            static const struct { const char *e, *v; } ent[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&nbsp;", " "}, {"&quot;", "\""}, {"&apos;", "'"}, {"&lrm;", ""}, {"&rlm;", ""}};
            int done = 0;
            for (size_t i = 0; i < sizeof ent / sizeof ent[0] && !done; i++) {
                size_t l = strlen(ent[i].e);
                if (!strncmp(p, ent[i].e, l)) { size_t vl = strlen(ent[i].v); memmove(o, ent[i].v, vl); o += vl; p += l; done = 1; }
            }
            if (done) continue;
        }
        *o++ = *p++;
    }
    *o = 0;
}

/* Блоки разделены пустыми строками. Первый — заголовок (WEBVTT, X-TIMESTAMP-MAP). В блоке реплики строка с «-->»
 * — время, после неё до пустой строки — текст; строка до неё — необязательный идентификатор. NOTE / STYLE — без
 * времени, пропускаются. */
int webvtt_parse(const char *text, size_t n, webvtt_cue_fn cb, void *ctx) {
    int64_t mpegts = 0;
    double local = 0, a = -1, b = -1;
    int count = 0, header = 1, in_text = 0;
    sbuf body = {0};
    const char *p = text, *end = text + n;
    while (p <= end) {
        const char *e = memchr(p, '\n', (size_t)(end - p));
        if (!e) e = end;
        size_t l = (size_t)(e - p);
        if (l && p[l - 1] == '\r') l--;
        char line[1024];
        if (l >= sizeof line) l = sizeof line - 1;
        memcpy(line, p, l);
        line[l] = 0;
        int blank = !l;
        if (header) {
            char *m = strstr(line, "X-TIMESTAMP-MAP=");
            if (m) {
                char *mp = strstr(m, "MPEGTS:"), *lc = strstr(m, "LOCAL:");
                if (mp) mpegts = strtoll(mp + 7, NULL, 10);
                if (lc) { double v = vtime(lc + 6, NULL); if (v >= 0) local = v; }
            }
            if (blank || strstr(line, "-->")) header = 0;
            if (blank) { p = e + 1; continue; }
        }
        if (blank || e == end) {   /* конец блока: реплика готова */
            if (!blank && in_text) { if (body.n) sb_addc(&body, '\n'); sb_add(&body, line); }
            if (in_text && a >= 0 && b > a && body.s) {
                clean(body.s);
                trim(body.s);
                if (*body.s) {
                    cb(ctx, mpegts + (int64_t)((a - local) * 90000.0), mpegts + (int64_t)((b - local) * 90000.0), body.s);
                    count++;
                }
            }
            sb_clear(&body);
            in_text = 0;
            a = b = -1;
        } else if (!in_text && strstr(line, "-->")) {
            char *q = strstr(line, "-->") + 3;
            while (*q == ' ') q++;
            a = vtime(line, NULL);
            b = vtime(q, NULL);
            in_text = 1;
        } else if (in_text) {
            if (body.n) sb_addc(&body, '\n');
            sb_add(&body, line);
        }
        if (e == end) break;
        p = e + 1;
    }
    sb_free(&body);
    return count;
}

/* ---------- реплики ---------- */
static void add_cue(void *ctx, int64_t s, int64_t e, const char *text) {
    job *j = ctx;
    pthread_mutex_lock(&W.mu);
    if (W.j == j) {
        int dup = 0;
        for (int i = W.n - 1; i >= 0 && i >= W.n - 200 && !dup; i--) dup = W.c[i].s == s && !strcmp(W.c[i].t, text);
        if (!dup) {
            if (W.n == MAX_CUES) {   /* самые старые — прочь */
                free(W.c[0].t);
                memmove(W.c, W.c + 1, (size_t)(W.n - 1) * sizeof *W.c);
                W.n--;
            }
            if (W.n == W.cap) {
                int cap = W.cap ? W.cap * 2 : 256;
                cue *n2 = realloc(W.c, (size_t)cap * sizeof *n2);
                if (n2) { W.c = n2; W.cap = cap; }
            }
            if (W.n < W.cap) W.c[W.n++] = (cue){s, e, strdup(text)};
        }
    }
    pthread_mutex_unlock(&W.mu);
}

static char *get(job *j, const char *url, size_t *len) {
    char err[120] = "";
    hs_req rq = {.url = url, .headers = j->hdr[0] ? j->hdr : NULL, .timeout_ms = 10000, .cancel = &j->cancel, .keepalive = 1};
    hs *h = hs_open(&rq, err, sizeof err);
    if (!h) return NULL;
    sbuf b = {0};
    char buf[16384];
    long r;
    while ((r = hs_read(h, buf, sizeof buf)) > 0 && b.n < (4u << 20)) sb_addn(&b, buf, (size_t)r);
    hs_close(h);
    *len = b.n;
    return r < 0 ? (sb_free(&b), NULL) : sb_take(&b);
}

static void nap(job *j, double s) {
    for (double t = 0; t < s && !j->cancel; t += 0.1) usleep(100000);
}

static void *worker(void *arg) {
    job *j = arg;
    pthread_setname_np(pthread_self(), "ptv-vtt");
    strlist seen = {0};
    while (!j->cancel) {
        size_t n = 0;
        char *pl = get(j, j->url, &n);
        if (!pl) { nap(j, 3); continue; }
        /* плейлист: сегменты с длительностью и номером */
        double target = 6, t0 = 0, pos;
        int endlist = strstr(pl, "#EXT-X-ENDLIST") != NULL;
        pthread_mutex_lock(&W.mu);
        pos = W.pos;
        pthread_mutex_unlock(&W.mu);
        char *tg = strstr(pl, "#EXT-X-TARGETDURATION:");
        if (tg) target = atof(tg + 22);
        struct { char url[4096]; double t, d; } seg[64];
        int ns = 0, total = 0;
        double dur = 0;
        char *save = NULL;
        for (char *line = strtok_r(pl, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char *cr = strchr(line, '\r');
            if (cr) *cr = 0;
            if (!strncmp(line, "#EXTINF:", 8)) { dur = atof(line + 8); continue; }
            if (!*line || *line == '#') continue;
            total++;
            /* запись — сегменты около позиции; эфир — последние */
            int want = endlist ? t0 + dur > pos - 10 && t0 < pos + 60 : 1;
            if (want) {
                if (!endlist && ns == 64) { memmove(seg, seg + 1, sizeof seg[0] * 63); ns--; }
                if (ns < 64) { http_resolve(j->url, line, seg[ns].url, sizeof seg[ns].url); seg[ns].t = t0; seg[ns].d = dur; ns++; }
            }
            t0 += dur;
            dur = 0;
        }
        free(pl);
        int from = endlist ? 0 : (ns > 4 ? ns - 4 : 0), fetched = 0;
        for (int i = from; i < ns && !j->cancel && fetched < 6; i++) {
            if (sl_find(&seen, seg[i].url) >= 0) continue;
            size_t vn = 0;
            char *v = get(j, seg[i].url, &vn);
            if (!v) continue;
            webvtt_parse(v, vn, add_cue, j);
            free(v);
            sl_add(&seen, seg[i].url);
            if (seen.n > MAX_SEEN) sl_remove(&seen, 0);
            fetched++;
        }
        (void)total;
        nap(j, endlist ? 1 : target / 2 > 1 ? target / 2 : 1);
    }
    sl_clear(&seen);
    pthread_mutex_lock(&W.mu);
    if (W.j == j) W.j = NULL;
    pthread_mutex_unlock(&W.mu);
    free(j);
    return NULL;
}

static void clear_cues(void) {
    for (int i = 0; i < W.n; i++) free(W.c[i].t);
    W.n = 0;
}

void webvtt_stop(void) {
    pthread_mutex_lock(&W.mu);
    if (W.j) { W.j->cancel = 1; W.j = NULL; }   /* поток сам выйдет и освободит задачу */
    clear_cues();
    pthread_mutex_unlock(&W.mu);
}

void webvtt_start(const char *url, const char *headers) {
    webvtt_stop();
    job *j = calloc(1, sizeof *j);
    if (!j) return;
    snprintf(j->url, sizeof j->url, "%s", url);
    snprintf(j->hdr, sizeof j->hdr, "%s", headers ? headers : "");
    pthread_mutex_lock(&W.mu);
    W.j = j;
    j->gen = ++W.gen;
    pthread_mutex_unlock(&W.mu);
    pthread_t t;
    if (pthread_create(&t, NULL, worker, j)) {
        pthread_mutex_lock(&W.mu);
        W.j = NULL;
        pthread_mutex_unlock(&W.mu);
        free(j);
        return;
    }
    pthread_detach(t);
    fprintf(stderr, "webvtt_start\n");
}

int webvtt_running(void) { return W.j != NULL; }

void webvtt_clock(int64_t frame_pts, double position) {
    pthread_mutex_lock(&W.mu);
    W.frame_pts = frame_pts;
    W.pos = position;
    pthread_mutex_unlock(&W.mu);
}

size_t webvtt_text_at(int64_t pts, char *out, size_t cap) {
    size_t o = 0;
    if (cap) out[0] = 0;
    pthread_mutex_lock(&W.mu);
    for (int i = 0; i < W.n; i++) {
        cue *c = &W.c[i];
        if (pts < c->s || pts >= c->e) continue;
        size_t l = strlen(c->t);
        if (o + l + 2 >= cap) break;
        if (o) out[o++] = '\n';
        memcpy(out + o, c->t, l);
        o += l;
        out[o] = 0;
    }
    pthread_mutex_unlock(&W.mu);
    return o;
}

int webvtt_cues(void) { return W.n; }
