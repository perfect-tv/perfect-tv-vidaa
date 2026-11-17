#define _GNU_SOURCE
#include "http.h"
#include "clock.h"
#include "prefs.h"
#include "util.h"
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef PTV_VERSION
#define PTV_VERSION "dev"
#endif

static SSL_CTX *ctx_verify, *ctx_any;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static const char *ca_mem;
static size_t ca_len;

static void init_ctx(void) {
    OPENSSL_init_ssl(0, NULL);
    ctx_verify = SSL_CTX_new(TLS_client_method());
    ctx_any = SSL_CTX_new(TLS_client_method());
    if (!ctx_verify || !ctx_any) return;
    SSL_CTX_set_verify(ctx_verify, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_verify(ctx_any, SSL_VERIFY_NONE, NULL);
    /* У провайдеров встречаются старые серверы: TLS 1.0 оставлен для «любых сертификатов». */
    SSL_CTX_set_min_proto_version(ctx_verify, TLS1_2_VERSION);
    SSL_CTX_set_security_level(ctx_any, 0);
    int n = 0;
    if (ca_mem && ca_len) {
        BIO *bio = BIO_new_mem_buf(ca_mem, (int)ca_len);
        X509_STORE *st = SSL_CTX_get_cert_store(ctx_verify);
        X509 *x;
        while (bio && (x = PEM_read_bio_X509(bio, NULL, NULL, NULL))) { if (X509_STORE_add_cert(st, x) == 1) n++; X509_free(x); }
        BIO_free(bio);
        ERR_clear_error();
    }
    if (!n) SSL_CTX_set_default_verify_paths(ctx_verify);
    fprintf(stderr, "http_ca=%d\n", n);
}
void http_init(const char *pem, size_t len) { ca_mem = pem; ca_len = len; pthread_once(&once, init_ctx); }

struct hs {
    int fd; SSL *ssl; FILE *file;
    int timeout, status, chunked, eof;
    long long length, received, chunk_left;
    char etag[160], last_modified[64], location[2048], error[120];
    char final_url[4096];         /* после редиректов (база для относительных адресов HLS) */
    volatile int *cancel;
    /* буфер чтения сырых байтов */
    char buf[16384]; size_t bpos, blen;
};

static int cancelled(hs *h) { return h->cancel && *h->cancel; }

static int wait_fd(hs *h, int ev, int ms) {
    struct pollfd p = {h->fd, (short)ev, 0};
    double end = mono_now() + ms / 1000.0;
    while (!cancelled(h)) {
        int left = (int)((end - mono_now()) * 1000);
        if (left <= 0) return 0;
        int r = poll(&p, 1, left > 200 ? 200 : left);
        if (r > 0) return r;
        if (r < 0 && errno != EINTR) return -1;
    }
    return -1;
}

static long raw_read(hs *h, char *b, size_t n) {
    for (;;) {
        if (cancelled(h)) { snprintf(h->error, sizeof h->error, "cancelled"); return -1; }
        int r = h->ssl ? SSL_read(h->ssl, b, (int)n) : (int)recv(h->fd, b, n, 0);
        if (r > 0) return r;
        if (h->ssl) {
            int e = SSL_get_error(h->ssl, r);
            if (e == SSL_ERROR_ZERO_RETURN) return 0;
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
                if (wait_fd(h, e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, h->timeout) <= 0) { snprintf(h->error, sizeof h->error, "timeout"); return -1; }
                continue;
            }
            if (e == SSL_ERROR_SYSCALL && r == 0) return 0;
            snprintf(h->error, sizeof h->error, "tls read");
            return -1;
        }
        if (r == 0) return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (wait_fd(h, POLLIN, h->timeout) <= 0) { snprintf(h->error, sizeof h->error, "timeout"); return -1; }
            continue;
        }
        if (errno == EINTR) continue;
        snprintf(h->error, sizeof h->error, "read: %s", strerror(errno));
        return -1;
    }
}

static int raw_write(hs *h, const char *b, size_t n) {
    size_t off = 0;
    while (off < n) {
        if (cancelled(h)) return -1;
        int r = h->ssl ? SSL_write(h->ssl, b + off, (int)(n - off)) : (int)send(h->fd, b + off, n - off, MSG_NOSIGNAL);
        if (r > 0) { off += (size_t)r; continue; }
        if (h->ssl) {
            int e = SSL_get_error(h->ssl, r);
            if ((e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) && wait_fd(h, e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, h->timeout) > 0) continue;
            return -1;
        }
        if ((errno == EAGAIN || errno == EWOULDBLOCK) && wait_fd(h, POLLOUT, h->timeout) > 0) continue;
        if (errno == EINTR) continue;
        return -1;
    }
    return 0;
}

/* байт из буфера; -1 конец/ошибка */
static int getbyte(hs *h) {
    if (h->bpos >= h->blen) {
        long r = raw_read(h, h->buf, sizeof h->buf);
        if (r <= 0) return -1;
        h->bpos = 0;
        h->blen = (size_t)r;
    }
    return (unsigned char)h->buf[h->bpos++];
}
static int getline_(hs *h, char *out, size_t cap) {
    size_t n = 0;
    for (;;) {
        int c = getbyte(h);
        if (c < 0) { if (!n) return -1; break; }
        if (c == '\n') break;
        if (n + 1 < cap) out[n++] = (char)c;
    }
    while (n && (out[n - 1] == '\r' || out[n - 1] == ' ')) n--;
    out[n] = 0;
    return (int)n;
}

static void conn_close(hs *h) {
    if (h->ssl) { SSL_free(h->ssl); h->ssl = NULL; }
    if (h->fd >= 0) { close(h->fd); h->fd = -1; }
}

typedef struct { char scheme[8], host[256], port[8], path[4096], auth[300]; } url_t;

static void b64(const char *in, char *out, size_t cap) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(in), o = 0;
    for (size_t i = 0; i < n && o + 5 < cap; i += 3) {
        unsigned v = (unsigned char)in[i] << 16 | (i + 1 < n ? (unsigned char)in[i + 1] << 8 : 0) | (i + 2 < n ? (unsigned char)in[i + 2] : 0);
        out[o++] = T[v >> 18 & 63];
        out[o++] = T[v >> 12 & 63];
        out[o++] = i + 1 < n ? T[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    out[o] = 0;
}

static int pct(int c) { return isxdigit(c) ? (isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10)) : -1; }
static void unpct(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && pct((unsigned char)s[1]) >= 0 && pct((unsigned char)s[2]) >= 0) { *o++ = (char)(pct((unsigned char)s[1]) * 16 + pct((unsigned char)s[2])); s += 2; }
        else *o++ = *s;
    }
    *o = 0;
}

static int parse_url(const char *u, url_t *p) {
    memset(p, 0, sizeof *p);
    const char *s = strstr(u, "://");
    if (!s || s - u >= (long)sizeof p->scheme) return -1;
    for (int i = 0; i < s - u; i++) p->scheme[i] = (char)tolower((unsigned char)u[i]);
    if (strcmp(p->scheme, "http") && strcmp(p->scheme, "https")) return -1;
    s += 3;
    const char *e = s;
    while (*e && *e != '/' && *e != '?' && *e != '#') e++;
    const char *at = NULL;
    for (const char *q = s; q < e; q++) if (*q == '@') at = q;
    if (at) {
        char ui[200];
        size_t n = (size_t)(at - s);
        if (n >= sizeof ui) n = sizeof ui - 1;
        memcpy(ui, s, n);
        ui[n] = 0;
        unpct(ui);
        b64(ui, p->auth, sizeof p->auth);
        s = at + 1;
    }
    const char *colon = NULL;
    if (*s == '[') {
        const char *rb = memchr(s, ']', (size_t)(e - s));
        if (!rb || (size_t)(rb - s - 1) >= sizeof p->host) return -1;
        memcpy(p->host, s + 1, (size_t)(rb - s - 1));
        colon = rb + 1 < e && rb[1] == ':' ? rb + 1 : NULL;
    } else {
        colon = memchr(s, ':', (size_t)(e - s));
        const char *he = colon ? colon : e;
        if (he == s || (size_t)(he - s) >= sizeof p->host) return -1;
        memcpy(p->host, s, (size_t)(he - s));
    }
    if (colon) {
        size_t n = (size_t)(e - colon - 1);
        if (!n || n >= sizeof p->port) return -1;
        memcpy(p->port, colon + 1, n);
    } else strcpy(p->port, strcmp(p->scheme, "https") ? "80" : "443");
    const char *path = *e ? e : "/";
    const char *hash = strchr(path, '#');
    size_t pl = hash ? (size_t)(hash - path) : strlen(path);
    if (pl + 2 > sizeof p->path) return -1;
    if (*path == '?') { p->path[0] = '/'; memcpy(p->path + 1, path, pl); p->path[pl + 1] = 0; }
    else { memcpy(p->path, path, pl); p->path[pl] = 0; }
    /* пробелы в пути у самодельных серверов */
    if (strchr(p->path, ' ')) {
        char t[4096]; size_t o = 0;
        for (const char *q = p->path; *q && o + 4 < sizeof t; q++) { if (*q == ' ') { memcpy(t + o, "%20", 3); o += 3; } else t[o++] = *q; }
        t[o] = 0;
        snprintf(p->path, sizeof p->path, "%s", t);
    }
    return 0;
}

static void resolve(const char *base, const char *loc, char *out, size_t cap) {
    if (strstr(loc, "://")) { snprintf(out, cap, "%s", loc); return; }
    url_t b;
    if (parse_url(base, &b)) { snprintf(out, cap, "%s", loc); return; }
    int def = (!strcmp(b.scheme, "https") && !strcmp(b.port, "443")) || (!strcmp(b.scheme, "http") && !strcmp(b.port, "80"));
    char hostport[300];
    snprintf(hostport, sizeof hostport, def ? "%s" : "%s:%s", b.host, b.port);
    if (loc[0] == '/' && loc[1] == '/') { snprintf(out, cap, "%s:%s", b.scheme, loc); return; }
    if (loc[0] == '/') { snprintf(out, cap, "%s://%s%s", b.scheme, hostport, loc); return; }
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", b.path);
    char *q = strchr(dir, '?');
    if (q) *q = 0;
    char *sl = strrchr(dir, '/');
    if (sl) sl[1] = 0;
    snprintf(out, cap, "%s://%s%s%s", b.scheme, hostport, dir, loc);
}

static int conn_open(hs *h, const url_t *u) {
    struct addrinfo hints = {0}, *res = NULL, *ai;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(u->host, u->port, &hints, &res);
    if (rc) { snprintf(h->error, sizeof h->error, "dns: %s", gai_strerror(rc)); return -1; }
    h->fd = -1;
    for (ai = res; ai && !cancelled(h); ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) continue;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        int r = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (r < 0 && errno == EINPROGRESS) {
            h->fd = fd;
            if (wait_fd(h, POLLOUT, 10000) > 0) {
                int e = 0; socklen_t l = sizeof e;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l);
                r = e ? -1 : 0;
            } else r = -1;
            h->fd = -1;
        }
        if (r == 0) { h->fd = fd; break; }
        close(fd);
    }
    freeaddrinfo(res);
    if (h->fd < 0) { snprintf(h->error, sizeof h->error, cancelled(h) ? "cancelled" : "connect failed"); return -1; }
    if (!strcmp(u->scheme, "https")) {
        int any = P.any_cert;
        SSL_CTX *c = any ? ctx_any : ctx_verify;
        if (!c) { snprintf(h->error, sizeof h->error, "tls: init"); return -1; }
        h->ssl = SSL_new(c);
        SSL_set_fd(h->ssl, h->fd);
        SSL_set_tlsext_host_name(h->ssl, u->host);
        if (!any) X509_VERIFY_PARAM_set1_host(SSL_get0_param(h->ssl), u->host, 0);
        for (;;) {
            int r = SSL_connect(h->ssl);
            if (r == 1) break;
            int e = SSL_get_error(h->ssl, r);
            if ((e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) && wait_fd(h, e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, h->timeout) > 0) continue;
            long vr = SSL_get_verify_result(h->ssl);
            if (vr != X509_V_OK) snprintf(h->error, sizeof h->error, "tls: %s", X509_verify_cert_error_string(vr));
            else { const char *why = ERR_reason_error_string(ERR_peek_last_error()); snprintf(h->error, sizeof h->error, "tls: %s", why ? why : "handshake"); }
            ERR_clear_error();
            return -1;
        }
    }
    return 0;
}

static int header_is(const char *line, const char *name, const char **val) {
    size_t n = strlen(name);
    if (strncasecmp(line, name, n) || line[n] != ':') return 0;
    const char *v = line + n + 1;
    while (*v == ' ' || *v == '\t') v++;
    *val = v;
    return 1;
}

static int request(hs *h, const char *url, const hs_req *rq) {
    url_t u;
    if (parse_url(url, &u)) { snprintf(h->error, sizeof h->error, "bad url"); return -1; }
    if (conn_open(h, &u)) return -1;
    sbuf b = {0};
    sb_printf(&b, "GET %s HTTP/1.1\r\nHost: %s", u.path, u.host);
    if ((!strcmp(u.scheme, "https") && strcmp(u.port, "443")) || (!strcmp(u.scheme, "http") && strcmp(u.port, "80"))) sb_printf(&b, ":%s", u.port);
    sb_add(&b, "\r\n");
    int has_ua = rq->headers && strcasestr(rq->headers, "user-agent:");
    if (!has_ua) sb_add(&b, "User-Agent: PerfectTV/" PTV_VERSION " (VIDAA; native)\r\n");
    sb_add(&b, "Accept: */*\r\nAccept-Encoding: gzip\r\nConnection: close\r\n");
    if (u.auth[0]) sb_printf(&b, "Authorization: Basic %s\r\n", u.auth);
    if (rq->etag && *rq->etag) sb_printf(&b, "If-None-Match: %s\r\n", rq->etag);
    if (rq->last_modified && *rq->last_modified) sb_printf(&b, "If-Modified-Since: %s\r\n", rq->last_modified);
    if (rq->headers) sb_add(&b, rq->headers);
    sb_add(&b, "\r\n");
    int rc = raw_write(h, b.s, b.n);
    sb_free(&b);
    if (rc) { snprintf(h->error, sizeof h->error, "write failed"); return -1; }
    char line[4200];
    if (getline_(h, line, sizeof line) < 0) { if (!h->error[0]) snprintf(h->error, sizeof h->error, "no response"); return -1; }
    if (strncmp(line, "HTTP/", 5) || !strchr(line, ' ')) { snprintf(h->error, sizeof h->error, "bad response"); return -1; }
    h->status = atoi(strchr(line, ' ') + 1);
    h->length = -1;
    h->chunked = 0;
    h->location[0] = 0;
    for (;;) {
        int n = getline_(h, line, sizeof line);
        if (n < 0) { snprintf(h->error, sizeof h->error, "headers cut"); return -1; }
        if (n == 0) break;
        const char *v;
        if (header_is(line, "Content-Length", &v)) h->length = atoll(v);
        else if (header_is(line, "Transfer-Encoding", &v)) h->chunked = strcasestr(v, "chunked") != NULL;
        else if (header_is(line, "Location", &v)) resolve(url, v, h->location, sizeof h->location);
        else if (header_is(line, "ETag", &v)) snprintf(h->etag, sizeof h->etag, "%s", v);
        else if (header_is(line, "Last-Modified", &v)) snprintf(h->last_modified, sizeof h->last_modified, "%s", v);
        else if (header_is(line, "Date", &v)) clock_server(http_date(v));
    }
    if (h->chunked) h->length = -1;
    h->chunk_left = h->chunked ? -1 : 0;
    return 0;
}

hs *hs_open(const hs_req *rq, char *err, size_t errn) {
    hs *h = calloc(1, sizeof *h);
    if (!h) { snprintf(err, errn, "no memory"); return NULL; }
    h->fd = -1;
    h->timeout = rq->timeout_ms > 0 ? rq->timeout_ms : 30000;
    h->cancel = rq->cancel;
    const char *u = rq->url;
    if (!strncasecmp(u, "file://", 7) || u[0] == '/') {   /* файл на ТВ (USB) */
        const char *path = u[0] == '/' ? u : u + 7;
        h->file = fopen(path, "rb");
        if (!h->file) { snprintf(err, errn, "file: %s", strerror(errno)); free(h); return NULL; }
        h->status = 200;
        h->length = file_size(path);
        snprintf(h->final_url, sizeof h->final_url, "%s", u);
        return h;
    }
    http_init(NULL, 0);
    char cur[4096];
    snprintf(cur, sizeof cur, "%s", u);
    for (int hop = 0; hop < 8; hop++) {
        h->error[0] = 0;
        if (request(h, cur, rq)) { snprintf(err, errn, "%s", h->error); conn_close(h); free(h); return NULL; }
        if (h->status >= 300 && h->status < 400 && h->status != 304 && h->location[0]) {
            conn_close(h);
            h->bpos = h->blen = 0;
            snprintf(cur, sizeof cur, "%s", h->location);
            continue;
        }
        if (h->status == 200 || h->status == 304 || h->status == 206) { snprintf(h->final_url, sizeof h->final_url, "%s", cur); return h; }
        snprintf(err, errn, "http_%d", h->status);
        conn_close(h);
        free(h);
        return NULL;
    }
    snprintf(err, errn, "too many redirects");
    conn_close(h);
    free(h);
    return NULL;
}

int hs_status(const hs *h) { return h->status; }
const char *hs_url(const hs *h) { return h->final_url; }
void http_resolve(const char *base, const char *ref, char *out, size_t cap) { resolve(base, ref, out, cap); }
long long hs_length(const hs *h) { return h->length; }
long long hs_received(const hs *h) { return h->received; }
const char *hs_etag(const hs *h) { return h->etag; }
const char *hs_last_modified(const hs *h) { return h->last_modified; }
const char *hs_error(const hs *h) { return h->error; }

/* тело без chunked: сначала остаток буфера заголовков */
static long body_raw(hs *h, char *out, size_t n) {
    if (h->bpos < h->blen) {
        size_t k = MIN(n, h->blen - h->bpos);
        memcpy(out, h->buf + h->bpos, k);
        h->bpos += k;
        return (long)k;
    }
    return raw_read(h, out, n);
}

long hs_read(hs *h, void *buf, size_t n) {
    if (h->eof || !n) return 0;
    if (h->file) {
        if (cancelled(h)) { snprintf(h->error, sizeof h->error, "cancelled"); return -1; }
        size_t r = fread(buf, 1, n, h->file);
        if (!r) { h->eof = 1; return ferror(h->file) ? -1 : 0; }
        h->received += (long long)r;
        return (long)r;
    }
    if (h->status == 304) return 0;
    if (h->chunked) {
        if (h->chunk_left <= 0) {
            char line[64];
            if (h->chunk_left == 0 && getline_(h, line, sizeof line) < 0) return -1;   /* CRLF после куска */
            if (getline_(h, line, sizeof line) < 0) { snprintf(h->error, sizeof h->error, "chunk cut"); return -1; }
            h->chunk_left = strtoll(line, NULL, 16);
            if (h->chunk_left <= 0) { h->eof = 1; return 0; }
        }
        if ((long long)n > h->chunk_left) n = (size_t)h->chunk_left;
        long r = body_raw(h, buf, n);
        if (r <= 0) { if (!h->error[0]) snprintf(h->error, sizeof h->error, "cut"); return -1; }
        h->chunk_left -= r;
        h->received += r;
        return r;
    }
    if (h->length >= 0) {
        long long left = h->length - h->received;
        if (left <= 0) { h->eof = 1; return 0; }
        if ((long long)n > left) n = (size_t)left;
    }
    long r = body_raw(h, buf, n);
    if (r == 0) {
        h->eof = 1;
        if (h->length >= 0 && h->received < h->length) { snprintf(h->error, sizeof h->error, "cut"); return -1; }
        return 0;
    }
    if (r > 0) h->received += r;
    return r;
}

void hs_close(hs *h) {
    if (!h) return;
    if (h->file) fclose(h->file);
    conn_close(h);
    free(h);
}

char *http_get_small(const char *url, size_t max, size_t *len, char *err, size_t errn) {
    hs_req rq = {.url = url, .timeout_ms = 15000};
    hs *h = hs_open(&rq, err, errn);
    if (!h) return NULL;
    sbuf b = {0};
    char tmp[8192];
    long r;
    while ((r = hs_read(h, tmp, sizeof tmp)) > 0) {
        if (b.n + (size_t)r > max) { snprintf(err, errn, "too big"); r = -1; break; }
        sb_addn(&b, tmp, (size_t)r);
    }
    if (r < 0 && !err[0]) snprintf(err, errn, "%s", hs_error(h));
    hs_close(h);
    if (r < 0) { sb_free(&b); return NULL; }
    if (len) *len = b.n;
    return sb_take(&b);
}

int64_t http_date(const char *s) {
    static const char *mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int d, y, H, M, S;
    char m[4];
    const char *c = strchr(s, ',');
    if (!c) return 0;
    if (sscanf(c + 1, " %d %3s %d %d:%d:%d", &d, m, &y, &H, &M, &S) != 6) return 0;
    const char *p = strstr(mon, m);
    if (!p) return 0;
    struct tm tm = {0};
    tm.tm_mday = d;
    tm.tm_mon = (int)(p - mon) / 3;
    tm.tm_year = y - 1900;
    tm.tm_hour = H;
    tm.tm_min = M;
    tm.tm_sec = S;
    return (int64_t)timegm(&tm);
}
