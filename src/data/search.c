#define _GNU_SOURCE
#include "search.h"
#include "app.h"
#include "net.h"
#include "epg.h"
#include "core/clock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_WORDS 8
#define MAX_HITS 2000
#define RECENT_KEY "search.recent"

typedef struct { uint32_t hash, gen; epg_sq *q; int nq; } jsrc;
typedef struct { int tag; int64_t start, stop; char title[200]; } jhit;
typedef struct {
    jsrc src[MAX_EPG];
    int ns;
    strpool pool;
    char words[MAX_WORDS][64];
    int nw;
    jhit *hits;
    int n, cap;
    volatile int cancel;
    int rev;
} sjob;

static struct {
    char query[200];          /* текст поля */
    char done_q[200];         /* по нему показаны передачи */
    double due;               /* запустить поиск передач в этот момент (0 — не надо) */
    sjob *job;
    channel **ch; int nch, cap_ch;
    search_hit *sec[SR_SECTIONS]; int nsec[SR_SECTIONS];
    int rev;
} S;

static int split_words(const char *q, char w[][64], int max) {
    char f[200];
    search_fold(q, f, sizeof f);
    int n = 0;
    for (char *t = strtok(f, " "); t && n < max; t = strtok(NULL, " ")) snprintf(w[n++], 64, "%s", t);
    return n;
}

static void clear_hits(void) {
    for (int i = 0; i < SR_SECTIONS; i++) { free(S.sec[i]); S.sec[i] = NULL; S.nsec[i] = 0; }
}

static void find_channels(void) {
    S.nch = 0;
    char w[MAX_WORDS][64];
    int nw = split_words(S.query, w, MAX_WORDS);
    if (!nw) return;
    int num = 0, digits = 1;
    for (const char *p = S.query; *p; p++) {
        if (*p >= '0' && *p <= '9') num = num * 10 + (*p - '0');
        else if (*p != ' ') digits = 0;
    }
    char f[400];
    for (int i = 0; i < R.nch; i++) {
        channel *c = &R.ch[i];
        if (repo_is_hidden(c)) continue;
        int ok = digits && num > 0 ? c->num == num : 1;
        if (!(digits && num > 0)) {
            search_fold(c->name, f, sizeof f);
            for (int k = 0; k < nw && ok; k++) if (!strstr(f, w[k])) ok = 0;
        }
        if (!ok) continue;
        if (S.nch == S.cap_ch) {
            int cap = S.cap_ch ? S.cap_ch * 2 : 64;
            channel **n = realloc(S.ch, (size_t)cap * sizeof *n);
            if (!n) return;
            S.ch = n;
            S.cap_ch = cap;
        }
        S.ch[S.nch++] = c;
        if (S.nch >= SEARCH_PER_SECTION) break;
    }
}

/* ---------- задача ---------- */
static void on_hit(void *ctx, int tag, const epg_item *it) {
    sjob *j = ctx;
    if (j->n == j->cap) {
        int cap = j->cap ? j->cap * 2 : 128;
        jhit *n = realloc(j->hits, (size_t)cap * sizeof *n);
        if (!n) return;
        j->hits = n;
        j->cap = cap;
    }
    jhit *h = &j->hits[j->n++];
    h->tag = tag;
    h->start = it->start;
    h->stop = it->stop;
    u8_copy(h->title, sizeof h->title, it->title);
}

static void job_work(void *p) {
    sjob *j = p;
    const char *w[MAX_WORDS];
    for (int i = 0; i < j->nw; i++) w[i] = j->words[i];
    for (int s = 0; s < j->ns && !j->cancel && j->n < MAX_HITS; s++)
        epg_search(j->src[s].hash, j->src[s].gen, j->src[s].q, j->src[s].nq, w, j->nw, MAX_HITS - j->n, on_hit, j, &j->cancel);
}

static void job_free(sjob *j) {
    for (int s = 0; s < j->ns; s++) free(j->src[s].q);
    sp_free(&j->pool);
    free(j->hits);
    free(j);
}

static int cmp_now(const void *a, const void *b) {
    const search_hit *x = a, *y = b;
    return x->c->idx - y->c->idx;
}
static int cmp_soon(const void *a, const void *b) {
    const search_hit *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start ? 1 : x->c->idx - y->c->idx;
}
static int cmp_arc(const void *a, const void *b) { return -cmp_soon(a, b); }

static void start_job(void);

static void job_done(void *p) {
    sjob *j = p;
    S.job = NULL;
    if (!j->cancel && j->rev == R.rev) {
        clear_hits();
        int64_t now = clock_now();
        for (int i = 0; i < SR_SECTIONS; i++) S.sec[i] = calloc((size_t)(j->n + 1), sizeof(search_hit));
        for (int i = 0; i < j->n; i++) {
            jhit *h = &j->hits[i];
            if (h->tag < 0 || h->tag >= R.nch) continue;
            channel *c = &R.ch[h->tag];
            int64_t sh = (int64_t)(c->tvg_shift * 3600);
            int64_t a = h->start + sh, b = h->stop + sh;
            int sec = a <= now && b > now ? SR_NOW : a > now ? SR_SOON : SR_ARCHIVE;
            if (!S.sec[sec]) continue;
            search_hit *o = &S.sec[sec][S.nsec[sec]++];
            o->c = c;
            o->start = a;
            o->stop = b;
            snprintf(o->title, sizeof o->title, "%s", h->title);
        }
        qsort(S.sec[SR_NOW], (size_t)S.nsec[SR_NOW], sizeof(search_hit), cmp_now);
        qsort(S.sec[SR_SOON], (size_t)S.nsec[SR_SOON], sizeof(search_hit), cmp_soon);
        qsort(S.sec[SR_ARCHIVE], (size_t)S.nsec[SR_ARCHIVE], sizeof(search_hit), cmp_arc);
        for (int i = 0; i < SR_SECTIONS; i++) if (S.nsec[i] > SEARCH_PER_SECTION) S.nsec[i] = SEARCH_PER_SECTION;
        S.rev++;
        fprintf(stderr, "search hits=%d now=%d soon=%d archive=%d\n", j->n, S.nsec[0], S.nsec[1], S.nsec[2]);
    } else if (j->rev != R.rev) S.done_q[0] = 0;   /* каналы сменились — повторить */
    job_free(j);
    app_dirty();
    /* набрали дальше, пока искали */
    if (strcmp(S.done_q, S.query) && !S.due) start_job();
}

static void start_job(void) {
    if (S.job) { S.job->cancel = 1; S.due = app_now() + 0.05; return; }   /* дождаться отмены */
    S.due = 0;
    snprintf(S.done_q, sizeof S.done_q, "%s", S.query);
    char f[200];
    search_fold(S.query, f, sizeof f);
    if ((int)strlen(f) < 2 || !R.nch) { clear_hits(); S.rev++; return; }   /* меньше двух букв — передачи не ищем */
    sjob *j = calloc(1, sizeof *j);
    if (!j) return;
    j->nw = split_words(S.query, j->words, MAX_WORDS);
    j->rev = R.rev;
    int64_t now = clock_now();
    for (int s = 0; s < R.nepg; s++) {
        epg_src *e = &R.epg[s];
        if (!epg_has_data(e)) continue;
        jsrc *js = &j->src[j->ns];
        js->hash = e->hash;
        js->gen = e->gen;
        js->q = calloc((size_t)(R.nch + 1), sizeof *js->q);
        if (!js->q) continue;
        for (int i = 0; i < R.nch; i++) {
            channel *c = &R.ch[i];
            if (c->epg_src != s || c->epg_ch < 0 || c->epg_ch >= e->nslots || repo_is_hidden(c)) continue;
            int64_t sh = (int64_t)(c->tvg_shift * 3600);
            epg_sq *q = &js->q[js->nq++];
            q->id = sp_dup(&j->pool, e->slots[c->epg_ch].id);
            q->from = (repo_has_archive(c) ? now - (int64_t)c->archive_days * 86400 : now) - sh;
            q->to = now + 7 * 86400 - sh;
            q->tag = i;
        }
        j->ns++;
    }
    S.job = j;
    pool_submit(job_work, job_done, j);
    app_dirty();
}

/* ---------- API ---------- */
void search_set(const char *query) {
    if (!strcmp(query, S.query)) return;
    snprintf(S.query, sizeof S.query, "%s", query);
    find_channels();
    S.rev++;
    if (S.job) S.job->cancel = 1;
    S.due = app_now() + 0.3;
}

void search_tick(double now) {
    if (S.due && now >= S.due) start_job();
}

void search_clear(void) {
    if (S.job) S.job->cancel = 1;
    S.query[0] = S.done_q[0] = 0;
    S.due = 0;
    S.nch = 0;
    clear_hits();
    S.rev++;
}

int search_busy(void) { return S.job != NULL || S.due != 0; }
int search_rev(void) { return S.rev; }
int search_channels(channel ***out) { *out = S.ch; return S.nch; }
int search_hits(int section, search_hit **out) {
    if (section < 0 || section >= SR_SECTIONS) return 0;
    *out = S.sec[section];
    return S.nsec[section];
}

int search_suggest(char out[][200], int max) {
    int n = 0;
    for (int s = 0; s < SR_SECTIONS && n < max; s++)
        for (int i = 0; i < S.nsec[s] && n < max; i++) {
            int dup = 0;
            for (int k = 0; k < n && !dup; k++) if (!strcmp(out[k], S.sec[s][i].title)) dup = 1;
            if (!dup) snprintf(out[n++], 200, "%s", S.sec[s][i].title);
        }
    return n;
}

int search_recent(char out[][200], int max) {
    strlist l = {0};
    sl_from_lines(&l, store_get(RECENT_KEY));
    int n = 0;
    for (int i = 0; i < l.n && n < max; i++) snprintf(out[n++], 200, "%s", l.v[i]);
    sl_clear(&l);
    return n;
}
static void save_recent(strlist *l) {
    while (l->n > 8) sl_remove(l, l->n - 1);
    char *s = sl_join(l, "\n");
    store_set(RECENT_KEY, s && *s ? s : NULL);
    free(s);
}
void search_remember(const char *q) {
    char t[200];
    snprintf(t, sizeof t, "%s", q);
    trim(t);
    for (char *p = t; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    if (!*t) return;
    strlist l = {0};
    sl_from_lines(&l, store_get(RECENT_KEY));
    int i = sl_find(&l, t);
    if (i >= 0) sl_remove(&l, i);
    sl_insert(&l, 0, t);
    save_recent(&l);
    sl_clear(&l);
}
void search_forget(const char *q) {
    strlist l = {0};
    sl_from_lines(&l, store_get(RECENT_KEY));
    int i = sl_find(&l, q);
    if (i >= 0) { sl_remove(&l, i); save_recent(&l); }
    sl_clear(&l);
}
