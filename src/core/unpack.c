#include "unpack.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

enum { K_UNKNOWN, K_PLAIN, K_GZIP, K_ZIP, K_STORED };

struct up {
    rd_fn rd; void *ctx;
    int kind, done, err_set, z_ready;
    z_stream z;
    unsigned char in[32768];
    size_t in_pos, in_len;
    long long stored_left;
    char error[32];
};

up *up_new(rd_fn rd, void *ctx) {
    up *u = calloc(1, sizeof *u);
    if (!u) return NULL;
    u->rd = rd;
    u->ctx = ctx;
    return u;
}

static long fill(up *u) {
    if (u->in_pos < u->in_len) return (long)(u->in_len - u->in_pos);
    long r = u->rd(u->ctx, u->in, sizeof u->in);
    if (r <= 0) return r;
    u->in_pos = 0;
    u->in_len = (size_t)r;
    return r;
}
/* не меньше n байтов в буфере (n <= размера буфера) */
static int need(up *u, size_t n) {
    if (u->in_len - u->in_pos >= n) return 0;
    memmove(u->in, u->in + u->in_pos, u->in_len - u->in_pos);
    u->in_len -= u->in_pos;
    u->in_pos = 0;
    while (u->in_len < n) {
        long r = u->rd(u->ctx, u->in + u->in_len, sizeof u->in - u->in_len);
        if (r <= 0) return r < 0 ? -1 : 1;
        u->in_len += (size_t)r;
    }
    return 0;
}
static int fail(up *u, const char *why) { snprintf(u->error, sizeof u->error, "%s", why); u->err_set = 1; return -1; }
static unsigned le16(const unsigned char *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int detect(up *u) {
    int r = need(u, 6);
    if (r < 0) return fail(u, "read");
    size_t have = u->in_len - u->in_pos;
    const unsigned char *b = u->in + u->in_pos;
    if (have >= 2 && b[0] == 0x1f && b[1] == 0x8b) {
        u->kind = K_GZIP;
        if (inflateInit2(&u->z, 15 + 16) != Z_OK) return fail(u, "gzip");
        u->z_ready = 1;
        return 0;
    }
    if (have >= 5 && b[0] == 0xfd && b[1] == '7' && b[2] == 'z' && b[3] == 'X' && b[4] == 'Z') return fail(u, "format");
    if (have >= 4 && b[0] == 'P' && b[1] == 'K' && b[2] == 3 && b[3] == 4) {
        if (need(u, 30)) return fail(u, "zip");
        b = u->in + u->in_pos;
        unsigned flags = le16(b + 6), method = le16(b + 8), nlen = le16(b + 26), xlen = le16(b + 28);
        uint32_t csize = le32(b + 18);
        u->in_pos += 30;
        size_t skip = nlen + xlen;   /* имя и поле extra пропускаются потоком */
        while (skip) {
            if (fill(u) <= 0) return fail(u, "zip");
            size_t k = u->in_len - u->in_pos < skip ? u->in_len - u->in_pos : skip;
            u->in_pos += k;
            skip -= k;
        }
        if (method == 8) {
            u->kind = K_ZIP;
            if (inflateInit2(&u->z, -15) != Z_OK) return fail(u, "zip");
            u->z_ready = 1;
            return 0;
        }
        if (method == 0 && !(flags & 8)) { u->kind = K_STORED; u->stored_left = csize; return 0; }
        return fail(u, "zip method");
    }
    u->kind = K_PLAIN;
    return 0;
}

long up_read(up *u, void *out, size_t n) {
    if (u->err_set) return -1;
    if (u->done || !n) return 0;
    if (u->kind == K_UNKNOWN && detect(u)) return -1;
    if (u->kind == K_PLAIN || u->kind == K_STORED) {
        long r = fill(u);
        if (r < 0) return fail(u, "read");
        if (r == 0) { u->done = 1; return 0; }
        size_t k = u->in_len - u->in_pos < n ? u->in_len - u->in_pos : n;
        if (u->kind == K_STORED) {
            if ((long long)k > u->stored_left) k = (size_t)u->stored_left;
            u->stored_left -= (long long)k;
            if (!k) { u->done = 1; return 0; }
        }
        memcpy(out, u->in + u->in_pos, k);
        u->in_pos += k;
        return (long)k;
    }
    u->z.next_out = out;
    u->z.avail_out = (uInt)n;
    while (u->z.avail_out == n) {
        if (u->in_pos >= u->in_len) {
            long r = fill(u);
            if (r < 0) return fail(u, "read");
            if (r == 0) {
                /* конец данных: целый gzip-член уже кончился (Z_STREAM_END) или поток оборван */
                u->done = 1;
                if (u->z.avail_out == n) return 0;
                break;
            }
        }
        u->z.next_in = u->in + u->in_pos;
        u->z.avail_in = (uInt)(u->in_len - u->in_pos);
        int rc = inflate(&u->z, Z_NO_FLUSH);
        u->in_pos = u->in_len - u->z.avail_in;
        if (rc == Z_STREAM_END) {
            if (u->kind == K_ZIP) { u->done = 1; break; }
            /* склеенный gzip: следующий член, если есть */
            if (u->in_pos >= u->in_len && fill(u) <= 0) { u->done = 1; break; }
            if (u->in[u->in_pos] != 0x1f) { u->done = 1; break; }
            inflateReset(&u->z);
            continue;
        }
        if (rc != Z_OK && rc != Z_BUF_ERROR) return fail(u, u->kind == K_ZIP ? "zip" : "gzip");
        if (rc == Z_BUF_ERROR && u->z.avail_in) return fail(u, "gzip");
    }
    return (long)(n - u->z.avail_out);
}

const char *up_error(const up *u) { return u->error; }
const char *up_kind(const up *u) { return u->kind == K_GZIP ? "gzip" : u->kind == K_ZIP || u->kind == K_STORED ? "zip" : "plain"; }
void up_free(up *u) {
    if (!u) return;
    if (u->z_ready) inflateEnd(&u->z);
    free(u);
}
