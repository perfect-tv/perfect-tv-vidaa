#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64   /* d<gen>.bin бывает больше 2 ГБ */
#include "epg.h"
#include "names.h"
#include "xmltv.h"
#include "core/clock.h"
#include "json.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define IMAGIC 0x49565450u   /* "PTVI" */
#define IVERSION 2
#define REC 28               /* запись передачи в куске: 7 × u32 */
#define MAX_BUCKETS 256
#define PER_BUCKET 32        /* каналов на корзину (столько разбирается за раз) */

/* На диске — только u32 (порядок байтов ТВ и ПК один — little-endian). */
typedef struct { uint32_t magic, version, nb, nch, nprog, created_lo, created_hi, reserved; } ihdr;
/* кусок: начало первой передачи, конец последней, передач, длина сжатого и распакованного, смещение в d<gen>.bin */
typedef struct { uint32_t start0, stop1, count, clen, rlen, first; uint64_t off; } ck;
typedef struct { char *id; uint32_t total; int n; ck *c; } ech;
typedef struct { int loaded, n; ech *ch; smap ids; } ebk;
struct epg_store {
    int fi, fd;
    uint32_t hash, gen, nb, nprog;
    uint32_t *doff, *dlen;
    ebk *bk;
};

/* ================= пути и метаданные ================= */
void epg_dir(uint32_t h, char *out, size_t cap) {
    char n[40];
    snprintf(n, sizeof n, "epg/%08x", h);
    snprintf(out, cap, "%s", data_path(n));
}
static void gen_path(uint32_t h, char kind, uint32_t gen, const char *ext, char *out, size_t cap) {
    char d[700];
    epg_dir(h, d, sizeof d);
    snprintf(out, cap, "%s/%c%u.%s", d, kind, gen, ext);
}
static void meta_path(uint32_t h, char *out, size_t cap) {
    char d[700];
    epg_dir(h, d, sizeof d);
    snprintf(out, cap, "%s/meta.json", d);
}
static int make_dir(uint32_t h) {
    mkdir(data_path("epg"), 0755);
    char d[700];
    epg_dir(h, d, sizeof d);
    return mkdir(d, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static void rm_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char p[1000];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        unlink(p);
    }
    closedir(d);
    rmdir(dir);
}
void epg_remove_files(uint32_t h) {
    char d[700];
    epg_dir(h, d, sizeof d);
    rm_dir(d);
}

/* «c12.tsv» -> 12 и расширение; 0 — не файл выгрузки */
static uint32_t file_gen(const char *n, const char **ext) {
    if ((n[0] != 'c' && n[0] != 'd' && n[0] != 'i') || n[1] < '0' || n[1] > '9') return 0;
    char *e;
    unsigned long g = strtoul(n + 1, &e, 10);
    if (*e != '.') return 0;
    *ext = e + 1;
    return (uint32_t)g;
}
void epg_prune(uint32_t h, uint32_t keep) {
    char dir[700], p[1000];
    epg_dir(h, dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *ext = "";
        uint32_t g = file_gen(e->d_name, &ext);
        if (!g) continue;
        if (g == keep && strcmp(ext, "new")) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        unlink(p);
    }
    closedir(d);
}

static void load_meta(epg_src *s) {
    char meta[700];
    meta_path(s->hash, meta, sizeof meta);
    size_t n;
    char *t = file_read(meta, &n);
    if (!t) return;
    json_doc *d = json_parse(t, n);
    free(t);
    if (!d) return;
    json *r = json_root(d);
    s->gen = (uint32_t)json_int(json_get(r, "gen"), 0);
    s->loaded_at = json_int(json_get(r, "loaded_at"), 0);
    s->want_hash = (uint32_t)json_int(json_get(r, "want_hash"), 0);
    s->users_sig = (uint32_t)json_int(json_get(r, "users_sig"), 0);
    s->programmes = (long)json_int(json_get(r, "programmes"), 0);
    snprintf(s->etag, sizeof s->etag, "%s", json_str(json_get(r, "etag"), ""));
    snprintf(s->last_modified, sizeof s->last_modified, "%s", json_str(json_get(r, "last_modified"), ""));
    snprintf(s->error, sizeof s->error, "%s", json_str(json_get(r, "error"), ""));
    json_free(d);
}

void epg_save_meta(const epg_src *s) {
    if (make_dir(s->hash)) return;
    char meta[700];
    meta_path(s->hash, meta, sizeof meta);
    sbuf b = {0};
    size_t cap = strlen(s->url) * 6 + 400;
    char *u = malloc(cap), *e = malloc(1000), *l = malloc(400), *er = malloc(600);
    if (u && e && l && er) {
        json_escape(u, cap, s->url);
        json_escape(e, 1000, s->etag);
        json_escape(l, 400, s->last_modified);
        json_escape(er, 600, s->error);
        sb_printf(&b, "{\"url\":\"%s\",\"gen\":%u,\"loaded_at\":%lld,\"want_hash\":%u,\"users_sig\":%u,\"programmes\":%ld", u,
                  s->gen, (long long)s->loaded_at, s->want_hash, s->users_sig, s->programmes);
        sb_printf(&b, ",\"etag\":\"%s\",\"last_modified\":\"%s\",\"error\":\"%s\"}\n", e, l, er);
        file_write_atomic(meta, b.s, b.n);
    }
    free(u); free(e); free(l); free(er);
    sb_free(&b);
}

/* Папки источников, которых нет в keep, и файлы прежнего формата (epg_<хэш>.idx/.str/.json); у оставшихся —
 * только текущая выгрузка. Вызывать, когда программа не загружается. */
void epg_gc(const uint32_t *keep, int n) {
    char p[1000];
    DIR *d = opendir(data_path("."));
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strncmp(e->d_name, "epg_", 4)) continue;
            snprintf(p, sizeof p, "%s", data_path(e->d_name));
            unlink(p);
        }
        closedir(d);
    }
    char root[700];
    snprintf(root, sizeof root, "%s", data_path("epg"));
    d = opendir(root);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        char *end;
        if (strlen(e->d_name) != 8) continue;
        uint32_t h = (uint32_t)strtoul(e->d_name, &end, 16);
        if (*end) continue;
        int used = 0;
        for (int i = 0; i < n; i++) if (keep[i] == h) used = 1;
        if (!used) {
            snprintf(p, sizeof p, "%s/%s", root, e->d_name);
            rm_dir(p);
            fprintf(stderr, "epg_gc removed=%08x\n", h);
            continue;
        }
        epg_src s;
        memset(&s, 0, sizeof s);
        s.hash = h;
        load_meta(&s);
        epg_prune(h, s.gen);
    }
    closedir(d);
}

/* ================= выгрузка: оглавление ================= */
static int pread_all(int fd, void *buf, size_t n, uint64_t off) {
    char *p = buf;
    while (n) {
        ssize_t r = pread(fd, p, n, (off_t)off);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
        p += r;
        n -= (size_t)r;
        off += (uint64_t)r;
    }
    return 0;
}

static void store_close(epg_store *st) {
    if (!st) return;
    if (st->fi >= 0) close(st->fi);
    if (st->fd >= 0) close(st->fd);
    for (uint32_t b = 0; st->bk && b < st->nb; b++) {
        ebk *k = &st->bk[b];
        for (int i = 0; i < k->n; i++) { free(k->ch[i].id); free(k->ch[i].c); }
        free(k->ch);
        if (k->loaded) smap_free(&k->ids);
    }
    free(st->bk);
    free(st->doff);
    free(st->dlen);
    free(st);
}

static epg_store *store_open(uint32_t h, uint32_t gen) {
    if (!gen) return NULL;
    char pi[800], pd[800];
    gen_path(h, 'i', gen, "bin", pi, sizeof pi);
    gen_path(h, 'd', gen, "bin", pd, sizeof pd);
    epg_store *st = calloc(1, sizeof *st);
    if (!st) return NULL;
    st->hash = h;
    st->gen = gen;
    st->fi = open(pi, O_RDONLY | O_CLOEXEC);
    st->fd = open(pd, O_RDONLY | O_CLOEXEC);
    ihdr hd;
    if (st->fi < 0 || st->fd < 0 || pread_all(st->fi, &hd, sizeof hd, 0) || hd.magic != IMAGIC || hd.version != IVERSION ||
        !hd.nb || hd.nb > MAX_BUCKETS || (hd.nb & (hd.nb - 1))) {
        store_close(st);
        return NULL;
    }
    st->nb = hd.nb;
    st->nprog = hd.nprog;
    st->doff = malloc(hd.nb * sizeof(uint32_t));
    st->dlen = malloc(hd.nb * sizeof(uint32_t));
    st->bk = calloc(hd.nb, sizeof *st->bk);
    uint32_t *dir = malloc(hd.nb * 8);
    if (!st->doff || !st->dlen || !st->bk || !dir || pread_all(st->fi, dir, hd.nb * 8, sizeof hd)) {
        free(dir);
        store_close(st);
        return NULL;
    }
    for (uint32_t b = 0; b < hd.nb; b++) { st->doff[b] = dir[b * 2]; st->dlen[b] = dir[b * 2 + 1]; }
    free(dir);
    return st;
}

static uint32_t rd32(const unsigned char **p, const unsigned char *end, int *bad) {
    uint32_t v = 0;
    if (*p + 4 > end) { *bad = 1; return 0; }
    memcpy(&v, *p, 4);
    *p += 4;
    return v;
}

/* Корзина оглавления — при первом обращении (у корзины десятки каналов: читается и разбирается за доли мс). */
static ebk *bucket(epg_store *st, uint32_t b) {
    ebk *k = &st->bk[b];
    if (k->loaded) return k;
    k->loaded = 1;
    smap_init(&k->ids, PER_BUCKET);
    uint32_t len = st->dlen[b];
    if (!len) return k;
    unsigned char *buf = malloc(len);
    if (!buf || pread_all(st->fi, buf, len, st->doff[b])) { free(buf); return k; }
    const unsigned char *p = buf, *end = buf + len;
    int bad = 0;
    uint32_t n = rd32(&p, end, &bad);
    if (!bad && n <= len / 12) k->ch = calloc(n ? n : 1, sizeof *k->ch);
    for (uint32_t i = 0; k->ch && i < n && !bad; i++) {
        uint32_t il = rd32(&p, end, &bad);
        if (bad || il > (size_t)(end - p)) break;
        char *id = xstrndup((const char *)p, il);
        p += il;
        uint32_t nc = rd32(&p, end, &bad), total = rd32(&p, end, &bad);
        if (bad || !id || (size_t)(end - p) / REC < nc) { free(id); break; }
        ech *e = &k->ch[k->n];
        e->id = id;
        e->c = malloc((nc ? nc : 1) * sizeof *e->c);
        if (!e->c) { free(id); break; }
        uint32_t first = 0;
        for (uint32_t j = 0; j < nc; j++) {
            ck *c = &e->c[j];
            c->start0 = rd32(&p, end, &bad);
            c->stop1 = rd32(&p, end, &bad);
            c->count = rd32(&p, end, &bad);
            c->clen = rd32(&p, end, &bad);
            c->rlen = rd32(&p, end, &bad);
            uint32_t lo = rd32(&p, end, &bad), hi = rd32(&p, end, &bad);
            c->off = (uint64_t)hi << 32 | lo;
            c->first = first;
            first += c->count;
        }
        e->n = (int)nc;
        e->total = first < total ? first : total;
        smap_set(&k->ids, id, k->n++);
    }
    free(buf);
    return k;
}

static ech *find_ch(epg_store *st, const char *id) {
    if (!st || !id || !*id) return NULL;
    ebk *k = bucket(st, hash_str(id) & (st->nb - 1));
    int i = smap_get(&k->ids, id, -1);
    return i >= 0 ? &k->ch[i] : NULL;
}

/* Кусок с диска: распакованный буфер (последний байт — 0) и число записей; NULL — битый. */
static unsigned char *read_chunk(int fd, const ck *c, uint32_t *n) {
    *n = 0;
    if (!c->clen || c->rlen < (uint64_t)c->count * REC + 1 || c->rlen > (64u << 20)) return NULL;
    unsigned char *z = malloc(c->clen), *raw = malloc(c->rlen);
    if (!z || !raw || pread_all(fd, z, c->clen, c->off)) { free(z); free(raw); return NULL; }
    uLongf rl = c->rlen;
    int rc = uncompress(raw, &rl, z, c->clen);
    free(z);
    if (rc != Z_OK || rl != c->rlen) { free(raw); return NULL; }
    raw[c->rlen - 1] = 0;
    *n = c->count;
    return raw;
}

static void rec_at(const unsigned char *raw, uint32_t n, uint32_t rlen, uint32_t j, epg_item *out) {
    uint32_t r[7];
    memcpy(r, raw + (size_t)j * REC, REC);
    const char *base = (const char *)raw + (size_t)n * REC;
    uint32_t sl = rlen - n * REC;
#define S(o) ((o) < sl ? base + (o) : "")
    out->start = r[0];
    out->stop = r[1];
    out->title = S(r[2]);
    out->sub = S(r[3]);
    out->desc = S(r[4]);
    out->cat = S(r[5]);
    out->icon = S(r[6]);
#undef S
}

/* ================= кэш кусков (главный поток) ================= */
typedef struct { uint32_t hash, gen; uint64_t off; unsigned char *raw; uint32_t rlen, n; unsigned frame; } cent;
static cent *C;
static int nC, capC;
static unsigned FRAME = 2;
static long cbytes;
#define CACHE_SOFT 96   /* кусков: ~2–5 МБ; больше — только если все нужны в этом кадре */

void epg_frame(void) { FRAME++; }
long epg_cache_bytes(void) { return cbytes; }
void epg_cache_free(void) {
    for (int i = 0; i < nC; i++) free(C[i].raw);
    free(C);
    C = NULL;
    nC = capC = 0;
    cbytes = 0;
}

static cent *chunk(epg_store *st, const ck *c) {
    static int last;
    if (last < nC && C[last].off == c->off && C[last].gen == st->gen && C[last].hash == st->hash) {
        C[last].frame = FRAME;
        return &C[last];
    }
    for (int i = 0; i < nC; i++)
        if (C[i].off == c->off && C[i].gen == st->gen && C[i].hash == st->hash) { C[i].frame = FRAME; last = i; return &C[i]; }
    uint32_t n;
    unsigned char *raw = read_chunk(st->fd, c, &n);
    int at = -1;
    if (nC >= CACHE_SOFT) {
        unsigned best = FRAME - 1;
        for (int i = 0; i < nC; i++) if (C[i].frame < best) { best = C[i].frame; at = i; }
    }
    if (at < 0) {
        if (nC == capC) {
            int cap = capC ? capC * 2 : 64;
            cent *g = realloc(C, (size_t)cap * sizeof *g);
            if (!g) { free(raw); return NULL; }
            C = g;
            capC = cap;
        }
        at = nC++;
    } else {
        free(C[at].raw);
        cbytes -= C[at].rlen;
    }
    C[at] = (cent){st->hash, st->gen, c->off, raw, raw ? c->rlen : 0, n, FRAME};
    cbytes += C[at].rlen;
    last = at;
    return &C[at];
}

/* ================= выгрузка: чтение ================= */
void epg_slots_clear(epg_src *s) {
    for (int i = 0; i < s->nslots; i++) { free(s->slots[i].id); free(s->slots[i].icon); }
    free(s->slots);
    s->slots = NULL;
    s->nslots = s->cap_slots = 0;
}
int epg_slot_add(epg_src *s, const char *id, const char *icon) {
    if (s->nslots == s->cap_slots) {
        int cap = s->cap_slots ? s->cap_slots * 2 : 64;
        epg_slot *n = realloc(s->slots, (size_t)cap * sizeof *n);
        if (!n) return -1;
        s->slots = n;
        s->cap_slots = cap;
    }
    epg_slot *e = &s->slots[s->nslots];
    e->id = strdup(id ? id : "");
    e->icon = strdup(icon ? icon : "");
    e->ch = NULL;
    e->resolved = 0;
    return s->nslots++;
}
const char *epg_slot_icon(const epg_src *s, int slot) {
    return slot >= 0 && slot < s->nslots && s->slots[slot].icon ? s->slots[slot].icon : "";
}
static ech *slot_ch(epg_src *s, int slot) {
    if (!s->st || slot < 0 || slot >= s->nslots) return NULL;
    epg_slot *e = &s->slots[slot];
    if (!e->resolved) { e->resolved = 1; e->ch = find_ch(s->st, e->id); }
    return e->ch;
}
int epg_count(epg_src *s, int slot) {
    ech *c = slot_ch(s, slot);
    return c ? (int)c->total : 0;
}
static int chunk_of(const ech *c, uint32_t i) {
    int lo = 0, hi = c->n - 1;
    while (lo < hi) {
        int m = (lo + hi + 1) / 2;
        if (c->c[m].first <= i) lo = m;
        else hi = m - 1;
    }
    return lo;
}
int epg_get(epg_src *s, int slot, int i, epg_item *out) {
    memset(out, 0, sizeof *out);
    out->title = out->sub = out->desc = out->cat = out->icon = "";
    ech *c = slot_ch(s, slot);
    if (!c || i < 0 || (uint32_t)i >= c->total) return -1;
    const ck *k = &c->c[chunk_of(c, (uint32_t)i)];
    cent *e = chunk(s->st, k);
    uint32_t j = (uint32_t)i - k->first;
    if (!e || !e->raw || j >= e->n) return -1;
    rec_at(e->raw, e->n, e->rlen, j, out);
    return 0;
}
int epg_lower(epg_src *s, int slot, int64_t t) {
    ech *c = slot_ch(s, slot);
    if (!c || !c->n) return 0;
    if (t < 0) return 0;
    if (t >= 0xffffffffLL) return (int)c->total;
    uint32_t u = (uint32_t)t;
    int lo = 0, hi = c->n;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (c->c[m].stop1 <= u) lo = m + 1;
        else hi = m;
    }
    if (lo == c->n) return (int)c->total;
    const ck *k = &c->c[lo];
    cent *e = chunk(s->st, k);
    if (!e || !e->raw) return (int)k->first;
    uint32_t a = 0, b = e->n;
    while (a < b) {
        uint32_t m = (a + b) / 2, stop;
        memcpy(&stop, e->raw + (size_t)m * REC + 4, 4);
        if (stop <= u) a = m + 1;
        else b = m;
    }
    return (int)(k->first + a);
}

int epg_has_data(const epg_src *s) { return s->st != NULL; }

void epg_close(epg_src *s) {
    store_close(s->st);
    s->st = NULL;
    epg_slots_clear(s);
}

void epg_open(epg_src *s, const char *url) {
    epg_close(s);
    snprintf(s->url, sizeof s->url, "%s", url);
    s->hash = hash_str(url);
    s->gen = 0;
    load_meta(s);
    s->st = store_open(s->hash, s->gen);
    if (s->st) s->programmes = (long)s->st->nprog;
}

int epg_scan(const epg_src *s, epg_chan_cb cb, void *ctx) {
    if (!s->st) return -1;
    char p[800];
    gen_path(s->hash, 'c', s->gen, "tsv", p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) > 0) {
        if (line[len - 1] == '\n') line[--len] = 0;
        const char *fld[34];
        int nf = 0;
        char *q = line;
        fld[nf++] = q;
        for (; *q && nf < 34; q++) if (*q == '\t') { *q = 0; fld[nf++] = q + 1; }
        if (nf < 2 || !*fld[0]) continue;
        cb(ctx, fld[0], fld[1], fld + 2, nf - 2);
    }
    free(line);
    fclose(f);
    return 0;
}

uint32_t epg_want_hash(const epg_want *w, int nw) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < nw; i++) {
        const char *v[3] = {w[i].tvg_id, w[i].tvg_name_key, w[i].name_key};
        for (int k = 0; k < 3; k++) { h ^= hash_str(v[k] ? v[k] : ""); h *= 16777619u; }
        h ^= (uint32_t)w[i].archive_days;
        h *= 16777619u;
    }
    return h ? h : 1;
}

/* ================= построение ================= */
typedef struct { uint32_t start, stop, off[5]; int is_new; } crec;
typedef struct { ck *c; int n, cap; uint32_t total; int carried; } bslot;
typedef struct { char *id, *names; } bchan;

struct epg_build {
    uint32_t hash, old_gen, new_gen;
    int64_t now, win_end;
    char tmp_d[800], tmp_c[800], tmp_i[800];
    FILE *fd, *fc;
    int rfd;                      /* чтение d<gen>.new (канал встретился второй раз) */
    uint64_t doff;
    int io_error;
    bchan *ch; bslot *bs; int nch, cap_ch;
    smap ids;                     /* id -> индекс ch */
    const epg_want *w; int nw;
    int *slot_depth;              /* -1 — канал не нужен, иначе глубина архива (дни) */
    int matched;
    /* передачи текущего канала */
    int cur;
    crec *r; size_t nr, cap_r;
    sbuf cs;                      /* их строки; смещение 0 — пустая строка */
    long parsed, stored;
    epg_store *old;
    xmltv *x;
    const char *langs[2];
    sbuf raw, ss;
    unsigned char *z; size_t zcap;
};

static uint32_t cs_put(epg_build *b, const char *s) {
    if (!s || !*s) return 0;
    size_t n = strlen(s);
    if (b->cs.n + n + 1 >= 0xffffffffu) return 0;
    uint32_t off = (uint32_t)b->cs.n;
    if (sb_addn(&b->cs, s, n + 1)) { b->io_error = 1; return 0; }
    return off;
}
static void cs_reset(epg_build *b) {
    sb_clear(&b->cs);
    sb_addn(&b->cs, "", 1);
}
static crec *new_rec(epg_build *b) {
    if (b->nr >= b->cap_r) {
        size_t cap = b->cap_r ? b->cap_r * 2 : 4096;
        crec *r = realloc(b->r, cap * sizeof *r);
        if (!r) { b->io_error = 1; return NULL; }
        b->r = r;
        b->cap_r = cap;
    }
    return &b->r[b->nr++];
}
static void add_item(epg_build *b, const epg_item *it, int is_new) {
    crec *r = new_rec(b);
    if (!r) return;
    r->start = (uint32_t)it->start;
    r->stop = (uint32_t)it->stop;
    r->is_new = is_new;
    r->off[0] = cs_put(b, it->title);
    r->off[1] = cs_put(b, it->sub);
    r->off[2] = cs_put(b, it->desc);
    r->off[3] = cs_put(b, it->cat);
    r->off[4] = cs_put(b, it->icon);
}

static int cmp_rec(const void *a, const void *b) {
    const crec *x = a, *y = b;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    if (x->is_new != y->is_new) return x->is_new > y->is_new ? -1 : 1;   /* новая — первой */
    return 0;
}

static void add_chunk(epg_build *b, bslot *s, const ck *c) {
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 16;
        ck *n = realloc(s->c, (size_t)cap * sizeof *n);
        if (!n) { b->io_error = 1; return; }
        s->c = n;
        s->cap = cap;
    }
    s->c[s->n++] = *c;
    s->total += c->count;
}

/* Передачи r[a..e) (одни сутки) — одним сжатым куском в d<gen>.new. Уровень zlib 1: на 2 ГБ XMLTV вдвое быстрее
 * уровня 6 (сжатие — половина времени сборки), данные больше всего на 20 %. */
static void emit(epg_build *b, bslot *s, size_t a, size_t e) {
    uint32_t n = (uint32_t)(e - a);
    sb_clear(&b->raw);
    sb_clear(&b->ss);
    sb_addn(&b->ss, "", 1);
    for (size_t i = a; i < e; i++) {
        uint32_t r[7] = {b->r[i].start, b->r[i].stop, 0, 0, 0, 0, 0};
        for (int k = 0; k < 5; k++) {
            if (!b->r[i].off[k]) continue;
            const char *str = b->cs.s + b->r[i].off[k];
            size_t l = strlen(str);
            r[2 + k] = (uint32_t)b->ss.n;
            sb_addn(&b->ss, str, l + 1);
        }
        sb_addn(&b->raw, (const char *)r, REC);
    }
    sb_addn(&b->raw, b->ss.s, b->ss.n);
    uLongf zl = compressBound((uLong)b->raw.n);
    if (zl > b->zcap) {
        unsigned char *z = realloc(b->z, zl);
        if (!z) { b->io_error = 1; return; }
        b->z = z;
        b->zcap = zl;
    }
    if (compress2(b->z, &zl, (const Bytef *)b->raw.s, (uLong)b->raw.n, 1) != Z_OK) { b->io_error = 1; return; }
    if (fwrite(b->z, 1, zl, b->fd) != zl) { b->io_error = 1; return; }
    ck c = {b->r[a].start, b->r[e - 1].stop, n, (uint32_t)zl, (uint32_t)b->raw.n, 0, b->doff};
    b->doff += zl;
    add_chunk(b, s, &c);
    b->stored += n;
}

/* Канал встретился в XMLTV второй раз (или пачка была слишком большой): его куски — обратно в память. */
static void read_back(epg_build *b, bslot *s) {
    if (fflush(b->fd)) { b->io_error = 1; return; }
    if (b->rfd < 0) b->rfd = open(b->tmp_d, O_RDONLY | O_CLOEXEC);
    if (b->rfd < 0) { b->io_error = 1; return; }
    for (int k = 0; k < s->n; k++) {
        uint32_t n;
        unsigned char *raw = read_chunk(b->rfd, &s->c[k], &n);
        for (uint32_t j = 0; raw && j < n; j++) {
            epg_item it;
            rec_at(raw, n, s->c[k].rlen, j, &it);
            add_item(b, &it, 1);
        }
        free(raw);
    }
    b->stored -= s->total;
    s->n = 0;
    s->total = 0;
}

/* Прошлое прежней выгрузки: передачи до первой новой (многие XMLTV — только с сегодняшнего дня, а архив — неделя),
 * не старше глубины архива. */
static void carry(epg_build *b, int slot) {
    ech *oc = b->old ? find_ch(b->old, b->ch[slot].id) : NULL;
    if (!oc) return;
    uint32_t first = 0xffffffffu;
    for (size_t i = 0; i < b->nr; i++) if (b->r[i].start < first) first = b->r[i].start;
    int64_t from = day_start(b->now) - (int64_t)(b->slot_depth[slot] + 1) * 86400;
    uint32_t lim = first != 0xffffffffu ? first : (uint32_t)(b->now > 0 ? b->now : 0);
    for (int k = 0; k < oc->n; k++) {
        const ck *c = &oc->c[k];
        if ((int64_t)c->stop1 <= from || c->start0 >= lim) continue;
        uint32_t n;
        unsigned char *raw = read_chunk(b->old->fd, c, &n);
        for (uint32_t j = 0; raw && j < n; j++) {
            epg_item it;
            rec_at(raw, n, c->rlen, j, &it);
            if (it.stop <= from || it.stop > lim) continue;
            add_item(b, &it, 0);
        }
        free(raw);
    }
}

/* Передачи текущего канала: перенос прошлого, порядок, повторы и перекрытия, по суткам — в файл. */
static void write_slot(epg_build *b, int slot) {
    bslot *s = &b->bs[slot];
    if (s->n) read_back(b, s);
    if (!s->carried) { s->carried = 1; carry(b, slot); }
    if (b->nr) {
        qsort(b->r, b->nr, sizeof *b->r, cmp_rec);
        /* одинаковое начало — остаётся новая; перекрытие — обрезка по следующей */
        size_t w = 0;
        for (size_t i = 0; i < b->nr; i++) {
            if (w && b->r[w - 1].start == b->r[i].start) continue;
            b->r[w++] = b->r[i];
        }
        b->nr = w;
        w = 0;
        for (size_t i = 0; i < b->nr; i++) {
            crec *r = &b->r[i];
            if (i + 1 < b->nr && b->r[i + 1].start < r->stop) r->stop = b->r[i + 1].start;
            if (r->stop <= r->start) continue;
            b->r[w++] = *r;
        }
        b->nr = w;
        size_t a = 0;
        for (size_t i = 1; i <= b->nr; i++)
            if (i == b->nr || b->r[i].start / 86400 != b->r[a].start / 86400) { emit(b, s, a, i); a = i; }
    }
    b->nr = 0;
    cs_reset(b);
}

static void flush_cur(epg_build *b) {
    if (b->cur < 0) return;
    int slot = b->cur;
    b->cur = -1;
    write_slot(b, slot);
}

static void put_field(FILE *f, const char *s) {
    for (; s && *s; s++) fputc(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s, f);
}

static void on_channel(void *ctx, const char *id, const char *const *names, int n, const char *icon) {
    epg_build *b = ctx;
    if (!id || !*id) return;
    int i = smap_get(&b->ids, id, -1);
    if (i < 0) {
        if (b->nch >= b->cap_ch) {
            int cap = b->cap_ch ? b->cap_ch * 2 : 512;
            bchan *c = realloc(b->ch, (size_t)cap * sizeof *c);
            if (!c) { b->io_error = 1; return; }
            b->ch = c;
            bslot *s = realloc(b->bs, (size_t)cap * sizeof *s);
            if (!s) { b->io_error = 1; return; }
            memset(s + b->cap_ch, 0, (size_t)(cap - b->cap_ch) * sizeof *s);
            b->bs = s;
            b->cap_ch = cap;
        }
        i = b->nch++;
        smap_set(&b->ids, id, i);
        b->ch[i].id = strdup(id);
        b->ch[i].names = NULL;
    }
    sbuf nm = {0};
    for (int k = 0; k < n; k++) { if (k) sb_addc(&nm, '\n'); sb_add(&nm, names[k]); }
    free(b->ch[i].names);
    b->ch[i].names = sb_take(&nm);
    /* строка каналов: id, иконка, имена */
    put_field(b->fc, id);
    fputc('\t', b->fc);
    put_field(b->fc, icon);
    for (int k = 0; k < n && k < 32; k++) { fputc('\t', b->fc); put_field(b->fc, names[k]); }
    fputc('\n', b->fc);
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
    /* смена канала — его передачи в файл; очень длинный канал — частями (соберутся в write_slot) */
    if (slot != b->cur || b->nr >= 200000 || b->cs.n >= (64u << 20)) { flush_cur(b); b->cur = slot; }
    epg_item it = {start, stop, t, su, d, c, ic};
    add_item(b, &it, 1);
    b->parsed++;
}

/* перед передачами: какие каналы XMLTV нужны (сопоставление с каналами плейлистов) и глубина их архива */
static void before_programmes(void *ctx) {
    epg_build *b = ctx;
    free(b->slot_depth);
    b->slot_depth = malloc((size_t)(b->nch + 1) * sizeof(int));
    if (!b->slot_depth) { b->io_error = 1; return; }
    for (int i = 0; i < b->nch; i++) b->slot_depth[i] = -1;
    smap nk;
    smap_init(&nk, b->nch * 2 + 16);
    char key[300];
    for (int i = 0; i < b->nch; i++) {
        const char *n = b->ch[i].names;
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

epg_build *epg_build_new(uint32_t hash, uint32_t old_gen, uint32_t new_gen, const epg_want *w, int nw, int64_t now,
                         const char *lang) {
    if (make_dir(hash)) return NULL;
    epg_build *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->hash = hash;
    b->old_gen = old_gen;
    b->new_gen = new_gen;
    b->now = now;
    b->win_end = now + 8 * 86400;
    b->w = w;
    b->nw = nw;
    b->cur = -1;
    b->rfd = -1;
    smap_init(&b->ids, 1024);
    gen_path(hash, 'd', new_gen, "new", b->tmp_d, sizeof b->tmp_d);
    gen_path(hash, 'c', new_gen, "new", b->tmp_c, sizeof b->tmp_c);
    gen_path(hash, 'i', new_gen, "new", b->tmp_i, sizeof b->tmp_i);
    b->fd = fopen(b->tmp_d, "wb");
    b->fc = fopen(b->tmp_c, "wb");
    if (!b->fd || !b->fc) { epg_build_free(b); return NULL; }
    setvbuf(b->fd, NULL, _IOFBF, 1 << 16);
    setvbuf(b->fc, NULL, _IOFBF, 1 << 15);
    cs_reset(b);
    b->old = store_open(hash, old_gen);
    b->langs[0] = lang && *lang ? lang : "ru";
    b->langs[1] = "ru";
    return b;
}

void epg_build_push(epg_build *b, const char *data, size_t n) {
    if (!b->x) {
        xmltv_cb cb = {.ctx = b, .on_channel = on_channel, .before_programmes = before_programmes, .wanted = wanted,
                       .on_programme = on_programme, .accept = accept_prog, .nlangs = 2};
        cb.langs[0] = b->langs[0];
        cb.langs[1] = b->langs[1];
        b->x = xmltv_new(&cb);
        if (!b->x) { b->io_error = 1; return; }
    }
    xmltv_push(b->x, data, n);
}

long epg_build_programmes(const epg_build *b) { return b->parsed; }
long epg_build_matched(const epg_build *b) { return b->matched; }
long epg_build_stored(const epg_build *b) { return b->stored; }

static void w32(FILE *f, uint32_t v, int *ok) { if (fwrite(&v, 4, 1, f) != 1) *ok = 0; }

/* Оглавление: каналы с передачами по корзинам (хэш id), у канала — его куски. */
static int write_index(epg_build *b) {
    int nst = 0;
    for (int i = 0; i < b->nch; i++) if (b->bs[i].n) nst++;
    uint32_t nb = 1;
    while (nb < MAX_BUCKETS && nb * PER_BUCKET < (uint32_t)nst) nb <<= 1;
    int *order = malloc((size_t)(nst + 1) * sizeof *order);
    uint32_t *cnt = calloc(nb + 1, sizeof *cnt), *dir = calloc(nb * 2, sizeof *dir), *pos = calloc(nb, sizeof *pos);
    FILE *f = fopen(b->tmp_i, "wb");
    int ok = order && cnt && dir && pos && f;
    if (ok) {
        /* сортировка подсчётом по корзинам */
        for (int i = 0; i < b->nch; i++) if (b->bs[i].n) cnt[(hash_str(b->ch[i].id) & (nb - 1)) + 1]++;
        for (uint32_t k = 1; k <= nb; k++) cnt[k] += cnt[k - 1];
        for (int i = 0; i < b->nch; i++) {
            if (!b->bs[i].n) continue;
            uint32_t k = hash_str(b->ch[i].id) & (nb - 1);
            order[cnt[k] + pos[k]++] = i;
        }
        uint32_t np = b->stored > 0xffffffffL ? 0xffffffffu : (uint32_t)b->stored;
        ihdr h = {IMAGIC, IVERSION, nb, (uint32_t)nst, np, (uint32_t)(uint64_t)b->now, (uint32_t)((uint64_t)b->now >> 32), 0};
        ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(dir, 8, nb, f) == nb;
        uint32_t off = (uint32_t)(sizeof h + nb * 8);
        for (uint32_t k = 0; ok && k < nb; k++) {
            uint32_t start = off;
            w32(f, cnt[k + 1] - cnt[k], &ok);
            off += 4;
            for (uint32_t q = cnt[k]; ok && q < cnt[k + 1]; q++) {
                int i = order[q];
                bslot *s = &b->bs[i];
                uint32_t il = (uint32_t)strlen(b->ch[i].id);
                w32(f, il, &ok);
                if (fwrite(b->ch[i].id, 1, il, f) != il) ok = 0;
                w32(f, (uint32_t)s->n, &ok);
                w32(f, s->total, &ok);
                for (int j = 0; j < s->n; j++) {
                    const ck *c = &s->c[j];
                    uint32_t r[7] = {c->start0, c->stop1, c->count, c->clen, c->rlen, (uint32_t)c->off, (uint32_t)(c->off >> 32)};
                    if (fwrite(r, 4, 7, f) != 7) ok = 0;
                }
                off += 12 + il + (uint32_t)s->n * REC;
            }
            dir[k * 2] = start;
            dir[k * 2 + 1] = off - start;
        }
        if (ok && (fseeko(f, (off_t)sizeof h, SEEK_SET) || fwrite(dir, 8, nb, f) != nb)) ok = 0;
        if (ok && fflush(f)) ok = 0;
        if (ok) fsync(fileno(f));
    }
    if (f && fclose(f)) ok = 0;
    free(order);
    free(cnt);
    free(dir);
    free(pos);
    return ok ? 0 : -1;
}

int epg_build_finish(epg_build *b, char *err, size_t errn) {
    if (b->x) xmltv_end(b->x);
    if (!b->x || !xmltv_channels(b->x)) { snprintf(err, errn, "format"); return -1; }
    if (!b->slot_depth) before_programmes(b);   /* каналы без передач */
    flush_cur(b);
    /* нужные каналы без новых передач (XMLTV их больше не отдаёт) — прошедшее из прежней выгрузки */
    for (int i = 0; b->slot_depth && i < b->nch; i++)
        if (b->slot_depth[i] >= 0 && !b->bs[i].carried) write_slot(b, i);
    if (fflush(b->fd) || b->io_error) { snprintf(err, errn, "write: %s", strerror(errno)); return -1; }
    fsync(fileno(b->fd));
    int e1 = fclose(b->fd), e2 = fclose(b->fc);
    b->fd = b->fc = NULL;
    if (e1 || e2 || write_index(b)) { snprintf(err, errn, "write: %s", strerror(errno)); return -1; }
    char pd[800], pc[800], pi[800];
    gen_path(b->hash, 'd', b->new_gen, "bin", pd, sizeof pd);
    gen_path(b->hash, 'c', b->new_gen, "tsv", pc, sizeof pc);
    gen_path(b->hash, 'i', b->new_gen, "bin", pi, sizeof pi);
    /* оглавление — последним: без него выгрузка не откроется */
    if (rename(b->tmp_d, pd) || rename(b->tmp_c, pc) || rename(b->tmp_i, pi)) {
        snprintf(err, errn, "rename: %s", strerror(errno));
        return -1;
    }
    fprintf(stderr, "epg_build channels=%d matched=%d programmes=%ld parsed=%ld skipped=%ld data=%llu\n", b->nch, b->matched,
            b->stored, xmltv_programmes(b->x), xmltv_skipped(b->x), (unsigned long long)b->doff);
    return 0;
}

void epg_build_free(epg_build *b) {
    if (!b) return;
    if (b->fd) fclose(b->fd);
    if (b->fc) fclose(b->fc);
    if (b->rfd >= 0) close(b->rfd);
    unlink(b->tmp_d);
    unlink(b->tmp_c);
    unlink(b->tmp_i);
    xmltv_free(b->x);
    for (int i = 0; i < b->nch; i++) { free(b->ch[i].id); free(b->ch[i].names); free(b->bs[i].c); }
    free(b->ch);
    free(b->bs);
    smap_free(&b->ids);
    free(b->slot_depth);
    free(b->r);
    sb_free(&b->cs);
    sb_free(&b->raw);
    sb_free(&b->ss);
    free(b->z);
    store_close(b->old);
    free(b);
}
