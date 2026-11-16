#define _GNU_SOURCE
#include "epg.h"
#include "names.h"
#include "xmltv.h"
#include "core/clock.h"
#include "json.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAGIC 0x45565450u   /* "PTVE" */
#define VERSION 1
typedef struct { uint32_t magic, version, nch, nprog; int64_t created; } hdr;

void epg_paths(uint32_t h, char *idx, char *str, char *meta, size_t cap) {
    char n[40];
    snprintf(n, sizeof n, "epg_%08x.idx", h);
    if (idx) snprintf(idx, cap, "%s", data_path(n));
    snprintf(n, sizeof n, "epg_%08x.str", h);
    if (str) snprintf(str, cap, "%s", data_path(n));
    snprintf(n, sizeof n, "epg_%08x.json", h);
    if (meta) snprintf(meta, cap, "%s", data_path(n));
}

void epg_remove_files(uint32_t h) {
    char a[700], b[700], c[700];
    epg_paths(h, a, b, c, sizeof a);
    unlink(a);
    unlink(b);
    unlink(c);
}

static void *map_file(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    void *p = NULL;
    if (!fstat(fd, &st) && st.st_size > 0) {
        p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) p = NULL;
        else *len = (size_t)st.st_size;
    }
    close(fd);
    return p;
}

void epg_close(epg_src *s) {
    if (s->idx) munmap(s->idx, s->idx_len);
    if (s->str) munmap((void *)s->str, s->str_len);
    s->idx = NULL;
    s->str = NULL;
    s->idx_len = s->str_len = 0;
    s->nch = s->nprog = 0;
    s->ch = NULL;
    s->pr = NULL;
    smap_free(&s->ids);
    smap_free(&s->names);
}

const char *epg_s(const epg_src *s, uint32_t off) { return s->str && off < s->str_len ? s->str + off : ""; }

static void load_meta(epg_src *s) {
    char meta[700];
    epg_paths(s->hash, NULL, NULL, meta, sizeof meta);
    size_t n;
    char *t = file_read(meta, &n);
    if (!t) return;
    json_doc *d = json_parse(t, n);
    free(t);
    if (!d) return;
    json *r = json_root(d);
    s->loaded_at = json_int(json_get(r, "loaded_at"), 0);
    snprintf(s->etag, sizeof s->etag, "%s", json_str(json_get(r, "etag"), ""));
    snprintf(s->last_modified, sizeof s->last_modified, "%s", json_str(json_get(r, "last_modified"), ""));
    snprintf(s->error, sizeof s->error, "%s", json_str(json_get(r, "error"), ""));
    json_free(d);
}

void epg_save_meta(const epg_src *s) {
    char meta[700];
    epg_paths(s->hash, NULL, NULL, meta, sizeof meta);
    sbuf b = {0};
    size_t cap = strlen(s->url) * 6 + 400;
    char *u = malloc(cap), *e = malloc(400), *l = malloc(200), *er = malloc(600);
    if (u && e && l && er) {
        json_escape(u, cap, s->url);
        json_escape(e, 400, s->etag);
        json_escape(l, 200, s->last_modified);
        json_escape(er, 600, s->error);
        sb_printf(&b, "{\"url\":\"%s\",\"loaded_at\":%lld,\"etag\":\"%s\",\"last_modified\":\"%s\",\"error\":\"%s\"}\n", u, (long long)s->loaded_at, e, l, er);
        file_write_atomic(meta, b.s, b.n);
    }
    free(u); free(e); free(l); free(er);
    sb_free(&b);
}

void epg_open(epg_src *s, const char *url) {
    epg_close(s);
    snprintf(s->url, sizeof s->url, "%s", url);
    s->hash = hash_str(url);
    load_meta(s);
    char idx[700], str[700];
    epg_paths(s->hash, idx, str, NULL, sizeof idx);
    s->idx = map_file(idx, &s->idx_len);
    s->str = map_file(str, &s->str_len);
    const hdr *h = s->idx;
    if (!s->idx || !s->str || s->idx_len < sizeof *h || h->magic != MAGIC || h->version != VERSION ||
        sizeof *h + (size_t)h->nch * sizeof(epg_chan) + (size_t)h->nprog * sizeof(epg_prog) > s->idx_len) {
        epg_close(s);
        return;
    }
    s->nch = h->nch;
    s->nprog = h->nprog;
    s->ch = (const epg_chan *)(h + 1);
    s->pr = (const epg_prog *)(s->ch + s->nch);
    s->programmes = (long)s->nprog;
    smap_init(&s->ids, (int)s->nch);
    smap_init(&s->names, (int)s->nch * 2);
    char key[300];
    for (uint32_t i = 0; i < s->nch; i++) {
        smap_set(&s->ids, epg_s(s, s->ch[i].id), (int)i);
        const char *n = epg_s(s, s->ch[i].names);
        while (*n) {
            const char *e = strchr(n, '\n');
            size_t l = e ? (size_t)(e - n) : strlen(n);
            char nm[300];
            if (l >= sizeof nm) l = sizeof nm - 1;
            memcpy(nm, n, l);
            nm[l] = 0;
            names_match_key(nm, key, sizeof key);
            if (key[0] && smap_get(&s->names, key, -1) < 0) smap_set(&s->names, key, (int)i);
            n = e ? e + 1 : n + l;
        }
    }
}

int epg_find_id(const epg_src *s, const char *id) { return id && *id ? smap_get(&s->ids, id, -1) : -1; }
int epg_find_name(const epg_src *s, const char *k) { return k && *k ? smap_get(&s->names, k, -1) : -1; }

/* ================= построение ================= */
typedef struct { uint32_t slot, is_new; epg_prog p; } brec;
typedef struct { uint32_t id, names, icon; char *id_s; } bchan;

#define DEDUP 65536
typedef struct { uint32_t hash, off; char *s; } dent;

struct epg_build {
    uint32_t hash;
    int64_t now, win_end;
    char tmp_str[700], tmp_idx[700];
    FILE *fs;
    uint32_t str_off;
    int io_error;
    bchan *ch; int nch, cap_ch;
    smap ids;                    /* id -> индекс ch */
    /* сопоставление */
    const epg_want *w; int nw;
    int *slot_depth;             /* -1 — канал не нужен, иначе глубина архива (дни) */
    int matched;
    brec *r; size_t nr, cap_r;
    dent *dd;
    xmltv *x;
    const char *langs[2];
    char **names; int names_cap;   /* display-name каналов (через \n) для сопоставления */
};

static uint32_t put_str(epg_build *b, const char *s) {
    if (!s || !*s) return 0;
    size_t n = strlen(s);
    uint32_t h = 0;
    dent *e = NULL;
    if (n <= 160 && b->dd) {
        h = hash_bytes(s, n);
        e = &b->dd[h & (DEDUP - 1)];
        if (e->s && e->hash == h && !strcmp(e->s, s)) return e->off;
    }
    uint32_t off = b->str_off;
    if (fwrite(s, 1, n + 1, b->fs) != n + 1) b->io_error = 1;
    b->str_off += (uint32_t)(n + 1);
    if (e) { free(e->s); e->s = strdup(s); e->hash = h; e->off = off; }
    return off;
}

static void on_channel(void *ctx, const char *id, const char *const *names, int n, const char *icon) {
    epg_build *b = ctx;
    int i = smap_get(&b->ids, id, -1);
    if (i < 0) {
        if (b->nch >= b->cap_ch) {
            int cap = b->cap_ch ? b->cap_ch * 2 : 512;
            bchan *c = realloc(b->ch, (size_t)cap * sizeof *c);
            if (!c) return;
            b->ch = c;
            b->cap_ch = cap;
        }
        i = b->nch++;
        smap_set(&b->ids, id, i);
        b->ch[i].id = put_str(b, id);
        b->ch[i].id_s = strdup(id);
    }
    sbuf nm = {0};
    for (int k = 0; k < n; k++) { if (k) sb_addc(&nm, '\n'); sb_add(&nm, names[k]); }
    b->ch[i].names = put_str(b, nm.s);
    b->ch[i].icon = put_str(b, icon);
    sb_free(&nm);
}

static int wanted(void *ctx, const char *id) {
    epg_build *b = ctx;
    int i = smap_get(&b->ids, id, -1);
    if (i < 0 || !b->slot_depth || i >= b->nch) return -1;
    return b->slot_depth[i] >= 0 ? i : -1;
}

static int accept_prog(void *ctx, int slot, int64_t start, int64_t stop) {
    epg_build *b = ctx;
    int depth = b->slot_depth ? b->slot_depth[slot] : 0;
    int64_t from = day_start(b->now) - (int64_t)(depth + 1) * 86400;
    return stop > from && start < b->win_end;
}

static void on_programme(void *ctx, int slot, int64_t start, int64_t stop, const char *t, const char *su, const char *d, const char *c, const char *ic) {
    epg_build *b = ctx;
    if (!accept_prog(ctx, slot, start, stop)) return;
    if (start < 0 || stop > 0xffffffffLL) return;
    if (b->nr >= b->cap_r) {
        size_t cap = b->cap_r ? b->cap_r * 2 : 65536;
        brec *r = realloc(b->r, cap * sizeof *r);
        if (!r) { b->io_error = 1; return; }
        b->r = r;
        b->cap_r = cap;
    }
    brec *r = &b->r[b->nr++];
    r->slot = (uint32_t)slot;
    r->is_new = 1;
    r->p.start = (uint32_t)start;
    r->p.stop = (uint32_t)stop;
    r->p.title = put_str(b, t);
    r->p.sub = put_str(b, su);
    r->p.desc = put_str(b, d);
    r->p.cat = put_str(b, c);
    r->p.icon = put_str(b, ic);
}

epg_build *epg_build_new(uint32_t hash, const epg_want *w, int nw, int64_t now, const char *lang) {
    epg_build *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->hash = hash;
    b->now = now;
    b->win_end = now + 8 * 86400;
    b->w = w;
    b->nw = nw;
    b->dd = calloc(DEDUP, sizeof *b->dd);
    char idx[700], str[700];
    epg_paths(hash, idx, str, NULL, sizeof idx);
    snprintf(b->tmp_idx, sizeof b->tmp_idx, "%s.new", idx);
    snprintf(b->tmp_str, sizeof b->tmp_str, "%s.new", str);
    b->fs = fopen(b->tmp_str, "wb");
    if (!b->fs) { free(b->dd); free(b); return NULL; }
    setvbuf(b->fs, NULL, _IOFBF, 1 << 16);
    if (fwrite("", 1, 1, b->fs) != 1) b->io_error = 1;   /* смещение 0 — пустая строка */
    b->str_off = 1;
    smap_init(&b->ids, 1024);
    b->langs[0] = lang && *lang ? lang : "ru";
    b->langs[1] = "ru";
    return b;
}

/* ---- имена каналов в памяти для сопоставления ---- */
static void on_channel_nm(void *ctx, const char *id, const char *const *names, int n, const char *icon) {
    epg_build *b = ctx;
    on_channel(ctx, id, names, n, icon);
    int i = smap_get(&b->ids, id, -1);
    if (i < 0) return;
    if (i >= b->names_cap) {
        int cap = i + 1024;
        char **v = realloc(b->names, (size_t)cap * sizeof *v);
        if (!v) return;
        memset(v + b->names_cap, 0, (size_t)(cap - b->names_cap) * sizeof *v);
        b->names = v;
        b->names_cap = cap;
    }
    sbuf s = {0};
    for (int k = 0; k < n; k++) { if (k) sb_addc(&s, '\n'); sb_add(&s, names[k]); }
    free(b->names[i]);
    b->names[i] = sb_take(&s);
}

static void before_nm(void *ctx) {
    epg_build *b = ctx;
    free(b->slot_depth);
    b->slot_depth = malloc((size_t)(b->nch + 1) * sizeof(int));
    if (!b->slot_depth) return;
    for (int i = 0; i < b->nch; i++) b->slot_depth[i] = -1;
    smap nk;
    smap_init(&nk, b->nch * 2 + 16);
    char key[300];
    for (int i = 0; i < b->nch && i < b->names_cap; i++) {
        const char *n = b->names[i];
        while (n && *n) {
            const char *e = strchr(n, '\n');
            size_t l = e ? (size_t)(e - n) : strlen(n);
            char nm[300];
            if (l >= sizeof nm) l = sizeof nm - 1;
            memcpy(nm, n, l);
            nm[l] = 0;
            names_match_key(nm, key, sizeof key);
            if (key[0] && smap_get(&nk, key, -1) < 0) smap_set(&nk, key, i);
            n = e ? e + 1 : n + l;
        }
    }
    b->matched = 0;
    for (int k = 0; k < b->nw; k++) {
        const epg_want *w = &b->w[k];
        int i = w->tvg_id && *w->tvg_id ? smap_get(&b->ids, w->tvg_id, -1) : -1;
        if (i < 0 && w->tvg_name_key && *w->tvg_name_key) i = smap_get(&nk, w->tvg_name_key, -1);
        if (i < 0 && w->name_key && *w->name_key) i = smap_get(&nk, w->name_key, -1);
        if (i < 0) continue;
        b->matched++;
        if (w->archive_days > b->slot_depth[i]) b->slot_depth[i] = w->archive_days;
        else if (b->slot_depth[i] < 0) b->slot_depth[i] = 0;
    }
    smap_free(&nk);
}

void epg_build_push(epg_build *b, const char *data, size_t n) {
    if (!b->x) {
        xmltv_cb cb = {.ctx = b, .on_channel = on_channel_nm, .before_programmes = before_nm, .wanted = wanted,
                       .on_programme = on_programme, .accept = accept_prog, .nlangs = 2};
        cb.langs[0] = b->langs[0];
        cb.langs[1] = b->langs[1];
        b->x = xmltv_new(&cb);
        if (!b->x) { b->io_error = 1; return; }
    }
    xmltv_push(b->x, data, n);
}

long epg_build_programmes(const epg_build *b) { return (long)b->nr; }
long epg_build_matched(const epg_build *b) { return b->matched; }

static int cmp_rec(const void *a, const void *b) {
    const brec *x = a, *y = b;
    if (x->slot != y->slot) return x->slot < y->slot ? -1 : 1;
    if (x->p.start != y->p.start) return x->p.start < y->p.start ? -1 : 1;
    if (x->is_new != y->is_new) return x->is_new > y->is_new ? -1 : 1;   /* новая — первой */
    return 0;
}

/* Перенести прошлое из старой выгрузки: передачи канала до первой новой (архив не теряется). */
static void merge_old(epg_build *b) {
    epg_src old;
    memset(&old, 0, sizeof old);
    char idx[700], str[700];
    epg_paths(b->hash, idx, str, NULL, sizeof idx);
    old.idx = map_file(idx, &old.idx_len);
    old.str = map_file(str, &old.str_len);
    const hdr *h = old.idx;
    if (!old.idx || !old.str || old.idx_len < sizeof *h || h->magic != MAGIC || h->version != VERSION ||
        sizeof *h + (size_t)h->nch * sizeof(epg_chan) + (size_t)h->nprog * sizeof(epg_prog) > old.idx_len) {
        if (old.idx) munmap(old.idx, old.idx_len);
        if (old.str) munmap((void *)old.str, old.str_len);
        return;
    }
    const epg_chan *oc = (const epg_chan *)(h + 1);
    const epg_prog *op = (const epg_prog *)(oc + h->nch);
    /* первая новая передача каждого слота */
    uint32_t *first = malloc((size_t)(b->nch + 1) * sizeof *first);
    if (!first) goto done;
    for (int i = 0; i < b->nch; i++) first[i] = 0xffffffffu;
    for (size_t i = 0; i < b->nr; i++) if (b->r[i].p.start < first[b->r[i].slot]) first[b->r[i].slot] = b->r[i].p.start;
    int64_t today = day_start(b->now);
    size_t added = 0;
    for (uint32_t c = 0; c < h->nch; c++) {
        const char *id = old.str + (oc[c].id < old.str_len ? oc[c].id : 0);
        int slot = smap_get(&b->ids, id, -1);
        if (slot < 0 || !b->slot_depth || b->slot_depth[slot] < 0) continue;
        int64_t from = today - (int64_t)(b->slot_depth[slot] + 1) * 86400;
        uint32_t lim = first[slot] != 0xffffffffu ? first[slot] : (uint32_t)(b->now > 0 ? b->now : 0);
        for (uint32_t k = oc[c].first; k < oc[c].first + oc[c].count && k < h->nprog; k++) {
            const epg_prog *p = &op[k];
            if ((int64_t)p->stop <= from || p->stop > lim) continue;
            if (b->nr >= b->cap_r) {
                size_t cap = b->cap_r ? b->cap_r * 2 : 65536;
                brec *r = realloc(b->r, cap * sizeof *r);
                if (!r) goto done;
                b->r = r;
                b->cap_r = cap;
            }
            brec *r = &b->r[b->nr++];
#define OS(o) ((o) < old.str_len ? old.str + (o) : "")
            r->slot = (uint32_t)slot;
            r->is_new = 0;
            r->p.start = p->start;
            r->p.stop = p->stop;
            r->p.title = put_str(b, OS(p->title));
            r->p.sub = put_str(b, OS(p->sub));
            r->p.desc = put_str(b, OS(p->desc));
            r->p.cat = put_str(b, OS(p->cat));
            r->p.icon = put_str(b, OS(p->icon));
#undef OS
            added++;
        }
    }
    if (added) fprintf(stderr, "epg_merge kept=%zu\n", added);
done:
    free(first);
    munmap(old.idx, old.idx_len);
    munmap((void *)old.str, old.str_len);
}

int epg_build_finish(epg_build *b, char *err, size_t errn) {
    if (b->x) xmltv_end(b->x);
    if (!b->x || !xmltv_channels(b->x)) { snprintf(err, errn, "format"); return -1; }
    if (!b->slot_depth) before_nm(b);   /* каналы без передач */
    merge_old(b);
    qsort(b->r, b->nr, sizeof *b->r, cmp_rec);
    /* повторы и перекрытия: одинаковое начало — остаётся новая; перекрытие — обрезка по следующей */
    size_t w = 0;
    for (size_t i = 0; i < b->nr; i++) {
        if (w && b->r[w - 1].slot == b->r[i].slot && b->r[w - 1].p.start == b->r[i].p.start) continue;
        b->r[w++] = b->r[i];
    }
    b->nr = w;
    w = 0;
    for (size_t i = 0; i < b->nr; i++) {
        brec *r = &b->r[i];
        if (i + 1 < b->nr && b->r[i + 1].slot == r->slot && b->r[i + 1].p.start < r->p.stop) r->p.stop = b->r[i + 1].p.start;
        if (r->p.stop <= r->p.start) continue;
        b->r[w++] = *r;
    }
    b->nr = w;
    if (fflush(b->fs) || b->io_error) { snprintf(err, errn, "write: %s", strerror(errno)); return -1; }
    fclose(b->fs);
    b->fs = NULL;
    FILE *f = fopen(b->tmp_idx, "wb");
    if (!f) { snprintf(err, errn, "write: %s", strerror(errno)); return -1; }
    hdr h = {MAGIC, VERSION, (uint32_t)b->nch, (uint32_t)b->nr, b->now};
    int ok = fwrite(&h, sizeof h, 1, f) == 1;
    size_t k = 0;
    for (int c = 0; c < b->nch && ok; c++) {
        epg_chan ch = {b->ch[c].id, b->ch[c].names, b->ch[c].icon, (uint32_t)k, 0};
        while (k < b->nr && b->r[k].slot < (uint32_t)c) k++;
        ch.first = (uint32_t)k;
        size_t e = k;
        while (e < b->nr && b->r[e].slot == (uint32_t)c) e++;
        ch.count = (uint32_t)(e - k);
        ok = fwrite(&ch, sizeof ch, 1, f) == 1;
    }
    for (size_t i = 0; i < b->nr && ok; i++) ok = fwrite(&b->r[i].p, sizeof b->r[i].p, 1, f) == 1;
    if (fflush(f)) ok = 0;
    fsync(fileno(f));
    fclose(f);
    if (!ok) { snprintf(err, errn, "write"); unlink(b->tmp_idx); return -1; }
    char idx[700], str[700];
    epg_paths(b->hash, idx, str, NULL, sizeof idx);
    if (rename(b->tmp_str, str) || rename(b->tmp_idx, idx)) { snprintf(err, errn, "rename: %s", strerror(errno)); return -1; }
    fprintf(stderr, "epg_build channels=%d matched=%d programmes=%zu parsed=%ld skipped=%ld\n", b->nch, b->matched, b->nr,
            xmltv_programmes(b->x), xmltv_skipped(b->x));
    return 0;
}

void epg_build_free(epg_build *b) {
    if (!b) return;
    if (b->fs) { fclose(b->fs); unlink(b->tmp_str); }
    xmltv_free(b->x);
    for (int i = 0; i < b->nch; i++) free(b->ch[i].id_s);
    free(b->ch);
    smap_free(&b->ids);
    free(b->slot_depth);
    free(b->r);
    if (b->dd) { for (int i = 0; i < DEDUP; i++) free(b->dd[i].s); free(b->dd); }
    for (int i = 0; i < b->names_cap; i++) free(b->names[i]);
    free(b->names);
    free(b);
}
