#define _GNU_SOURCE
#include "phone.h"
#include "lang.h"
#include "prefs.h"
#include "util.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PORT 47381
#define MAX_CONN 8
#define REQ_MAX 16384
#define LIFETIME 600.0      /* с: страница живёт 10 минут */
#define CONN_TIMEOUT 8.0

typedef struct { int fd; char *buf; size_t len; double t0; } conn;
static struct {
    int lfd;
    int port, kind;
    char token[24];
    double t_start;
    conn c[MAX_CONN];
} S = {.lfd = -1};

int phone_running(void) { return S.lfd >= 0; }

static void conn_close(conn *c) {
    if (c->fd >= 0) close(c->fd);
    free(c->buf);
    c->fd = -1;
    c->buf = NULL;
    c->len = 0;
}

void phone_stop(void) {
    for (int i = 0; i < MAX_CONN; i++) if (S.c[i].fd > 0 || S.c[i].buf) conn_close(&S.c[i]);
    if (S.lfd >= 0) close(S.lfd);
    S.lfd = -1;
}

/* Предпочтение: проводная/беспроводная сеть, затем любая не петля; мосты и виртуальные — в последнюю очередь. */
int phone_lan_ip(char *out, size_t cap) {
    struct ifaddrs *ifs = NULL, *a;
    if (getifaddrs(&ifs)) return -1;
    int best = -1;
    for (a = ifs; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
        const char *n = a->ifa_name ? a->ifa_name : "";
        int score = !strncmp(n, "eth", 3) || !strncmp(n, "wlan", 4) || !strncmp(n, "en", 2) || !strncmp(n, "wl", 2) ? 3
                  : !strncmp(n, "docker", 6) || !strncmp(n, "veth", 4) || !strncmp(n, "br-", 3) || !strncmp(n, "virbr", 5) ? 1 : 2;
        if (score <= best) continue;
        char ip[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &((struct sockaddr_in *)a->ifa_addr)->sin_addr, ip, sizeof ip)) continue;
        if (!strncmp(ip, "169.254.", 8)) continue;
        snprintf(out, cap, "%s", ip);
        best = score;
    }
    freeifaddrs(ifs);
    return best >= 0 ? 0 : -1;
}

static void make_token(char *out) {
    unsigned char r[8] = {0};
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, r, sizeof r) : -1;
    if (fd >= 0) close(fd);
    if (n != (ssize_t)sizeof r) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint32_t h = hash_bytes(&ts, sizeof ts) ^ (uint32_t)getpid();
        for (int i = 0; i < 8; i++) { h = h * 1103515245u + 12345u; r[i] ^= (unsigned char)(h >> 16); }
    }
    for (int i = 0; i < 8; i++) sprintf(out + i * 2, "%02x", r[i]);
}

int phone_start(int kind, char *url, size_t cap, char *err, size_t errn) {
    phone_stop();
    S.kind = kind;
    char ip[INET_ADDRSTRLEN];
    if (phone_lan_ip(ip, sizeof ip)) { snprintf(err, errn, "no_network"); return -1; }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { snprintf(err, errn, "socket: %s", strerror(errno)); return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    int port = 0;
    for (int p = PORT; p < PORT + 10 && !port; p++) {
        struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)p), .sin_addr.s_addr = htonl(INADDR_ANY)};
        if (!bind(fd, (struct sockaddr *)&sa, sizeof sa)) port = p;
    }
    if (!port || listen(fd, 8)) { snprintf(err, errn, "bind: %s", strerror(errno)); close(fd); return -1; }
    S.lfd = fd;
    S.port = port;
    for (int i = 0; i < MAX_CONN; i++) { S.c[i].fd = -1; S.c[i].buf = NULL; S.c[i].len = 0; }
    make_token(S.token);
    S.t_start = mono_now();
    snprintf(url, cap, "http://%s:%d/%s", ip, port, S.token);
    fprintf(stderr, "phone_start port=%d\n", port);
    return 0;
}

/* ---------- HTTP ---------- */
static void html_esc(sbuf *b, const char *s) {
    for (; s && *s; s++) {
        if (*s == '&') sb_add(b, "&amp;");
        else if (*s == '<') sb_add(b, "&lt;");
        else if (*s == '>') sb_add(b, "&gt;");
        else if (*s == '"') sb_add(b, "&quot;");
        else sb_addc(b, *s);
    }
}

static void send_all(int fd, const char *p, size_t n) {
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return; }
        p += w;
        n -= (size_t)w;
    }
}

static void respond(int fd, int code, const char *body) {
    sbuf h = {0};
    sb_printf(&h, "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\n", code,
              code == 200 ? "OK" : code == 404 ? "Not Found" : "Bad Request", strlen(body));
    sb_add(&h, "Cache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\n"
               "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; base-uri 'none'\r\n"
               "Connection: close\r\n\r\n");
    sb_add(&h, body);
    send_all(fd, h.s, h.n);
    sb_free(&h);
}

static void page(sbuf *b, const char *msg, int form) {
    sb_printf(&b[0], "<!doctype html><html lang=\"%s\"><head><meta charset=\"utf-8\">"
                     "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Perfect TV</title><style>",
              lang_is_ru() ? "ru" : "en");
    sb_add(b, "body{box-sizing:border-box;margin:0;padding:32px 18px;background:#141515;color:#ebebeb;font:17px system-ui,sans-serif}"
              "main{max-width:520px;margin:0 auto}h1{font-size:28px;margin:0 0 12px}p{color:#bbc3c8;line-height:1.5}"
              ".m{color:#ffb84d}label{display:block;margin:20px 0 7px}"
              "input{width:100%;box-sizing:border-box;background:#292c30;border:1px solid #62666a;padding:14px;border-radius:8px;color:#fff;font:16px sans-serif}"
              "button{width:100%;margin-top:26px;padding:15px;border:0;border-radius:8px;background:#ebebeb;color:#101010;font:700 17px sans-serif}"
              "</style></head><body><main><h1>Perfect TV</h1>");
    if (msg) { sb_add(b, "<p class=\"m\">"); html_esc(b, msg); sb_add(b, "</p>"); }
    if (form && S.kind == PHONE_TMDB) {
        sb_add(b, "<p>");
        html_esc(b, tr(S_PH_TMDB_INTRO));
        sb_printf(b, "</p><form action=\"/%s\" method=\"post\" accept-charset=\"utf-8\"><label for=\"key\">", S.token);
        html_esc(b, tr(S_TMDB_KEY));
        sb_add(b, "</label><input id=\"key\" name=\"key\" required maxlength=\"290\" autocomplete=\"off\" autofocus><label for=\"api\">");
        html_esc(b, tr(S_TMDB_API));
        sb_add(b, "</label><input id=\"api\" name=\"api\" type=\"url\" maxlength=\"290\" value=\"");
        html_esc(b, P.tmdb_api[0] ? P.tmdb_api : "https://api.themoviedb.org/3");
        sb_add(b, "\"><label for=\"img\">");
        html_esc(b, tr(S_TMDB_IMG));
        sb_add(b, "</label><input id=\"img\" name=\"img\" type=\"url\" maxlength=\"290\" value=\"");
        html_esc(b, P.tmdb_img[0] ? P.tmdb_img : "https://image.tmdb.org");
        sb_add(b, "\"><button type=\"submit\">");
        html_esc(b, tr(S_PH_PAGE_SEND));
        sb_add(b, "</button></form>");
    } else if (form) {
        sb_add(b, "<p>");
        html_esc(b, tr(S_PH_PAGE_INTRO));
        sb_printf(b, "</p><form action=\"/%s\" method=\"post\" accept-charset=\"utf-8\">", S.token);
        sb_add(b, "<label for=\"url\">");
        html_esc(b, tr(S_PH_PAGE_URL));
        sb_add(b, "</label><input id=\"url\" name=\"url\" type=\"url\" inputmode=\"url\" placeholder=\"https://example.com/playlist.m3u\" "
                  "required maxlength=\"2000\" autofocus><label for=\"name\">");
        html_esc(b, tr(S_PH_PAGE_NAME));
        sb_add(b, "</label><input id=\"name\" name=\"name\" maxlength=\"100\"><label for=\"epg\">");
        html_esc(b, tr(S_PH_PAGE_EPG));
        sb_add(b, "</label><input id=\"epg\" name=\"epg\" type=\"url\" inputmode=\"url\" placeholder=\"https://example.com/epg.xml.gz\" "
                  "maxlength=\"2000\"><button type=\"submit\">");
        html_esc(b, tr(S_PH_PAGE_SEND));
        sb_add(b, "</button></form>");
    }
    sb_add(b, "</main></body></html>");
}

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
/* значение поля key из application/x-www-form-urlencoded */
static void form_get(const char *body, size_t n, const char *key, char *out, size_t cap) {
    out[0] = 0;
    size_t kl = strlen(key), i = 0;
    while (i < n) {
        size_t e = i;
        while (e < n && body[e] != '&') e++;
        if (e - i > kl && !strncmp(body + i, key, kl) && body[i + kl] == '=') {
            size_t o = 0;
            for (size_t k = i + kl + 1; k < e && o + 1 < cap; k++) {
                int ch = body[k];
                if (ch == '+') ch = ' ';
                else if (ch == '%' && k + 2 < e && hexv(body[k + 1]) >= 0 && hexv(body[k + 2]) >= 0) {
                    ch = hexv(body[k + 1]) * 16 + hexv(body[k + 2]);
                    k += 2;
                }
                out[o++] = (char)ch;
            }
            out[o] = 0;
            trim(out);
            return;
        }
        i = e + 1;
    }
}
static int valid_url(const char *u) {
    if (!starts_with_ci(u, "http://") && !starts_with_ci(u, "https://")) return 0;
    for (const unsigned char *p = (const unsigned char *)u; *p; p++) if (*p < 0x21 || *p == 0x7f) return 0;
    return strlen(u) > 8;
}

/* 1 — запрос полный и обработан; r заполнен, если пришёл адрес */
static int handle(conn *c, phone_result *r, int *got) {
    char *he = memmem(c->buf, c->len, "\r\n\r\n", 4);
    if (!he) return c->len >= REQ_MAX ? (respond(c->fd, 400, ""), 1) : 0;
    size_t hl = (size_t)(he - c->buf) + 4;
    char method[8] = "", path[200] = "";
    if (sscanf(c->buf, "%7s %199s", method, path) != 2) { respond(c->fd, 400, ""); return 1; }
    size_t blen = 0;
    for (char *p = c->buf; p < he; p++) {
        if ((p == c->buf || p[-1] == '\n') && !strncasecmp(p, "Content-Length:", 15)) blen = (size_t)strtoul(p + 15, NULL, 10);
    }
    if (blen > REQ_MAX - hl) { respond(c->fd, 400, ""); return 1; }
    if (c->len < hl + blen) return 0;
    char want[40];
    snprintf(want, sizeof want, "/%s", S.token);
    char *q = strchr(path, '?');
    if (q) *q = 0;
    sbuf b = {0};
    if (strcmp(path, want)) {
        respond(c->fd, 404, "");
    } else if (!strcmp(method, "GET")) {
        page(&b, NULL, 1);
        respond(c->fd, 200, b.s);
    } else if (!strcmp(method, "POST") && S.kind == PHONE_TMDB) {
        const char *body = c->buf + hl;
        form_get(body, blen, "key", r->key, sizeof r->key);
        form_get(body, blen, "api", r->api, sizeof r->api);
        form_get(body, blen, "img", r->img, sizeof r->img);
        int ok = r->key[0] != 0;
        for (const char *q = r->key; *q && ok; q++)
            ok = (*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '.' || *q == '_' || *q == '-';
        if (!ok || (r->api[0] && !valid_url(r->api)) || (r->img[0] && !valid_url(r->img))) {
            page(&b, tr(ok ? S_PH_PAGE_BAD : S_PH_TMDB_BAD), 1);
            respond(c->fd, 200, b.s);
            memset(r, 0, sizeof *r);
        } else {
            r->kind = PHONE_TMDB;
            page(&b, tr(S_PH_PAGE_DONE_TMDB), 0);
            respond(c->fd, 200, b.s);
            *got = 1;
        }
    } else if (!strcmp(method, "POST")) {
        const char *body = c->buf + hl;
        form_get(body, blen, "url", r->url, sizeof r->url);
        form_get(body, blen, "name", r->name, sizeof r->name);
        form_get(body, blen, "epg", r->epg, sizeof r->epg);
        char nm[160];
        u8_copy(nm, sizeof nm, r->name);
        snprintf(r->name, sizeof r->name, "%s", nm);
        if (!valid_url(r->url) || (r->epg[0] && !valid_url(r->epg))) {
            page(&b, tr(S_PH_PAGE_BAD), 1);
            respond(c->fd, 200, b.s);
            memset(r, 0, sizeof *r);
        } else {
            page(&b, tr(S_PH_PAGE_DONE), 0);
            respond(c->fd, 200, b.s);
            *got = 1;
        }
    } else respond(c->fd, 400, "");
    sb_free(&b);
    return 1;
}

int phone_poll(phone_result *r) {
    if (S.lfd < 0) return 0;
    double now = mono_now();
    if (now - S.t_start > LIFETIME) { phone_stop(); return 0; }
    for (;;) {
        int fd = accept4(S.lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) break;
        int slot = -1;
        for (int i = 0; i < MAX_CONN && slot < 0; i++) if (S.c[i].fd < 0) slot = i;
        if (slot < 0) { close(fd); continue; }
        S.c[slot] = (conn){fd, malloc(REQ_MAX + 1), 0, now};
        if (!S.c[slot].buf) conn_close(&S.c[slot]);
    }
    int got = 0;
    phone_result res;
    memset(&res, 0, sizeof res);
    for (int i = 0; i < MAX_CONN && !got; i++) {
        conn *c = &S.c[i];
        if (c->fd < 0) continue;
        int closed = 0;
        while (c->len < REQ_MAX) {
            ssize_t n = recv(c->fd, c->buf + c->len, REQ_MAX - c->len, 0);
            if (n > 0) { c->len += (size_t)n; continue; }
            if (n == 0) closed = 1;
            break;
        }
        c->buf[c->len] = 0;
        if (handle(c, &res, &got) || closed || now - c->t0 > CONN_TIMEOUT) conn_close(c);
    }
    if (!got) return 0;
    *r = res;
    phone_stop();
    fprintf(stderr, "phone_received kind=%d epg=%d\n", r->kind, r->epg[0] != 0);
    return 1;
}
