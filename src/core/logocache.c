/* Логотипы каналов: память (LRU, 12 МБ) + диск (data_dir/logos, до 64 МБ). Загрузка — рабочий поток pool_submit_low;
 * в памяти ключ «адрес + размер», на диске — «адрес»: оригинальные байты одни на все размеры. Неудачный адрес
 * больше не запрашивается до logo_clear (как кэш каркаса), битый файл удаляется. */
#include "logocache.h"
#include "app.h"
#include "img.h"
#include "net.h"
#include "core/clock.h"
#include "core/http.h"
#include "core/util.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_E 96
#define MAX_FAIL 128
#define MAX_JOBS 8
#define MEM_BUDGET (12u << 20)
#define MAX_BYTES (8u << 20)
#define DISK_BUDGET (64u << 20)
#define DISK_PRUNE_OLD (180 * 86400)

typedef struct {
    unsigned long long key;   /* адрес + размер */
    char url[600];
    int w, h;
    gfx_image *im;
    int state;                /* 0 — грузится, 1 — готово, -1 — не вышло */
    double used;
} ent;
typedef struct {
    unsigned long long key, ukey;
    char url[600], path[800];
    int w, h, mode, gen;
    gfx_image *out;
} ljob;

static ent ents[MAX_E];
static int nent;
static unsigned long long fail[MAX_FAIL];
static int nfail;
static int jobs, inited, gen;
static char dir[700];

static unsigned long long hash64(const char *s, int w, int h) {
    unsigned long long v = 1469598103934665603ull;
    v ^= (unsigned long long)(unsigned)w << 40;
    v ^= (unsigned long long)(unsigned)h;
    for (; s && *s; s++) { v ^= (unsigned char)*s; v *= 1099511628211ull; }
    return v;
}
static void path_of(char *out, size_t cap, unsigned long long ukey) {
    if (dir[0]) snprintf(out, cap, "%s/%016llx", dir, ukey);
    else out[0] = 0;
}
static ent *find(unsigned long long key) {
    for (int i = 0; i < nent; i++) if (ents[i].key == key) return &ents[i];
    return NULL;
}
static int failed(unsigned long long key) {
    for (int i = 0; i < nfail; i++) if (fail[i] == key) return 1;
    return 0;
}
static void add_fail(unsigned long long key) {
    if (nfail < MAX_FAIL) fail[nfail++] = key;
}
/* Освободить память сверх бюджета: сначала самые давние готовые. */
static void trim_mem(void) {
    size_t bytes = 0;
    for (int i = 0; i < nent; i++) if (ents[i].state == 1 && ents[i].im) bytes += (size_t)ents[i].im->w * (size_t)ents[i].im->h * 4;
    while (bytes > MEM_BUDGET) {
        int v = -1;
        double oldest = 1e300;
        for (int i = 0; i < nent; i++)
            if (ents[i].state == 1 && ents[i].im && ents[i].used < oldest) { oldest = ents[i].used; v = i; }
        if (v < 0) break;
        bytes -= (size_t)ents[v].im->w * (size_t)ents[v].im->h * 4;
        gfx_image_free(ents[v].im);
        ents[v].im = NULL;
        memmove(ents + v, ents + v + 1, (size_t)(nent - v - 1) * sizeof *ents);
        nent--;
    }
}
static ent *add_ent(unsigned long long key, const char *url, int w, int h) {
    if (nent >= MAX_E) { trim_mem(); if (nent >= MAX_E) return NULL; }
    ent *e = &ents[nent++];
    memset(e, 0, sizeof *e);
    e->key = key;
    snprintf(e->url, sizeof e->url, "%s", url);
    e->w = w;
    e->h = h;
    e->used = app_now();
    return e;
}

/* ---------- диск ---------- */
typedef struct { char name[300]; long long mt, sz; } dfil;
static int cmp_mt(const void *a, const void *b) {
    const dfil *x = a, *y = b;
    return x->mt < y->mt ? 1 : x->mt > y->mt ? -1 : 0;
}
static void prune_disk(void) {
    if (!dir[0]) return;
    DIR *d = opendir(dir);
    if (!d) return;
    static dfil f[512];
    int n = 0;
    long long total = 0;
    int64_t now = clock_now();
    struct dirent *de;
    while ((de = readdir(d)) && n < 512) {
        if (de->d_name[0] == '.') continue;
        char p[1000];
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(p, &st) || !S_ISREG(st.st_mode)) continue;
        if (now - (int64_t)st.st_mtime > DISK_PRUNE_OLD) { unlink(p); continue; }
        snprintf(f[n].name, sizeof f[n].name, "%s", de->d_name);
        f[n].mt = (long long)st.st_mtime;
        f[n].sz = (long long)st.st_size;
        total += (long long)st.st_size;
        n++;
    }
    closedir(d);
    if (total <= (long long)DISK_BUDGET) return;
    qsort(f, (size_t)n, sizeof *f, cmp_mt);
    for (int i = 0; i < n && total > (long long)DISK_BUDGET; i++) {
        char p[1000];
        snprintf(p, sizeof p, "%s/%s", dir, f[i].name);
        if (!unlink(p)) total -= f[i].sz;
    }
}
static void init_dir(void) {
    if (inited) return;
    inited = 1;
    const app_platform *pl = app_platform_get();
    if (!pl || !pl->data_dir || !pl->data_dir[0]) return;
    snprintf(dir, sizeof dir, "%s/logos", pl->data_dir);
    if (mkdir(dir, 0755) && errno != EEXIST) dir[0] = 0;
    else prune_disk();
}

/* ---------- загрузка ---------- */
static void lwork(void *vp) {
    ljob *j = vp;
    char err[80] = "";
    size_t len = 0;
    char *data = http_get_small(j->url, MAX_BYTES, &len, err, sizeof err);
    if (!data || !len) { free(data); fprintf(stderr, "logo_load_failed %s\n", err); return; }
    if (j->path[0]) file_write_atomic(j->path, data, len);
    j->out = img_decode(data, len, j->w, j->h, j->mode);
    free(data);
}
static void ldone(void *vp) {
    ljob *j = vp;
    int ok = j->out != NULL;
    jobs--;
    if (j->gen == gen) {
        ent *e = find(j->key);
        if (e) { e->im = j->out; e->state = ok ? 1 : -1; e->used = app_now(); j->out = NULL; }
        if (!ok) {   /* адрес не дал картинку: битый файл убрать, в этой сессии не пробовать снова */
            add_fail(j->ukey);
            if (j->path[0]) unlink(j->path);
        }
    }
    if (j->out) gfx_image_free(j->out);
    free(j);
    app_dirty();
}

const gfx_image *logo_img(const char *url, int w, int h, int mode) {
    if (!url || !*url || w <= 0 || h <= 0 || strncmp(url, "http", 4)) return NULL;
    init_dir();
    unsigned long long key = hash64(url, w, h), ukey = hash64(url, 0, 0);
    ent *e = find(key);
    double now = app_now();
    if (e) {
        e->used = now;
        return e->state == 1 ? e->im : NULL;
    }
    if (failed(ukey)) return NULL;
    char p[800];
    path_of(p, sizeof p, ukey);
    struct stat st;
    if (p[0] && !stat(p, &st) && st.st_size > 0 && (unsigned long long)st.st_size <= MAX_BYTES) {
        size_t len = 0;
        char *data = file_read(p, &len);
        gfx_image *im = data && len ? img_decode(data, len, w, h, mode) : NULL;
        free(data);
        if (!im) { unlink(p); return NULL; }   /* битый файл — как будто его нет */
        e = add_ent(key, url, w, h);
        if (!e) { gfx_image_free(im); return NULL; }
        e->im = im;
        e->state = 1;
        trim_mem();
        return im;
    }
    if (jobs >= MAX_JOBS) return NULL;
    e = add_ent(key, url, w, h);
    ljob *j = calloc(1, sizeof *j);
    if (!e || !j) { free(j); if (e) { memmove(e, e + 1, (size_t)(nent - (int)(e - ents) - 1) * sizeof *ents); nent--; } return NULL; }
    j->key = key;
    j->ukey = ukey;
    snprintf(j->url, sizeof j->url, "%s", url);
    snprintf(j->path, sizeof j->path, "%s", p);
    j->w = w;
    j->h = h;
    j->mode = mode;
    j->gen = gen;
    jobs++;
    pool_submit_low(lwork, ldone, j);
    return NULL;
}

void logo_trim(void) { trim_mem(); }

int logo_cache_path(const char *url, char *out, size_t cap) {
    if (!url || !*url || !cap) return 0;
    init_dir();
    path_of(out, cap, hash64(url, 0, 0));
    return out[0] != 0;
}

void logo_clear(void) {
    for (int i = 0; i < nent; i++) gfx_image_free(ents[i].im);
    nent = 0;
    nfail = 0;
    gen++;
    if (!dir[0]) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[1000];
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        unlink(p);
    }
    closedir(d);
}
