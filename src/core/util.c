#define _GNU_SOURCE
#include "util.h"
#include "app.h"
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---------- sbuf ---------- */
static int sb_grow(sbuf *b, size_t need) {
    if (b->n + need + 1 <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->n + need + 1) cap *= 2;
    char *s = realloc(b->s, cap);
    if (!s) return -1;
    b->s = s;
    b->cap = cap;
    return 0;
}
int sb_addn(sbuf *b, const char *s, size_t n) {
    if (sb_grow(b, n)) return -1;
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
    return 0;
}
int sb_add(sbuf *b, const char *s) { return s ? sb_addn(b, s, strlen(s)) : 0; }
int sb_addc(sbuf *b, char c) { return sb_addn(b, &c, 1); }
int sb_printf(sbuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[256];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n < sizeof tmp) return sb_addn(b, tmp, (size_t)n);
    if (sb_grow(b, (size_t)n)) return -1;
    va_start(ap, fmt);
    vsnprintf(b->s + b->n, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->n += (size_t)n;
    return 0;
}
void sb_clear(sbuf *b) { b->n = 0; if (b->s) b->s[0] = 0; }
void sb_free(sbuf *b) { free(b->s); b->s = NULL; b->n = b->cap = 0; }
char *sb_take(sbuf *b) {
    char *s = b->s ? b->s : calloc(1, 1);
    b->s = NULL;
    b->n = b->cap = 0;
    return s;
}

/* ---------- strpool ---------- */
struct pool_blk { pool_blk *next; size_t used, cap; char data[]; };
char *sp_dupn(strpool *p, const char *s, size_t n) {
    pool_blk *b = p->head;
    if (!b || b->used + n + 1 > b->cap) {
        size_t cap = n + 1 > 60000 ? n + 1 : 64000;
        pool_blk *nb = malloc(sizeof *nb + cap);
        if (!nb) return NULL;
        nb->next = p->head;
        nb->used = 0;
        nb->cap = cap;
        p->head = b = nb;
    }
    char *d = b->data + b->used;
    memcpy(d, s, n);
    d[n] = 0;
    b->used += n + 1;
    return d;
}
char *sp_dup(strpool *p, const char *s) { return sp_dupn(p, s ? s : "", s ? strlen(s) : 0); }
void sp_free(strpool *p) {
    pool_blk *b = p->head;
    while (b) { pool_blk *n = b->next; free(b); b = n; }
    p->head = NULL;
}

/* ---------- UTF-8 ---------- */
uint32_t u8_next(const char **ps) {
    const unsigned char *s = (const unsigned char *)*ps;
    uint32_t c = s[0];
    if (!c) return 0;
    if (c < 0x80) { *ps += 1; return c; }
    if ((c >> 5) == 6 && (s[1] & 0xc0) == 0x80) { *ps += 2; return ((c & 31) << 6) | (s[1] & 63); }
    if ((c >> 4) == 14 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) { *ps += 3; return ((c & 15) << 12) | ((s[1] & 63) << 6) | (s[2] & 63); }
    if ((c >> 3) == 30 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80) {
        *ps += 4;
        return ((c & 7) << 18) | ((s[1] & 63) << 12) | ((s[2] & 63) << 6) | (s[3] & 63);
    }
    *ps += 1;
    return 0xfffd;
}
int u8_put(char *o, uint32_t c) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xc0 | (c >> 6)); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xe0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xf0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 63)); o[2] = (char)(0x80 | ((c >> 6) & 63)); o[3] = (char)(0x80 | (c & 63));
    return 4;
}
uint32_t u8_lower(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c < 0x80) return c;
    if (c >= 0x410 && c <= 0x42f) return c + 32;          /* А..Я */
    if (c >= 0x400 && c <= 0x40f) return c + 80;          /* Ѐ..Џ (Ё) */
    if (c >= 0xc0 && c <= 0xde && c != 0xd7) return c + 32;
    if (c >= 0x391 && c <= 0x3a9 && c != 0x3a2) return c + 32;
    if (c >= 0x460 && c <= 0x4ff && !(c & 1)) return c + 1;
    if (c >= 0x100 && c <= 0x17f && !(c & 1)) return c + 1;
    return c;
}
int u8_is_letter(uint32_t c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xc0 && c <= 0x24f && c != 0xd7 && c != 0xf7) ||
           (c >= 0x370 && c <= 0x3ff) || (c >= 0x400 && c <= 0x4ff);
}
void u8_lower_str(char *dst, size_t cap, const char *s) {
    size_t n = 0;
    uint32_t c;
    while ((c = u8_next(&s)) != 0) {
        char t[4];
        int k = u8_put(t, u8_lower(c));
        if (n + (size_t)k + 1 > cap) break;
        memcpy(dst + n, t, (size_t)k);
        n += (size_t)k;
    }
    if (cap) dst[n] = 0;
}
void u8_copy(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xc0) == 0x80) n--;
    }
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

char *xstrndup(const char *s, size_t n) {
    char *d = malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}
void trim(char *s) {
    size_t n = strlen(s), a = 0;
    while (a < n && isspace((unsigned char)s[a])) a++;
    while (n > a && isspace((unsigned char)s[n - 1])) n--;
    if (a) memmove(s, s + a, n - a);
    s[n - a] = 0;
}
int starts_with(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }
int starts_with_ci(const char *s, const char *p) { return !strncasecmp(s, p, strlen(p)); }
int ends_with_ci(const char *s, const char *x) {
    size_t a = strlen(s), b = strlen(x);
    return a >= b && !strcasecmp(s + a - b, x);
}
const char *strcasestr_ascii(const char *h, const char *n) {
    size_t k = strlen(n);
    for (; *h; h++) if (!strncasecmp(h, n, k)) return h;
    return NULL;
}

/* ---------- файлы ---------- */
char *file_read(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    sbuf b = {0};
    char tmp[16384];
    size_t r;
    while ((r = fread(tmp, 1, sizeof tmp, f)) > 0) if (sb_addn(&b, tmp, r)) { fclose(f); sb_free(&b); return NULL; }
    fclose(f);
    if (len) *len = b.n;
    return sb_take(&b);
}
int file_write_atomic(const char *path, const void *data, size_t len) {
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    if (len && fwrite(data, 1, len, f) != len) { fclose(f); unlink(tmp); return -1; }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}
int file_exists(const char *path) { struct stat st; return !stat(path, &st); }
long long file_size(const char *path) { struct stat st; return stat(path, &st) ? -1 : (long long)st.st_size; }
const char *data_path(const char *name) {
    static __thread char p[640];
    const app_platform *pl = app_platform_get();
    snprintf(p, sizeof p, "%s/%s", pl && pl->data_dir ? pl->data_dir : ".", name);
    return p;
}

/* ---------- хэш ---------- */
uint32_t hash_bytes(const void *p, size_t n) {
    const unsigned char *s = p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= s[i]; h *= 16777619u; }
    return h;
}
uint32_t hash_str(const char *s) { return hash_bytes(s, strlen(s)); }

void smap_init(smap *m, int expect) {
    int cap = 16;
    while (cap < expect * 2) cap *= 2;
    m->keys = calloc((size_t)cap, sizeof *m->keys);
    m->vals = calloc((size_t)cap, sizeof *m->vals);
    m->cap = m->keys && m->vals ? cap : 0;
    m->n = 0;
}
void smap_free(smap *m) {
    for (int i = 0; i < m->cap; i++) free(m->keys[i]);
    free(m->keys);
    free(m->vals);
    memset(m, 0, sizeof *m);
}
int smap_get(const smap *m, const char *k, int def) {
    if (!m->cap) return def;
    uint32_t i = hash_str(k) & (uint32_t)(m->cap - 1);
    while (m->keys[i]) {
        if (!strcmp(m->keys[i], k)) return m->vals[i];
        i = (i + 1) & (uint32_t)(m->cap - 1);
    }
    return def;
}
static void smap_put(smap *m, char *k, int v) {
    uint32_t i = hash_str(k) & (uint32_t)(m->cap - 1);
    while (m->keys[i]) i = (i + 1) & (uint32_t)(m->cap - 1);
    m->keys[i] = k;
    m->vals[i] = v;
    m->n++;
}
void smap_set(smap *m, const char *k, int v) {
    if (!m->cap) smap_init(m, 16);
    if (!m->cap) return;
    uint32_t i = hash_str(k) & (uint32_t)(m->cap - 1);
    while (m->keys[i]) {
        if (!strcmp(m->keys[i], k)) { m->vals[i] = v; return; }
        i = (i + 1) & (uint32_t)(m->cap - 1);
    }
    if ((m->n + 1) * 2 > m->cap) {
        smap old = *m;
        m->cap *= 2;
        m->keys = calloc((size_t)m->cap, sizeof *m->keys);
        m->vals = calloc((size_t)m->cap, sizeof *m->vals);
        m->n = 0;
        if (!m->keys || !m->vals) { free(m->keys); free(m->vals); *m = old; return; }
        for (int j = 0; j < old.cap; j++) if (old.keys[j]) smap_put(m, old.keys[j], old.vals[j]);
        free(old.keys);
        free(old.vals);
    }
    char *d = strdup(k);
    if (d) smap_put(m, d, v);
}

/* ---------- strlist ---------- */
void sl_insert(strlist *l, int at, const char *s) {
    if (l->n >= l->cap) {
        int cap = l->cap ? l->cap * 2 : 8;
        char **v = realloc(l->v, (size_t)cap * sizeof *v);
        if (!v) return;
        l->v = v;
        l->cap = cap;
    }
    if (at < 0 || at > l->n) at = l->n;
    char *d = strdup(s ? s : "");
    if (!d) return;
    memmove(l->v + at + 1, l->v + at, (size_t)(l->n - at) * sizeof *l->v);
    l->v[at] = d;
    l->n++;
}
void sl_add(strlist *l, const char *s) { sl_insert(l, l->n, s); }
void sl_remove(strlist *l, int at) {
    if (at < 0 || at >= l->n) return;
    free(l->v[at]);
    memmove(l->v + at, l->v + at + 1, (size_t)(l->n - at - 1) * sizeof *l->v);
    l->n--;
}
int sl_find(const strlist *l, const char *s) {
    for (int i = 0; i < l->n; i++) if (!strcmp(l->v[i], s)) return i;
    return -1;
}
void sl_clear(strlist *l) {
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}
void sl_from_lines(strlist *l, const char *t) {
    sl_clear(l);
    while (t && *t) {
        const char *e = strchr(t, '\n');
        size_t n = e ? (size_t)(e - t) : strlen(t);
        char *s = xstrndup(t, n);
        if (s) { trim(s); if (*s) sl_add(l, s); free(s); }
        t = e ? e + 1 : t + n;
    }
}
char *sl_join(const strlist *l, const char *sep) {
    sbuf b = {0};
    for (int i = 0; i < l->n; i++) { if (i) sb_add(&b, sep); sb_add(&b, l->v[i]); }
    return sb_take(&b);
}

double mono_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
