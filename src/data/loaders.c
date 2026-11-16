#define _GNU_SOURCE
#include "loaders.h"
#include "app.h"
#include "net.h"
#include "epg.h"
#include "m3u.h"
#include "names.h"
#include "repo.h"
#include "core/clock.h"
#include "core/http.h"
#include "core/lang.h"
#include "core/prefs.h"
#include "core/unpack.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void (*loaders_on_playlist)(int, int, const char *);
void (*loaders_on_epg)(int, const char *);

static volatile int cancel_all;

/* чтение HTTP для распаковщика */
static long hs_rd(void *ctx, void *buf, size_t n) { return hs_read(ctx, buf, n); }

const char *loaders_error_text(const char *e) {
    if (!e || !*e) return "";
    if (!strcmp(e, "no_url")) return tr(S_ERR_NO_URL);
    if (!strcmp(e, "format")) return tr(S_ERR_FORMAT);
    if (!strcmp(e, "empty")) return tr(S_ERR_EMPTY);
    if (!strcmp(e, "xz")) return tr(S_ERR_XZ);
    if (!strncmp(e, "http_", 5)) return trf(S_ERR_HTTP, e + 5);
    if (!strncmp(e, "write", 5)) return trf(S_ERR_WRITE, e);
    return trf(S_ERR_NETWORK, e);
}

/* ================= плейлист ================= */
typedef struct {
    int pl_id, index;
    char url[2048], etag[160], last_modified[64];
    char tmp[700], dst[700];
    FILE *out;
    atomic_long channels;
    atomic_llong got, total;
    char err[120];
    char *header_epg;
    char new_etag[160], new_lm[64];
    int not_modified, ok;
    volatile int cancel;
} pl_job;

static pl_job *pl_busy;

static void esc(FILE *f, const char *s) {
    for (; s && *s; s++) {
        if (*s == '\t') fputs("\\t", f);
        else if (*s == '\n') fputs("\\n", f);
        else if (*s == '\r') continue;
        else if (*s == '\\') fputs("\\\\", f);
        else fputc(*s, f);
    }
}
static void on_m3u(const m3u_channel *c, void *ctx) {
    pl_job *j = ctx;
    FILE *f = j->out;
    fprintf(f, "%d\t", c->num);
    esc(f, c->name); fputc('\t', f);
    esc(f, c->url); fputc('\t', f);
    esc(f, c->group); fputc('\t', f);
    esc(f, c->logo); fputc('\t', f);
    esc(f, c->tvg_id); fputc('\t', f);
    esc(f, c->tvg_name); fputc('\t', f);
    fprintf(f, "%g\t%d\t", c->tvg_shift, c->has_catchup);
    esc(f, c->has_catchup ? c->cu_type : ""); fputc('\t', f);
    esc(f, c->has_catchup ? c->cu_source : ""); fputc('\t', f);
    fprintf(f, "%d\t", c->has_catchup ? c->cu_days : 0);
    esc(f, c->user_agent); fputc('\t', f);
    esc(f, c->referrer); fputc('\t', f);
    esc(f, c->headers_json);
    fputc('\n', f);
    atomic_store(&j->channels, c->num);
}

static void pl_work(void *p) {
    pl_job *j = p;
    if (!j->url[0]) { snprintf(j->err, sizeof j->err, "no_url"); return; }
    hs_req rq = {.url = j->url, .etag = j->etag, .last_modified = j->last_modified, .cancel = &j->cancel};
    char e[120] = "";
    hs *h = hs_open(&rq, e, sizeof e);
    if (!h) { snprintf(j->err, sizeof j->err, "%s", e); return; }
    if (hs_status(h) == 304) { j->not_modified = 1; j->ok = 1; hs_close(h); return; }
    atomic_store(&j->total, hs_length(h));
    snprintf(j->new_etag, sizeof j->new_etag, "%s", hs_etag(h));
    snprintf(j->new_lm, sizeof j->new_lm, "%s", hs_last_modified(h));
    up *u = up_new(hs_rd, h);
    j->out = fopen(j->tmp, "wb");
    if (!u || !j->out) { snprintf(j->err, sizeof j->err, "write"); goto out; }
    m3u *m = m3u_new(on_m3u, j);
    char buf[32768];
    long r;
    while ((r = up_read(u, buf, sizeof buf)) > 0) {
        m3u_push(m, buf, (size_t)r);
        atomic_store(&j->got, hs_received(h));
        if (m3u_rejected(m)) break;
    }
    if (r < 0) snprintf(j->err, sizeof j->err, "%s", !strcmp(up_error(u), "format") ? "xz" : *hs_error(h) ? hs_error(h) : up_error(u));
    m3u_end(m);
    if (!j->err[0]) {
        if (m3u_rejected(m) || !m3u_is_m3u(m)) snprintf(j->err, sizeof j->err, "format");
        else if (!m3u_count(m)) snprintf(j->err, sizeof j->err, "empty");
    }
    j->header_epg = strdup(m3u_epg_urls(m));
    m3u_free(m);
    if (fflush(j->out) || ferror(j->out)) snprintf(j->err, sizeof j->err, "write");
    fclose(j->out);
    j->out = NULL;
    /* оборванный, но непустой плейлист лучше старого? Нет: старый список остаётся до успешной загрузки */
    if (!j->err[0] && rename(j->tmp, j->dst)) snprintf(j->err, sizeof j->err, "write");
    if (!j->err[0]) j->ok = 1;
out:
    if (j->out) fclose(j->out);
    unlink(j->tmp);
    up_free(u);
    hs_close(h);
}

static void pl_done(void *p) {
    pl_job *j = p;
    pl_busy = NULL;
    int idx = -1;
    for (int i = 0; i < R.npl; i++) if (R.pl[i].id == j->pl_id) idx = i;
    if (idx >= 0) {
        playlist *pl = &R.pl[idx];
        if (j->ok) {
            pl->loaded_at = clock_now();
            pl->error[0] = 0;
            if (!j->not_modified) {
                pl->count = (int)atomic_load(&j->channels);
                snprintf(pl->etag, sizeof pl->etag, "%s", j->new_etag);
                snprintf(pl->last_modified, sizeof pl->last_modified, "%s", j->new_lm);
                free(pl->header_epg);
                pl->header_epg = j->header_epg ? j->header_epg : strdup("");
                j->header_epg = NULL;
            }
        } else if (!j->cancel) snprintf(pl->error, sizeof pl->error, "%s", j->err);
        repo_save_playlists();
    }
    fprintf(stderr, "playlist_load id=%d ok=%d not_modified=%d channels=%ld err=%s\n", j->pl_id, j->ok, j->not_modified, (long)atomic_load(&j->channels), j->err);
    if (loaders_on_playlist && !j->cancel) loaders_on_playlist(idx, j->ok, j->err);
    free(j->header_epg);
    free(j);
    app_dirty();
}

void load_playlist(int index, int force) {
    if (index < 0 || index >= R.npl || pl_busy) return;
    playlist *p = &R.pl[index];
    pl_job *j = calloc(1, sizeof *j);
    if (!j) return;
    j->pl_id = p->id;
    j->index = index;
    snprintf(j->url, sizeof j->url, "%s", p->url);
    /* условный запрос — только если файл каналов на месте */
    repo_channels_path(p->id, j->dst, sizeof j->dst);
    if (!force && file_exists(j->dst)) {
        snprintf(j->etag, sizeof j->etag, "%s", p->etag);
        snprintf(j->last_modified, sizeof j->last_modified, "%s", p->last_modified);
    }
    snprintf(j->tmp, sizeof j->tmp, "%s.new", j->dst);
    pl_busy = j;
    pool_submit(pl_work, pl_done, j);
    app_dirty();
}
int loaders_playlist_busy(void) { return pl_busy != NULL; }
int loaders_playlist_progress(int *channels) {
    if (!pl_busy) { if (channels) *channels = 0; return -1; }
    if (channels) *channels = (int)atomic_load(&pl_busy->channels);
    long long t = atomic_load(&pl_busy->total), g = atomic_load(&pl_busy->got);
    return t > 0 ? (int)(g * 100 / t) : -1;
}

/* ================= программа ================= */
typedef struct {
    int src;                      /* индекс в R.epg */
    int pl_id;
    uint32_t hash;
    char url[2048], etag[160], last_modified[64];
    epg_want *w; int nw;
    strpool pool;
    char lang[4];
    int64_t now;
    char err[120], new_etag[160], new_lm[64];
    int ok, not_modified;
    atomic_llong got, total;
    atomic_long progs;
    volatile int cancel;
} epg_job;

static epg_job *epg_busy;
static int epg_queue[MAX_EPG], epg_nqueue, epg_force;

static void epg_work(void *p) {
    epg_job *j = p;
    hs_req rq = {.url = j->url, .etag = j->etag, .last_modified = j->last_modified, .cancel = &j->cancel};
    char e[120] = "";
    hs *h = hs_open(&rq, e, sizeof e);
    if (!h) { snprintf(j->err, sizeof j->err, "%s", e); return; }
    if (hs_status(h) == 304) { j->not_modified = 1; j->ok = 1; hs_close(h); return; }
    atomic_store(&j->total, hs_length(h));
    snprintf(j->new_etag, sizeof j->new_etag, "%s", hs_etag(h));
    snprintf(j->new_lm, sizeof j->new_lm, "%s", hs_last_modified(h));
    up *u = up_new(hs_rd, h);
    epg_build *b = epg_build_new(j->hash, j->w, j->nw, j->now, j->lang);
    if (!u || !b) { snprintf(j->err, sizeof j->err, "write"); goto out; }
    char *buf = malloc(1 << 16);
    long r = -1;
    while (buf && (r = up_read(u, buf, 1 << 16)) > 0) {
        epg_build_push(b, buf, (size_t)r);
        atomic_store(&j->got, hs_received(h));
        atomic_store(&j->progs, epg_build_programmes(b));
    }
    free(buf);
    if (r < 0) snprintf(j->err, sizeof j->err, "%s", !strcmp(up_error(u), "format") ? "xz" : *hs_error(h) ? hs_error(h) : up_error(u));
    /* оборванный поток: то, что успели разобрать, всё равно пишется (старое прошлое переносится) */
    char fe[120] = "";
    if (epg_build_finish(b, fe, sizeof fe)) { if (!j->err[0]) snprintf(j->err, sizeof j->err, "%s", fe); }
    else if (!j->err[0]) j->ok = 1;
out:
    epg_build_free(b);
    up_free(u);
    hs_close(h);
}

static void epg_next(void);

static void epg_done(void *p) {
    epg_job *j = p;
    epg_busy = NULL;
    playlist *pl = repo_cur();
    int same = pl && pl->id == j->pl_id && j->src < R.nepg && !strcmp(R.epg[j->src].url, j->url);
    fprintf(stderr, "epg_load src=%d ok=%d not_modified=%d programmes=%ld err=%s\n", j->src, j->ok, j->not_modified, (long)atomic_load(&j->progs), j->err);
    if (same) {
        epg_src *s = &R.epg[j->src];
        s->loading = 0;
        if (j->ok) {
            s->loaded_at = clock_now();
            s->error[0] = 0;
            if (!j->not_modified) {
                snprintf(s->etag, sizeof s->etag, "%s", j->new_etag);
                snprintf(s->last_modified, sizeof s->last_modified, "%s", j->new_lm);
            }
        } else if (!j->cancel) snprintf(s->error, sizeof s->error, "%s", j->err);
        epg_save_meta(s);
        if (j->ok && !j->not_modified) repo_reopen_epg(j->src);

    }
    int last = epg_nqueue == 0;
    if (loaders_on_epg && same && !j->cancel && last) loaders_on_epg(j->ok, j->err);
    sp_free(&j->pool);
    free(j->w);
    free(j);
    app_dirty();
    epg_next();
}

static void epg_start(int src) {
    playlist *pl = repo_cur();
    if (!pl || src >= R.nepg) return;
    epg_job *j = calloc(1, sizeof *j);
    if (!j) return;
    epg_src *s = &R.epg[src];
    j->src = src;
    j->pl_id = pl->id;
    j->hash = s->hash;
    j->now = clock_now();
    snprintf(j->url, sizeof j->url, "%s", s->url);
    if (!epg_force && s->idx) {
        snprintf(j->etag, sizeof j->etag, "%s", s->etag);
        snprintf(j->last_modified, sizeof j->last_modified, "%s", s->last_modified);
    }
    snprintf(j->lang, sizeof j->lang, "%s", lang_is_ru() ? "ru" : "en");
    /* снимок каналов: в потоке нельзя трогать R */
    j->w = calloc((size_t)(R.nch ? R.nch : 1), sizeof *j->w);
    for (int i = 0; i < R.nch && j->w; i++) {
        channel *c = &R.ch[i];
        epg_want *w = &j->w[j->nw++];
        w->tvg_id = sp_dup(&j->pool, c->tvg_id);
        w->tvg_name_key = sp_dup(&j->pool, c->tkey);
        w->name_key = sp_dup(&j->pool, c->mkey);
        w->archive_days = c->cu_type ? c->archive_days : 0;
    }
    s->loading = 1;
    epg_busy = j;
    pool_submit(epg_work, epg_done, j);   /* не в «низкой» полосе: она для логотипов */
}

static void epg_next(void) {
    if (epg_busy || !epg_nqueue) { if (!epg_nqueue) epg_force = 0; return; }
    int src = epg_queue[0];
    memmove(epg_queue, epg_queue + 1, (size_t)(--epg_nqueue) * sizeof epg_queue[0]);
    epg_start(src);
}

void load_epg(int force) {
    if (force) epg_force = 1;
    for (int i = 0; i < R.nepg; i++) {
        epg_src *s = &R.epg[i];
        int due = force || !s->idx || clock_now() - s->loaded_at > (int64_t)P.epg_hours * 3600;
        if (!due) continue;
        int queued = s->loading;
        for (int k = 0; k < epg_nqueue; k++) if (epg_queue[k] == i) queued = 1;
        if (!queued && epg_nqueue < MAX_EPG) { epg_queue[epg_nqueue++] = i; s->loading = 1; }
    }
    epg_next();
    app_dirty();
}

int loaders_epg_progress(long *programmes) {
    if (!epg_busy) { if (programmes) *programmes = 0; return -1; }
    if (programmes) *programmes = (long)atomic_load(&epg_busy->progs);
    long long t = atomic_load(&epg_busy->total), g = atomic_load(&epg_busy->got);
    return t > 0 ? (int)(g * 100 / t) : -1;
}

void loaders_cancel(void) {
    cancel_all = 1;
    if (pl_busy) pl_busy->cancel = 1;
    if (epg_busy) epg_busy->cancel = 1;
    epg_nqueue = 0;
    for (int i = 0; i < R.nepg; i++) R.epg[i].loading = 0;
    cancel_all = 0;
}

void loaders_tick(void) {
    static double next;
    double t = app_now();
    if (t < next) return;
    next = t + 60;
    playlist *p = repo_cur();
    if (!p) return;
    if (!pl_busy && (p->loaded_at == 0 || clock_now() - p->loaded_at > (int64_t)P.playlist_hours * 3600)) { load_playlist(R.cur, 0); return; }
    if (!pl_busy) load_epg(0);
}
