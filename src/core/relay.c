#define _GNU_SOURCE
#include "relay.h"
#include "http.h"
#include "util.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define PORT 47390
#define PES_MAX (64 * 1024)
#define PLAYLIST_MAX (4 << 20)

/* ================= разбор MPEG-TS ================= */
static struct {
    pthread_mutex_t mu;
    ttx *t;
    uint8_t carry[188];
    int carry_n;
    int pmt_pid[16], npmt;
    int ttx_pid, dvbsub;
    uint8_t *pes;
    size_t pes_n;
    int64_t pes_pts;
    int64_t base_pts;
    long long bytes;
    int gen;   /* растёт при relay_reset: соединения прежней ссылки разбор не трогают */
} D = {.mu = PTHREAD_MUTEX_INITIALIZER, .ttx_pid = -1, .base_pts = -1};

static void init_once(void) {
    if (!D.t) D.t = ttx_new();
    if (!D.pes) D.pes = malloc(PES_MAX);
}

void relay_reset(void) {
    pthread_mutex_lock(&D.mu);
    init_once();
    ttx_reset(D.t);
    D.carry_n = D.npmt = D.dvbsub = 0;
    D.ttx_pid = -1;
    D.pes_n = 0;
    D.base_pts = -1;
    D.bytes = 0;
    D.gen++;
    pthread_mutex_unlock(&D.mu);
}
ttx *relay_ttx(void) { pthread_mutex_lock(&D.mu); init_once(); pthread_mutex_unlock(&D.mu); return D.t; }
long long relay_ts_bytes(void) { return D.bytes; }
int relay_has_teletext(void) { return D.ttx_pid >= 0; }
int relay_has_dvbsub(void) { return D.dvbsub; }
int64_t relay_base_pts(void) { return D.base_pts; }

static int64_t pes_pts(const uint8_t *p, size_t n, size_t *hdr) {
    *hdr = 0;
    if (n < 9 || p[0] || p[1] || p[2] != 1) return -2;
    *hdr = 9 + (size_t)p[8];
    if (*hdr > n) return -2;
    if ((p[7] & 0x80) && n >= 14)
        return ((int64_t)(p[9] & 0x0E) << 29) | ((int64_t)p[10] << 22) | ((int64_t)(p[11] & 0xFE) << 14) | ((int64_t)p[12] << 7) | (p[13] >> 1);
    return -1;
}

static void flush_pes(void) {
    if (D.pes_n) {
        size_t hdr;
        int64_t pts = pes_pts(D.pes, D.pes_n, &hdr);
        if (pts != -2 && hdr < D.pes_n) ttx_feed(D.t, D.pes + hdr, D.pes_n - hdr, pts);
    }
    D.pes_n = 0;
}

/* секция PSI с указателем; tid — ожидаемая таблица */
static const uint8_t *section(const uint8_t *pl, size_t n, int tid, size_t *len) {
    if (n < 1 || (size_t)pl[0] + 1 >= n) return NULL;
    const uint8_t *s = pl + 1 + pl[0];
    size_t rest = n - 1 - pl[0];
    if (rest < 8 || s[0] != tid) return NULL;
    size_t sl = (size_t)((s[1] & 0x0F) << 8 | s[2]);
    if (sl + 3 > rest || sl < 9) return NULL;
    *len = sl + 3 - 4;   /* без CRC */
    return s;
}

static void parse_pat(const uint8_t *pl, size_t n) {
    size_t len;
    const uint8_t *s = section(pl, n, 0x00, &len);
    if (!s) return;
    for (size_t i = 8; i + 4 <= len; i += 4) {
        int prog = s[i] << 8 | s[i + 1], pid = (s[i + 2] & 0x1F) << 8 | s[i + 3];
        if (!prog) continue;
        int dup = 0;
        for (int k = 0; k < D.npmt; k++) if (D.pmt_pid[k] == pid) dup = 1;
        if (!dup && D.npmt < 16) D.pmt_pid[D.npmt++] = pid;
    }
}

static void parse_pmt(const uint8_t *pl, size_t n) {
    size_t len;
    const uint8_t *s = section(pl, n, 0x02, &len);
    if (!s || len < 12) return;
    size_t pil = (size_t)((s[10] & 0x0F) << 8 | s[11]), i = 12 + pil;
    while (i + 5 <= len) {
        int pid = (s[i + 1] & 0x1F) << 8 | s[i + 2];
        size_t el = (size_t)((s[i + 3] & 0x0F) << 8 | s[i + 4]), d = i + 5, end = d + el;
        if (end > len) break;
        while (d + 2 <= end) {
            int tag = s[d], dl = s[d + 1];
            const uint8_t *v = s + d + 2;
            if (d + 2 + (size_t)dl > end) break;
            if ((tag == 0x56 || tag == 0x46) && D.ttx_pid != pid) {
                char lang[4] = "";
                int subs[8], ns = 0;
                for (int k = 0; k + 5 <= dl; k += 5) {
                    if (!lang[0]) { memcpy(lang, v + k, 3); lang[3] = 0; }
                    int type = v[k + 3] >> 3, mag = v[k + 3] & 7;
                    if ((type == 2 || type == 5) && ns < 8) subs[ns++] = ((mag ? mag : 8) << 8) | v[k + 4];
                }
                D.ttx_pid = pid;
                D.pes_n = 0;
                ttx_declare(D.t, lang, subs, ns);
                fprintf(stderr, "relay_teletext pid=%d lang=%s subtitle_pages=%d\n", pid, lang, ns);
            }
            if (tag == 0x59 && !D.dvbsub) { D.dvbsub = 1; fprintf(stderr, "relay_dvbsub pid=%d\n", pid); }
            d += 2 + (size_t)dl;
        }
        i = end;
    }
}

static void packet(const uint8_t *p) {
    int pid = (p[1] & 0x1F) << 8 | p[2], pusi = p[1] & 0x40, afc = (p[3] >> 4) & 3;
    if (!(afc & 1)) return;
    size_t off = 4;
    if (afc & 2) off += 1 + (size_t)p[4];
    if (off >= 188) return;
    const uint8_t *pl = p + off;
    size_t n = 188 - off;
    if (pid == 0) { if (pusi) parse_pat(pl, n); return; }
    for (int k = 0; k < D.npmt; k++) if (pid == D.pmt_pid[k]) { if (pusi) parse_pmt(pl, n); return; }
    /* первая метка времени потока — от неё считается позиция плеера */
    if (pusi && D.base_pts < 0 && pid != D.ttx_pid) {
        size_t hdr;
        int64_t pts = pes_pts(pl, n, &hdr);
        if (pts >= 0 && pl[3] >= 0xC0 && pl[3] <= 0xEF) D.base_pts = pts;   /* видео или звук */
    }
    if (pid != D.ttx_pid) return;
    if (pusi) flush_pes();
    else if (!D.pes_n) return;   /* хвост PES без начала */
    if (D.pes_n + n <= PES_MAX) { memcpy(D.pes + D.pes_n, pl, n); D.pes_n += n; }
}

static void tap_locked(const uint8_t *d, size_t n) {
    init_once();
    D.bytes += (long long)n;
    size_t i = 0;
    if (D.carry_n) {
        size_t need = 188 - (size_t)D.carry_n, k = need < n ? need : n;
        memcpy(D.carry + D.carry_n, d, k);
        D.carry_n += (int)k;
        i = k;
        if (D.carry_n < 188) return;
        if (D.carry[0] == 0x47) packet(D.carry);
        D.carry_n = 0;
    }
    while (i < n) {
        if (d[i] != 0x47) { i++; continue; }   /* потеря синхронизации — искать 0x47 */
        if (n - i < 188) { memcpy(D.carry, d + i, n - i); D.carry_n = (int)(n - i); return; }
        if (n - i > 188 && d[i + 188] != 0x47) { i++; continue; }
        packet(d + i);
        i += 188;
    }
}
void relay_tap(const uint8_t *d, size_t n) {
    pthread_mutex_lock(&D.mu);
    tap_locked(d, n);
    pthread_mutex_unlock(&D.mu);
}

/* ================= HTTP ================= */
static int lfd = -1, port;

static void urlenc(sbuf *b, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~') sb_addc(b, (char)*p);
        else sb_printf(b, "%%%02X", *p);
    }
}
static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static void query_get(const char *q, const char *key, char *out, size_t cap) {
    out[0] = 0;
    size_t kl = strlen(key);
    for (const char *p = q; p && *p;) {
        const char *e = strchr(p, '&');
        if (!e) e = p + strlen(p);
        if ((size_t)(e - p) > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t o = 0;
            for (const char *s = p + kl + 1; s < e && o + 1 < cap; s++) {
                if (*s == '%' && s + 2 < e && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) { out[o++] = (char)(hexv(s[1]) * 16 + hexv(s[2])); s += 2; }
                else out[o++] = *s == '+' ? ' ' : *s;
            }
            out[o] = 0;
            return;
        }
        p = *e ? e + 1 : e;
    }
}

static void make_url(sbuf *b, const char *url, const char *headers) {
    sb_printf(b, "http://127.0.0.1:%d/s?u=", port);
    urlenc(b, url);
    if (headers && *headers) { sb_add(b, "&h="); urlenc(b, headers); }
}

int relay_url(const char *url, const char *headers, char *out, size_t cap) {
    if (!starts_with_ci(url, "http://") && !starts_with_ci(url, "https://")) return -1;
    if (relay_start()) return -1;
    sbuf b = {0};
    make_url(&b, url, headers);
    int ok = b.n < cap;
    if (ok) memcpy(out, b.s, b.n + 1);
    sb_free(&b);
    return ok ? 0 : -1;
}

static int send_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return -1; }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* адрес из плейлиста -> через ретранслятор */
static void rewrite_uri(sbuf *o, const char *base, const char *ref, const char *headers) {
    char abs[4096];
    http_resolve(base, ref, abs, sizeof abs);
    make_url(o, abs, headers);
}

static void rewrite_playlist(sbuf *o, const char *text, size_t n, const char *base, const char *headers) {
    const char *p = text, *end = text + n;
    while (p < end) {
        const char *e = memchr(p, '\n', (size_t)(end - p));
        if (!e) e = end;
        size_t l = (size_t)(e - p);
        if (l && p[l - 1] == '\r') l--;
        char *line = xstrndup(p, l);
        char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        if (!*t) sb_add(o, line);
        else if (*t == '#') {
            /* URI="…" в тегах (ключи, карты, варианты звука) */
            char *u = strstr(t, "URI=\"");
            if (u) {
                char *q = strchr(u + 5, '"');
                if (q) {
                    sb_addn(o, line, (size_t)(u + 5 - line));
                    *q = 0;
                    rewrite_uri(o, base, u + 5, headers);
                    sb_addc(o, '"');
                    sb_add(o, q + 1);
                } else sb_add(o, line);
            } else sb_add(o, line);
        } else rewrite_uri(o, base, t, headers);
        sb_addc(o, '\n');
        free(line);
        p = e < end ? e + 1 : end;
    }
}

static void reply_status(int fd, int code) {
    char b[160];
    snprintf(b, sizeof b, "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", code, code == 404 ? "Not Found" : "Bad Gateway");
    send_all(fd, b, strlen(b));
}

static int is_playlist(const char *s, size_t n) {
    size_t i = 0;
    if (n >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) i = 3;
    while (i < n && (s[i] == ' ' || s[i] == '\r' || s[i] == '\n')) i++;
    return n - i >= 7 && !strncmp(s + i, "#EXTM3U", 7);
}

static void *client(void *arg) {
    int fd = (int)(intptr_t)arg;
    struct timeval tv = {15, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char req[16384];
    size_t rl = 0;
    while (rl + 1 < sizeof req) {
        ssize_t r = recv(fd, req + rl, sizeof req - 1 - rl, 0);
        if (r <= 0) break;
        rl += (size_t)r;
        req[rl] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    req[rl] = 0;
    char path[12000] = "";
    if (sscanf(req, "GET %11999s", path) != 1 || strncmp(path, "/s?", 3)) { reply_status(fd, 404); close(fd); return NULL; }
    char *url = malloc(4096), *hdr = malloc(4096);
    query_get(path + 3, "u", url, 4096);
    query_get(path + 3, "h", hdr, 4096);
    pthread_mutex_lock(&D.mu);
    int gen = D.gen;
    pthread_mutex_unlock(&D.mu);
    volatile int cancel = 0;
    hs_req rq = {.url = url, .headers = hdr[0] ? hdr : NULL, .timeout_ms = 20000, .cancel = &cancel};
    char err[120] = "";
    hs *h = hs_open(&rq, err, sizeof err);
    if (!h) {
        reply_status(fd, !strncmp(err, "http_", 5) ? atoi(err + 5) : 502);
        fprintf(stderr, "relay_open_failed %s\n", strncmp(err, "http_", 5) ? "network" : err);
        free(url); free(hdr); close(fd);
        return NULL;
    }
    char *buf = malloc(65536);
    long n = buf ? hs_read(h, buf, 65536) : -1;
    if (n > 0 && is_playlist(buf, (size_t)n)) {
        sbuf body = {0}, out = {0};
        sb_addn(&body, buf, (size_t)n);
        while ((n = hs_read(h, buf, 65536)) > 0 && body.n < PLAYLIST_MAX) sb_addn(&body, buf, (size_t)n);
        rewrite_playlist(&out, body.s, body.n, hs_url(h), hdr);
        char head[200];
        snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", out.n);
        if (!send_all(fd, head, strlen(head))) send_all(fd, out.s ? out.s : "", out.n);
        sb_free(&body);
        sb_free(&out);
    } else if (n >= 0) {
        char head[200];
        long long len = hs_length(h);
        if (len > 0) snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %lld\r\nConnection: close\r\n\r\n", len);
        else snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nConnection: close\r\n\r\n");
        int ok = !send_all(fd, head, strlen(head));
        while (ok && n > 0) {
            pthread_mutex_lock(&D.mu);
            if (D.gen == gen) tap_locked((const uint8_t *)buf, (size_t)n);   /* прежняя ссылка — не разбирать */
            pthread_mutex_unlock(&D.mu);
            if (send_all(fd, buf, (size_t)n)) break;   /* плеер закрыл соединение */
            n = hs_read(h, buf, 65536);
        }
    }
    free(buf);
    hs_close(h);
    free(url);
    free(hdr);
    close(fd);
    return NULL;
}

static void *acceptor(void *arg) {
    (void)arg;
    for (;;) {
        int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) { if (errno == EINTR || errno == ECONNABORTED) continue; usleep(100000); continue; }
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&a, 256 * 1024);
        if (pthread_create(&t, &a, client, (void *)(intptr_t)fd)) close(fd);
        pthread_attr_destroy(&a);
    }
    return NULL;
}

int relay_start(void) {
    if (lfd >= 0) return 0;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    for (int p = PORT; p < PORT + 10 && !port; p++) {
        struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)p)};
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (!bind(fd, (struct sockaddr *)&sa, sizeof sa)) port = p;
    }
    if (!port || listen(fd, 16)) { close(fd); port = 0; return -1; }
    lfd = fd;
    pthread_mutex_lock(&D.mu);
    init_once();
    pthread_mutex_unlock(&D.mu);
    pthread_t t;
    if (pthread_create(&t, NULL, acceptor, NULL)) { close(lfd); lfd = -1; port = 0; return -1; }
    pthread_detach(t);
    fprintf(stderr, "relay_start port=%d\n", port);
    return 0;
}
