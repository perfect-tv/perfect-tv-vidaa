#define _GNU_SOURCE
#include "relay.h"
#include "cookies.h"
#include "http.h"
#include "util.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <sys/epoll.h>
#include "prefs.h"
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
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
/* PES одной дорожки: готов, когда набрана длина из заголовка (или пришло начало следующего) */
typedef struct { int pid; uint8_t *buf; size_t n, want; } pesbuf;
static struct {
    pthread_mutex_t mu;
    ttx *t;
    uint8_t carry[188];
    int carry_n;
    int pmt_pid[16], npmt;
    int ttx_pid, dvbsub;
    relay_dvb dvb[8];
    int ndvb;
    relay_vtt vtt[8];     /* субтитры WebVTT из главного плейлиста HLS */
    int nvtt;
    int dvb_sel, dvb_idx; /* PID и номер выбранных DVB-субтитров (-1 — нет) */
    dvbsub *dvbd;
    pesbuf pt, pd;        /* телетекст, DVB-субтитры */
    /* закрытые субтитры CEA-608/708 в видео: PID и вид видео, начало PES (до 16 КБ), языки из PMT (0x86) и HLS */
    int vid_pid, vid_codec, pv_done;
    uint8_t *pv;
    size_t pv_n;
    ccdec *cc;
    char cc_lang608[4][16], cc_lang708[8][16];
    unsigned cc_decl608;
    unsigned cc_decl708;
    int64_t base_pts;
    long long bytes;
    int gen;   /* растёт при relay_reset: соединения прежней ссылки разбор не трогают */
} D = {.mu = PTHREAD_MUTEX_INITIALIZER, .ttx_pid = -1, .dvb_sel = -1, .dvb_idx = -1, .base_pts = -1, .vid_pid = -1};
#define PV_MAX (16 * 1024)

static void init_once(void) {
    if (!D.t) D.t = ttx_new();
    if (!D.pt.buf) D.pt.buf = malloc(PES_MAX);
    if (!D.pd.buf) D.pd.buf = malloc(PES_MAX);
    if (!D.pv) D.pv = malloc(PV_MAX);
    if (!D.cc) D.cc = cc_new();
}

void relay_reset(void) {
    pthread_mutex_lock(&D.mu);
    init_once();
    ttx_reset(D.t);
    D.carry_n = D.npmt = D.dvbsub = D.ndvb = D.nvtt = 0;
    D.ttx_pid = -1;
    D.pt.n = D.pd.n = 0;
    D.pt.want = D.pd.want = 0;
    dvbsub_free(D.dvbd);   /* выбор субтитров — заново по новому потоку (tv.c выберет снова) */
    D.dvbd = NULL;
    D.dvb_sel = D.dvb_idx = -1;
    D.base_pts = -1;
    D.bytes = 0;
    D.vid_pid = -1;
    D.vid_codec = 0;
    D.pv_n = 0;
    D.pv_done = 0;
    cc_select(D.cc, 0, 0);
    cc_reset(D.cc);
    memset(D.cc_lang608, 0, sizeof D.cc_lang608);
    memset(D.cc_lang708, 0, sizeof D.cc_lang708);
    D.cc_decl608 = D.cc_decl708 = 0;
    D.gen++;
    cookies_clear();   /* cookie провайдера — свои у каждого потока */
    pthread_mutex_unlock(&D.mu);
}
static int relay_gen(void) { pthread_mutex_lock(&D.mu); int g = D.gen; pthread_mutex_unlock(&D.mu); return g; }
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

/* PES набран: данные после заголовка — декодеру дорожки */
static void pes_done(pesbuf *b) {
    if (b->n) {
        size_t hdr;
        int64_t pts = pes_pts(b->buf, b->n, &hdr);
        if (pts != -2 && hdr < b->n) {
            if (b == &D.pt) ttx_feed(D.t, b->buf + hdr, b->n - hdr, pts);
            else if (D.dvbd) dvbsub_feed(D.dvbd, b->buf + hdr, b->n - hdr, pts);
        }
    }
    b->n = b->want = 0;
}
static void pes_add(pesbuf *b, const uint8_t *pl, size_t n, int pusi) {
    if (pusi) {
        pes_done(b);
        /* PES_packet_length: 0 — до начала следующего */
        if (n >= 6 && !pl[0] && !pl[1] && pl[2] == 1) { size_t l = (size_t)(pl[4] << 8 | pl[5]); b->want = l ? l + 6 : 0; }
    } else if (!b->n) return;   /* хвост PES без начала */
    if (b->n + n <= PES_MAX) { memcpy(b->buf + b->n, pl, n); b->n += n; }
    if (b->want && b->n >= b->want) { b->n = b->want; pes_done(b); }   /* сразу, не дожидаясь следующего */
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
        int pid = (s[i + 1] & 0x1F) << 8 | s[i + 2], st = s[i];
        int vc = st == 0x1B ? CC_VIDEO_H264 : st == 0x24 ? CC_VIDEO_HEVC : st == 0x01 || st == 0x02 ? CC_VIDEO_MPEG2 : 0;
        if (vc && D.vid_pid < 0) { D.vid_pid = pid; D.vid_codec = vc; D.pv_n = 0; fprintf(stderr, "relay_video pid=%d type=0x%02x\n", pid, st); }
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
                D.pt.n = D.pt.want = 0;
                ttx_declare(D.t, lang, subs, ns);
                fprintf(stderr, "relay_teletext pid=%d lang=%s subtitle_pages=%d\n", pid, lang, ns);
            }
            /* ATSC caption_service_descriptor: язык каждого канала 608 / сервиса 708 */
            if (tag == 0x86 && dl >= 1 && pid == D.vid_pid) {
                int ns = v[0] & 0x1F;
                for (int k = 0; k < ns && 1 + 6 * k + 6 <= dl; k++) {
                    const uint8_t *e6 = v + 1 + 6 * k;
                    char lg[4] = {(char)e6[0], (char)e6[1], (char)e6[2], 0};
                    if (e6[3] & 0x80) { int sn = e6[3] & 0x3F; if (sn >= 1 && sn <= 7) { snprintf(D.cc_lang708[sn], 16, "%s", lg); D.cc_decl708 |= 1u << sn; } }
                    else { int f = e6[3] & 1; snprintf(D.cc_lang608[f * 2], 16, "%s", lg); D.cc_decl608 |= 1u << (f * 2); }   /* поле 1 — CC1, 2 — CC3 */
                }
            }
            /* DVB-субтитры: по записи на дорожку (язык, тип, страница композиции и вспомогательная); у одного PID
               их бывает несколько — обычные и для слабослышащих на разных страницах */
            for (int k = 0; tag == 0x59 && k + 8 <= dl; k += 8) {
                const uint8_t *e8 = v + k;
                int comp = e8[4] << 8 | e8[5], known = 0;
                for (int q = 0; q < D.ndvb; q++) known |= D.dvb[q].pid == pid && D.dvb[q].comp == comp;
                if (known || D.ndvb >= 8) continue;
                relay_dvb *e = &D.dvb[D.ndvb++];
                e->pid = pid;
                memcpy(e->lang, e8, 3);
                e->lang[3] = 0;
                e->type = e8[3];
                e->comp = comp;
                e->anc = e8[6] << 8 | e8[7];
                D.dvbsub = 1;
                fprintf(stderr, "relay_dvbsub pid=%d lang=%s type=0x%02x page=%d\n", pid, e->lang, e->type, comp);
            }
            d += 2 + (size_t)dl;
        }
        i = end;
    }
}

/* видео: нужно только начало PES (SEI / user_data стоят перед срезами кадра) */
static void video_done(void) {
    if (D.pv_n && !D.pv_done) {
        size_t hdr;
        int64_t pts = pes_pts(D.pv, D.pv_n, &hdr);
        if (pts != -2 && hdr < D.pv_n) cc_video(D.cc, D.vid_codec, D.pv + hdr, D.pv_n - hdr, pts);
    }
    D.pv_done = 1;
}
static void video_add(const uint8_t *pl, size_t n, int pusi) {
    if (pusi) { video_done(); D.pv_n = 0; D.pv_done = 0; }
    else if (!D.pv_n || D.pv_done) return;
    size_t k = PV_MAX - D.pv_n < n ? PV_MAX - D.pv_n : n;
    memcpy(D.pv + D.pv_n, pl, k);
    D.pv_n += k;
    if (D.pv_n == PV_MAX) video_done();
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
    if (pid == D.vid_pid) video_add(pl, n, pusi);
    if (pid == D.ttx_pid) pes_add(&D.pt, pl, n, pusi);
    else if (pid == D.dvb_sel && D.dvbd) pes_add(&D.pd, pl, n, pusi);
}

int relay_dvb_list(relay_dvb *out, int max) {
    pthread_mutex_lock(&D.mu);
    int n = D.ndvb < max ? D.ndvb : max;
    memcpy(out, D.dvb, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&D.mu);
    return n;
}
void relay_dvb_select(int i) {
    pthread_mutex_lock(&D.mu);
    if (i != D.dvb_idx) {
        dvbsub_free(D.dvbd);
        D.dvbd = NULL;
        D.pd.n = D.pd.want = 0;
        D.dvb_sel = D.dvb_idx = -1;
        if (i >= 0 && i < D.ndvb) { D.dvbd = dvbsub_new(D.dvb[i].comp, D.dvb[i].anc); D.dvb_sel = D.dvb[i].pid; D.dvb_idx = i; }
    }
    pthread_mutex_unlock(&D.mu);
}
int relay_dvb_selected(void) { return D.dvb_idx; }

int relay_cc_list(relay_cc *out, int max) {
    pthread_mutex_lock(&D.mu);
    init_once();
    int n = 0;
    unsigned p608 = cc_present608(D.cc) | D.cc_decl608;
    uint64_t p708 = cc_present708(D.cc) | D.cc_decl708;
    for (int k = 0; k < 4 && n < max; k++)
        if (p608 & (1u << k)) { out[n] = (relay_cc){CC_608, k + 1, ""}; snprintf(out[n].lang, sizeof out[n].lang, "%s", D.cc_lang608[k]); n++; }
    for (int sn = 1; sn < 64 && n < max; sn++)
        if (p708 & (1ull << sn)) { out[n] = (relay_cc){CC_708, sn, ""}; if (sn < 8) snprintf(out[n].lang, sizeof out[n].lang, "%s", D.cc_lang708[sn]); n++; }
    pthread_mutex_unlock(&D.mu);
    return n;
}
void relay_cc_select(int kind, int num) {
    pthread_mutex_lock(&D.mu);
    init_once();
    cc_select(D.cc, kind, num);
    pthread_mutex_unlock(&D.mu);
}
int relay_cc_at(int64_t pts, cc_show *out) {
    pthread_mutex_lock(&D.mu);
    int r = D.cc ? cc_at(D.cc, pts, out) : 0;
    pthread_mutex_unlock(&D.mu);
    return r;
}
dvbsub *relay_dvbsub(void) { return D.dvbd; }

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

int relay_scheme_ok(const char *url) {
    return starts_with_ci(url, "http://") || starts_with_ci(url, "https://") || starts_with_ci(url, "udp://") || starts_with_ci(url, "rtp://") ||
           starts_with_ci(url, "rtsp://");
}

int relay_url(const char *url, const char *headers, char *out, size_t cap) {
    if (!relay_scheme_ok(url)) return -1;
    if (relay_start()) return -1;
    sbuf b = {0};
    make_url(&b, url, headers);
    int ok = b.n < cap;
    if (ok) memcpy(out, b.s, b.n + 1);
    sb_free(&b);
    return ok ? 0 : -1;
}

/* адрес из плейлиста -> через ретранслятор */
static void rewrite_uri(sbuf *o, const char *base, const char *ref, const char *headers) {
    char abs[4096];
    http_resolve(base, ref, abs, sizeof abs);
    make_url(o, abs, headers);
}

/* значение атрибута тега HLS: NAME="…" или NAME=… */
static void hls_attr(const char *line, const char *name, char *out, size_t cap) {
    out[0] = 0;
    size_t nl = strlen(name);
    for (const char *p = line; (p = strstr(p, name)); p++) {
        if (p != line && p[-1] != ',' && p[-1] != ':') continue;
        if (p[nl] != '=') continue;
        const char *v = p + nl + 1, *e;
        if (*v == '"') { v++; e = strchr(v, '"'); }
        else e = strchr(v, ',');
        size_t l = e ? (size_t)(e - v) : strlen(v);
        if (l >= cap) l = cap - 1;
        memcpy(out, v, l);
        out[l] = 0;
        return;
    }
}
/* #EXT-X-MEDIA:TYPE=SUBTITLES — запомнить (адрес провайдера, не ретранслятора) */
static void note_subtitles(const char *line, const char *base, const char *headers) {
    /* вариант: CODECS (stpp — TTML, wvtt — WebVTT) относится к его группе субтитров */
    if (!strncmp(line, "#EXT-X-STREAM-INF:", 18)) {
        char g[64], codecs[300];
        hls_attr(line, "SUBTITLES", g, sizeof g);
        hls_attr(line, "CODECS", codecs, sizeof codecs);
        const char *c = strstr(codecs, "stpp") ? "stpp" : strstr(codecs, "wvtt") ? "wvtt" : NULL;
        if (!g[0] || !c) return;
        pthread_mutex_lock(&D.mu);
        for (int i = 0; i < D.nvtt; i++) if (!strcmp(D.vtt[i].group, g)) snprintf(D.vtt[i].codec, sizeof D.vtt[i].codec, "%s", c);
        pthread_mutex_unlock(&D.mu);
        return;
    }
    if (!strncmp(line, "#EXT-X-MEDIA:", 13) && strstr(line, "TYPE=CLOSED-CAPTIONS")) {   /* язык каналов 608 / сервисов 708 */
        char id[32], lg[16];
        hls_attr(line, "INSTREAM-ID", id, sizeof id);
        hls_attr(line, "LANGUAGE", lg, sizeof lg);
        pthread_mutex_lock(&D.mu);
        if (!strncmp(id, "CC", 2) && id[2] >= '1' && id[2] <= '4') { int k = id[2] - '1'; snprintf(D.cc_lang608[k], 16, "%s", lg); D.cc_decl608 |= 1u << k; }
        else if (!strncmp(id, "SERVICE", 7)) { int sn = atoi(id + 7); if (sn >= 1 && sn <= 7) { snprintf(D.cc_lang708[sn], 16, "%s", lg); D.cc_decl708 |= 1u << sn; } }
        pthread_mutex_unlock(&D.mu);
        return;
    }
    if (strncmp(line, "#EXT-X-MEDIA:", 13) || !strstr(line, "TYPE=SUBTITLES")) return;
    char uri[4096];
    hls_attr(line, "URI", uri, sizeof uri);
    if (!uri[0]) return;
    relay_vtt v;
    memset(&v, 0, sizeof v);
    http_resolve(base, uri, v.url, sizeof v.url);
    hls_attr(line, "NAME", v.name, sizeof v.name);
    hls_attr(line, "LANGUAGE", v.lang, sizeof v.lang);
    hls_attr(line, "GROUP-ID", v.group, sizeof v.group);
    snprintf(v.hdr, sizeof v.hdr, "%s", headers ? headers : "");
    pthread_mutex_lock(&D.mu);
    int known = 0;
    for (int i = 0; i < D.nvtt; i++) known |= !strcmp(D.vtt[i].url, v.url);
    if (!known && D.nvtt < 8) { D.vtt[D.nvtt++] = v; fprintf(stderr, "relay_webvtt lang=%s\n", v.lang); }
    pthread_mutex_unlock(&D.mu);
}
int relay_vtt_list(relay_vtt *out, int max) {
    pthread_mutex_lock(&D.mu);
    int n = D.nvtt < max ? D.nvtt : max;
    memcpy(out, D.vtt, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&D.mu);
    return n;
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
            note_subtitles(t, base, headers);
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

static int is_playlist(const char *s, size_t n) {
    size_t i = 0;
    if (n >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) i = 3;
    while (i < n && (s[i] == ' ' || s[i] == '\r' || s[i] == '\n')) i++;
    return n - i >= 7 && !strncmp(s + i, "#EXTM3U", 7);
}

/* ---- заголовки: ретранслятор прозрачен — Range, Cookie, повторы и т. п. делает плеер ---- */
static int hname(const char *line, const char *name) {
    size_t n = strlen(name);
    return !strncasecmp(line, name, n) && line[n] == ':';
}
/* от плеера к провайдеру: всё, кроме заголовков соединения и Cookie (их ставит ретранслятор); заголовки канала (h=)
 * важнее */
static int pass_request(const char *line) {
    static const char *const drop[] = {"Host", "Connection", "Keep-Alive", "Accept-Encoding", "TE", "Upgrade", "Proxy-Connection",
                                       "Proxy-Authorization", "Transfer-Encoding", "Content-Length", "Cookie"};
    for (size_t i = 0; i < sizeof drop / sizeof drop[0]; i++) if (hname(line, drop[i])) return 0;
    return strchr(line, ':') != NULL;
}
/* от провайдера к плееру: тело и длину ретранслятор отдаёт сам */
static int pass_response(const char *line) {
    static const char *const drop[] = {"Connection", "Keep-Alive", "Transfer-Encoding", "Content-Length", "Proxy-Connection",
                                       "Upgrade", "Alt-Svc", "Strict-Transport-Security"};
    for (size_t i = 0; i < sizeof drop / sizeof drop[0]; i++) if (hname(line, drop[i])) return 0;
    return 1;
}
static void each_line(const char *block, void (*fn)(sbuf *, const char *, void *), sbuf *o, void *ctx) {
    for (const char *p = block; p && *p;) {
        const char *e = strstr(p, "\r\n");
        size_t l = e ? (size_t)(e - p) : strlen(p);
        char *line = xstrndup(p, l);
        if (line && *line) fn(o, line, ctx);
        free(line);
        p = e ? e + 2 : NULL;
    }
}
/* у канала (h=) уже есть заголовок с таким именем */
static int chan_has(const char *chan, const char *line) {
    size_t n = (size_t)(strchr(line, ':') - line);
    for (const char *p = chan; p && *p;) {
        if (!strncasecmp(p, line, n) && p[n] == ':') return 1;
        p = strchr(p, '\n');
        if (p) p++;
    }
    return 0;
}
static void req_fn(sbuf *o, const char *line, void *ctx) {
    if (!pass_request(line) || chan_has(ctx, line)) return;
    sb_add(o, line);
    sb_add(o, "\r\n");
}
static void resp_fn(sbuf *o, const char *line, void *ctx) {
    (void)ctx;
    if (!pass_response(line) || hname(line, "Set-Cookie")) return;   /* cookie хранит ретранслятор (core/cookies.c) */
    sb_add(o, line);
    sb_add(o, "\r\n");
}
static const char *reason(int code) {
    switch (code) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 304: return "Not Modified";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    default: return code >= 500 ? "Bad Gateway" : "Error";
    }
}

/* ================= цикл событий =================
 * Один поток: epoll по всем сокетам, неблокирующие сокеты и TLS (SSL_ERROR_WANT_READ/WRITE). Соединение плеера (cl)
 * получает запрос, к нему цепляется соединение с провайдером (up): адрес → connect → TLS → запрос → заголовки →
 * тело. Плейлист HLS копится и переписывается, остальное — плееру как есть (известна длина — Content-Length,
 * нет — кусками) и попутно в разбор TS. Плеер не успевает читать (в очереди > OUT_HIGH) — чтение у провайдера
 * приостанавливается. Дочитанное соединение с провайдером — в пул (keep-alive). DNS (getaddrinfo блокирующий) —
 * короткий поток на запрос, итог — через трубу в цикл; адреса кэшируются на минуту. Объекты, закрытые в обработке
 * пачки событий, освобождаются после неё. */
#define MAX_CLIENTS 64
#define OUT_HIGH (256 * 1024)
#define POOL_MAX 8
#define T_CONNECT 20.0
#define T_BODY 20.0
#define T_IDLE 15.0
#define T_POOL 20.0
#define DNS_TTL 60.0
#define NADDR 4

enum { K_LISTEN = 1, K_WAKE, K_CLIENT, K_UP };
typedef struct { int kind; void *p; } tag;
typedef struct cl cl;
typedef struct up up;

enum { U_RESOLVE, U_CONNECT, U_TLS, U_SEND, U_HEAD, U_BODY, U_IDLE, U_RTSP, U_UDP };
enum { P_HTTP, P_UDP, P_RTSP };
typedef struct rtsp_st rtsp_st;
typedef struct dns_job dns_job;
struct up {
    tag t;
    int fd, st, ev, dead, paused, reused, again;
    SSL *ssl;
    http_url u;
    char key[300];
    cl *c;
    dns_job *dns;
    struct sockaddr_storage addr[NADDR];
    socklen_t alen[NADDR];
    int naddr, ai;
    sbuf req;
    size_t req_off;
    char *in;
    size_t in_n, in_cap;
    int status, chunked, server_close, phase;   /* phase: 0 — размер куска, 1 — данные, 2 — CRLF, 3 — трейлеры */
    long long length, received, chunk_left;
    sbuf headers;
    char location[4096];
    double deadline;
    int proto, got_data;
    rtsp_st *rt;          /* RTSP: сеанс */
    up *next;
};
struct cl {
    tag t;
    int fd, ev, dead, busy, finishing, keep;
    char in[16384];
    size_t in_n;
    up *u;
    char url[4096], chan[4096];
    sbuf fwd;
    int client_close, gen, redirects, no_pool;
    int head_sent, decided, playlist, chunked_out, content_mpegurl;
    sbuf head, body, out;
    size_t out_off;
    double deadline;
    cl *next;
};
struct dns_job { char host[256], port[8]; up *u; int cancelled, n; struct sockaddr_storage a[NADDR]; socklen_t l[NADDR]; };
typedef struct { char host[256], port[8]; struct sockaddr_storage a[NADDR]; socklen_t l[NADDR]; int n; double t; } dns_entry;

static int ep = -1, wake_r = -1, wake_w = -1;
static tag t_listen = {K_LISTEN, NULL}, t_wake = {K_WAKE, NULL};
static cl *clients;
static up *ups;
static void *dead[256];
static int ndead;
static dns_entry dns_cache[16];
static int nclients;

static void set_ev(int fd, tag *t, int *cur, int want) {
    if (fd < 0 || *cur == want) return;
    struct epoll_event e = {.events = (uint32_t)want, .data.ptr = t};
    epoll_ctl(ep, *cur < 0 ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, fd, &e);
    *cur = want;
}
static void bury(void *p) { if (ndead < (int)(sizeof dead / sizeof dead[0])) dead[ndead++] = p; else free(p); }

/* ---------- провайдер ---------- */
static void client_upstream_done(cl *c, int ok);
static void client_upstream_failed(cl *c, int code);
static void up_start_request(up *u);
static void up_readable(up *u);

static void rtsp_teardown(up *u);
static void rtsp_connected(up *u);
static void rtsp_event(up *u, uint32_t ev);
static void udp_event(up *u);
static int udp_attach(cl *c);
static void rtsp_attach(cl *c);
static void rtsp_keepalive(up *u, double now);

static void up_free(up *u) {
    if (u->dead) return;
    u->dead = 1;
    if (u->dns) u->dns->cancelled = 1;   /* поток DNS ещё работает: итог выбросит цикл */
    if (u->rt) rtsp_teardown(u);
    if (u->fd >= 0) { epoll_ctl(ep, EPOLL_CTL_DEL, u->fd, NULL); }
    if (u->ssl) SSL_free(u->ssl);
    if (u->fd >= 0) close(u->fd);
    u->fd = -1;
    u->ssl = NULL;
    for (up **pp = &ups; *pp; pp = &(*pp)->next) if (*pp == u) { *pp = u->next; break; }
    sb_free(&u->req);
    sb_free(&u->headers);
    free(u->in);
    free(u->rt);
    bury(u);
}

static void client_attach(cl *c);
static void up_fail(up *u, int code) {
    cl *c = u->c;
    /* соединение из пула закрыто сервером в тот же миг — запрос ещё раз на новом */
    int retry = c && u->reused && (u->st == U_SEND || u->st == U_HEAD) && !u->in_n && !c->head_sent && !c->no_pool;
    u->c = NULL;
    up_free(u);
    if (!c) return;
    c->u = NULL;
    if (retry) { c->no_pool = 1; client_attach(c); return; }
    client_upstream_failed(c, code);
}

static int pool_count(void) { int n = 0; for (up *u = ups; u; u = u->next) n += u->st == U_IDLE; return n; }

static void up_to_pool(up *u) {
    u->c = NULL;
    u->st = U_IDLE;
    u->in_n = 0;
    u->again = 0;   /* в пуле нечего дочитывать: иначе epoll_wait(..., 0) крутится до T_POOL */
    u->deadline = mono_now() + T_POOL;
    if (pool_count() > POOL_MAX) {   /* самое старое — закрыть */
        up *old = NULL;
        for (up *x = ups; x; x = x->next) if (x->st == U_IDLE && x != u && (!old || x->deadline < old->deadline)) old = x;
        if (old) up_free(old);
    }
    set_ev(u->fd, &u->t, &u->ev, EPOLLIN | EPOLLRDHUP);   /* сервер закрыл — узнаем сразу */
}

static void dns_store(const char *host, const char *port_, const struct sockaddr_storage *a, const socklen_t *l, int n) {
    int slot = 0;
    for (int i = 0; i < 16; i++) {
        if (!strcmp(dns_cache[i].host, host) && !strcmp(dns_cache[i].port, port_)) { slot = i; break; }
        if (dns_cache[i].t < dns_cache[slot].t) slot = i;
    }
    dns_entry *e = &dns_cache[slot];
    snprintf(e->host, sizeof e->host, "%s", host);
    snprintf(e->port, sizeof e->port, "%s", port_);
    e->n = n;
    memcpy(e->a, a, sizeof e->a);
    memcpy(e->l, l, sizeof e->l);
    e->t = mono_now();
}

static void *dns_thread(void *arg) {
    dns_job *j = arg;
    pthread_setname_np(pthread_self(), "ptv-dns");
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (!getaddrinfo(j->host, j->port, &hints, &res)) {
        for (struct addrinfo *a = res; a && j->n < NADDR; a = a->ai_next) {
            memcpy(&j->a[j->n], a->ai_addr, a->ai_addrlen);
            j->l[j->n++] = a->ai_addrlen;
        }
        freeaddrinfo(res);
    }
    if (write(wake_w, &j, sizeof j) != sizeof j) free(j);
    return NULL;
}

static void up_connect_next(up *u) {
    while (u->ai < u->naddr) {
        int i = u->ai++;
        int fd = socket(u->addr[i].ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (connect(fd, (struct sockaddr *)&u->addr[i], u->alen[i]) && errno != EINPROGRESS) { close(fd); continue; }
        u->fd = fd;
        u->ev = -1;
        u->st = U_CONNECT;
        u->deadline = mono_now() + T_CONNECT;
        set_ev(fd, &u->t, &u->ev, EPOLLOUT);
        return;
    }
    fprintf(stderr, "relay_open_failed network\n");
    up_fail(u, 502);
}

static void up_resolved(up *u) {
    u->ai = 0;
    if (!u->naddr) { fprintf(stderr, "relay_open_failed dns\n"); up_fail(u, 502); return; }
    up_connect_next(u);
}

static void up_resolve(up *u) {
    double now = mono_now();
    for (int i = 0; i < 16; i++) {
        dns_entry *e = &dns_cache[i];
        if (e->n && now - e->t < DNS_TTL && !strcmp(e->host, u->u.host) && !strcmp(e->port, u->u.port)) {
            memcpy(u->addr, e->a, sizeof u->addr);
            memcpy(u->alen, e->l, sizeof u->alen);
            u->naddr = e->n;
            up_resolved(u);
            return;
        }
    }
    dns_job *j = calloc(1, sizeof *j);
    if (!j) { up_fail(u, 502); return; }
    snprintf(j->host, sizeof j->host, "%s", u->u.host);
    snprintf(j->port, sizeof j->port, "%s", u->u.port);
    j->u = u;
    u->dns = j;
    u->st = U_RESOLVE;
    u->deadline = now + T_CONNECT;
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 128 * 1024);
    if (pthread_create(&t, &a, dns_thread, j)) { u->dns = NULL; free(j); up_fail(u, 502); }
    pthread_attr_destroy(&a);
}

static void dns_done(void) {
    dns_job *j;
    while (read(wake_r, &j, sizeof j) == sizeof j) {
        if (j->cancelled) { free(j); continue; }
        up *u = j->u;
        u->dns = NULL;
        memcpy(u->addr, j->a, sizeof u->addr);
        memcpy(u->alen, j->l, sizeof u->alen);
        u->naddr = j->n;
        if (j->n) dns_store(j->host, j->port, j->a, j->l, j->n);
        free(j);
        up_resolved(u);
    }
}

/* TLS: шаг рукопожатия; 1 — готово, 0 — ждём, -1 — ошибка */
static int up_tls_step(up *u) {
    int r = SSL_connect(u->ssl);
    if (r == 1) return 1;
    int e = SSL_get_error(u->ssl, r);
    if (e == SSL_ERROR_WANT_READ) { set_ev(u->fd, &u->t, &u->ev, EPOLLIN); return 0; }
    if (e == SSL_ERROR_WANT_WRITE) { set_ev(u->fd, &u->t, &u->ev, EPOLLOUT); return 0; }
    long vr = SSL_get_verify_result(u->ssl);
    const char *why = vr != X509_V_OK ? X509_verify_cert_error_string(vr) : ERR_reason_error_string(ERR_peek_last_error());
    fprintf(stderr, "relay_tls_failed %s\n", why ? why : "handshake");
    ERR_clear_error();
    return -1;
}

static void up_send(up *u) {
    while (u->req_off < u->req.n) {
        size_t n = u->req.n - u->req_off;
        ssize_t w;
        if (u->ssl) {
            int r = SSL_write(u->ssl, u->req.s + u->req_off, (int)n);
            if (r <= 0) {
                int e = SSL_get_error(u->ssl, r);
                if (e == SSL_ERROR_WANT_WRITE) { set_ev(u->fd, &u->t, &u->ev, EPOLLOUT); return; }
                if (e == SSL_ERROR_WANT_READ) { set_ev(u->fd, &u->t, &u->ev, EPOLLIN); return; }
                ERR_clear_error();
                up_fail(u, 502);
                return;
            }
            w = r;
        } else {
            w = send(u->fd, u->req.s + u->req_off, n, MSG_NOSIGNAL);
            if (w < 0) {
                if (errno == EAGAIN || errno == EINTR) { set_ev(u->fd, &u->t, &u->ev, EPOLLOUT | (u->proto == P_RTSP ? EPOLLIN | EPOLLRDHUP : 0)); return; }
                up_fail(u, 502);
                return;
            }
        }
        u->req_off += (size_t)w;
    }
    if (u->proto == P_RTSP) {   /* RTSP: запрос ушёл — ждём ответ и данные дальше */
        sb_clear(&u->req);
        u->req_off = 0;
        set_ev(u->fd, &u->t, &u->ev, EPOLLIN | EPOLLRDHUP);
        return;
    }
    u->st = U_HEAD;
    u->in_n = 0;
    u->deadline = mono_now() + T_CONNECT;
    set_ev(u->fd, &u->t, &u->ev, EPOLLIN | EPOLLRDHUP);
}

static void up_after_connect(up *u) {
    if (u->proto == P_RTSP) { rtsp_connected(u); return; }
    if (!strcmp(u->u.scheme, "https")) {
        int any = P.any_cert;
        SSL_CTX *ctx = http_ssl_ctx(any);
        u->ssl = ctx ? SSL_new(ctx) : NULL;
        if (!u->ssl) { up_fail(u, 502); return; }
        SSL_set_fd(u->ssl, u->fd);
        SSL_set_tlsext_host_name(u->ssl, u->u.host);
        if (!any) X509_VERIFY_PARAM_set1_host(SSL_get0_param(u->ssl), u->u.host, 0);
        u->st = U_TLS;
        int r = up_tls_step(u);
        if (r < 0) { up_fail(u, 502); return; }
        if (!r) return;
    }
    u->st = U_SEND;
    up_send(u);
}

/* запрос к провайдеру (заголовки — как у блокирующего клиента; соединение — keep-alive) */
static void build_request(up *u, cl *c) {
    sb_clear(&u->req);
    u->req_off = 0;
    sb_printf(&u->req, "GET %s HTTP/1.1\r\nHost: %s", u->u.path, u->u.host);
    if ((!strcmp(u->u.scheme, "https") && strcmp(u->u.port, "443")) || (!strcmp(u->u.scheme, "http") && strcmp(u->u.port, "80")))
        sb_printf(&u->req, ":%s", u->u.port);
    sb_add(&u->req, "\r\n");
    const char *f = c->fwd.s ? c->fwd.s : "";
    if (!strcasestr(f, "user-agent:")) sb_add(&u->req, "User-Agent: PerfectTV/" PTV_VERSION " (VIDAA; native)\r\n");
    if (strncasecmp(f, "accept:", 7) && !strcasestr(f, "\naccept:")) sb_add(&u->req, "Accept: */*\r\n");
    sb_add(&u->req, "Accept-Encoding: identity\r\nConnection: keep-alive\r\n");
    if (u->u.auth[0]) sb_printf(&u->req, "Authorization: Basic %s\r\n", u->u.auth);
    cookies_merge(&u->req, f, c->url);   /* Cookie канала и хранилища — одной строкой */
    sb_add(&u->req, "\r\n");
}

static void up_start_request(up *u) {
    cl *c = u->c;
    build_request(u, c);
    u->status = u->chunked = u->server_close = u->phase = 0;
    u->length = -1;
    u->received = u->chunk_left = 0;
    u->in_n = 0;
    u->again = 0;
    u->location[0] = 0;
    sb_clear(&u->headers);
    u->paused = 0;
    if (u->st == U_IDLE) { u->st = U_SEND; u->reused = 1; u->deadline = mono_now() + T_CONNECT; up_send(u); }   /* из пула */
    else up_resolve(u);
}

/* Соединение к провайдеру для запроса клиента: из пула (тот же адрес и режим сертификатов) или новое. */
static void client_attach(cl *c) {
    if (starts_with_ci(c->url, "udp://") || starts_with_ci(c->url, "rtp://")) { if (udp_attach(c)) client_upstream_failed(c, 502); return; }
    if (starts_with_ci(c->url, "rtsp://")) { rtsp_attach(c); return; }
    http_url hu;
    if (http_split_url(c->url, &hu)) { client_upstream_failed(c, 404); return; }
    char key[300];
    snprintf(key, sizeof key, "%s://%s:%s|%d", hu.scheme, hu.host, hu.port, P.any_cert);
    up *u = NULL;
    for (up *x = ups; x && !c->no_pool; x = x->next) if (x->st == U_IDLE && !x->dead && !strcmp(x->key, key)) { u = x; break; }
    if (!u) {
        u = calloc(1, sizeof *u);
        if (!u) { client_upstream_failed(c, 502); return; }
        u->t = (tag){K_UP, u};
        u->fd = -1;
        u->ev = -1;
        u->st = -1;
        snprintf(u->key, sizeof u->key, "%s", key);
        u->next = ups;
        ups = u;
    }
    u->u = hu;
    u->c = c;
    c->u = u;
    up_start_request(u);
}

/* разбор заголовков ответа; 1 — готовы, 0 — ждём ещё, -1 — ошибка */
static int up_parse_head(up *u) {
    char *e = memmem(u->in, u->in_n, "\r\n\r\n", 4);
    if (!e) return u->in_n > 64 * 1024 ? -1 : 0;
    *e = 0;
    char *line = u->in, *nl = strstr(line, "\r\n");
    if (strncmp(line, "HTTP/", 5) || !strchr(line, ' ')) return -1;
    u->status = atoi(strchr(line, ' ') + 1);
    u->server_close = !strncmp(line, "HTTP/1.0", 8);
    while (nl) {
        line = nl + 2;
        nl = strstr(line, "\r\n");
        if (nl) *nl = 0;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        const char *v = colon + 1;
        while (*v == ' ') v++;
        size_t kl = (size_t)(colon - line);
        if (kl == 14 && !strncasecmp(line, "Content-Length", 14)) u->length = atoll(v);
        else if (kl == 17 && !strncasecmp(line, "Transfer-Encoding", 17)) u->chunked = strcasestr(v, "chunked") != NULL;
        else if (kl == 10 && !strncasecmp(line, "Connection", 10)) u->server_close = strcasestr(v, "close") != NULL;
        else if (kl == 8 && !strncasecmp(line, "Location", 8)) snprintf(u->location, sizeof u->location, "%s", v);
        sb_add(&u->headers, line);
        sb_add(&u->headers, "\r\n");
    }
    if (u->chunked) u->length = -1;
    if (u->c && u->c->gen == relay_gen()) cookies_from_headers(u->c->url, u->headers.s);   /* и у редиректа */
    size_t used = (size_t)(e - u->in) + 4;
    memmove(u->in, u->in + used, u->in_n - used);
    u->in_n -= used;
    return 1;
}

/* ---------- плеер ---------- */
static void client_free(cl *c) {
    if (c->dead) return;
    c->dead = 1;
    if (c->u) { c->u->c = NULL; up_free(c->u); c->u = NULL; }   /* недочитанное соединение в пул не идёт */
    epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    for (cl **pp = &clients; *pp; pp = &(*pp)->next) if (*pp == c) { *pp = c->next; break; }
    sb_free(&c->fwd);
    sb_free(&c->head);
    sb_free(&c->body);
    sb_free(&c->out);
    nclients--;
    bury(c);
}

static size_t out_pending(cl *c) { return c->out.n - c->out_off; }

static void client_flush(cl *c);

static void out_add(cl *c, const char *d, size_t n) {
    if (c->out_off && c->out_off == c->out.n) { sb_clear(&c->out); c->out_off = 0; }
    else if (c->out_off > (1u << 20)) {   /* отправленное — выкинуть */
        memmove(c->out.s, c->out.s + c->out_off, c->out.n - c->out_off);
        c->out.n -= c->out_off;
        c->out.s[c->out.n] = 0;
        c->out_off = 0;
    }
    sb_addn(&c->out, d, n);
}

static void client_reply_simple(cl *c, int code) {
    char b[200];
    snprintf(b, sizeof b, "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", code, reason(code));
    out_add(c, b, strlen(b));
    c->busy = 0;
    c->head_sent = 1;   /* ответ начат: следующие ошибки только обрывают соединение */
    c->finishing = 1;
    c->keep = 0;
    client_flush(c);
}

static void client_upstream_failed(cl *c, int code) {
    if (c->head_sent) { client_free(c); return; }   /* ответ уже начат — только оборвать */
    client_reply_simple(c, code);
}

/* заголовки ответа плееру; len < 0 — длина неизвестна (куски) */
static void client_send_head(cl *c, up *u, long long len) {
    sb_clear(&c->head);
    sb_printf(&c->head, "HTTP/1.1 %d %s\r\n", u->status, reason(u->status));
    each_line(u->headers.s ? u->headers.s : "", resp_fn, &c->head, NULL);
    c->chunked_out = len < 0;
    if (c->chunked_out) sb_add(&c->head, "Transfer-Encoding: chunked\r\n");
    else sb_printf(&c->head, "Content-Length: %lld\r\n", len);
    sb_add(&c->head, c->client_close ? "Connection: close\r\n\r\n" : "Connection: keep-alive\r\n\r\n");
    out_add(c, c->head.s, c->head.n);
    c->head_sent = 1;
}

/* кусок тела от провайдера */
static void client_deliver(cl *c, up *u, const char *d, size_t n) {
    if (c->dead || c->finishing) return;
    if (!c->decided) {
        int enc = u->headers.s && strcasestr(u->headers.s, "Content-Encoding:") && !strcasestr(u->headers.s, "Content-Encoding: identity");
        c->playlist = !enc && (c->content_mpegurl || is_playlist(d, n));
        c->decided = 1;
        if (!c->playlist) client_send_head(c, u, u->length);
    }
    if (c->playlist) {
        if (c->body.n + n > PLAYLIST_MAX) { client_upstream_failed(c, 502); return; }
        sb_addn(&c->body, d, n);
        return;
    }
    pthread_mutex_lock(&D.mu);
    if (D.gen == c->gen) tap_locked((const uint8_t *)d, n);   /* прежняя ссылка — не разбирать */
    pthread_mutex_unlock(&D.mu);
    if (c->chunked_out) {
        char h[24];
        snprintf(h, sizeof h, "%zx\r\n", n);
        out_add(c, h, strlen(h));
        out_add(c, d, n);
        out_add(c, "\r\n", 2);
    } else out_add(c, d, n);
}

/* ответ провайдера кончился: плейлист — переписать и отдать, иначе — завершить куски */
static void client_upstream_done(cl *c, int clean) {
    up *u = c->u;
    if (!c->decided) {   /* тела не было */
        c->decided = 1;
        client_send_head(c, u, 0);
    } else if (c->playlist) {
        sbuf out = {0};
        rewrite_playlist(&out, c->body.s ? c->body.s : "", c->body.n, c->url, c->chan);
        client_send_head(c, u, (long long)out.n);
        out_add(c, out.s ? out.s : "", out.n);
        sb_free(&out);
    } else if (c->chunked_out) out_add(c, "0\r\n\r\n", 5);
    c->u = NULL;
    if (clean && !u->server_close && !u->in_n) up_to_pool(u);
    else up_free(u);
    c->busy = 0;
    c->finishing = 1;
    c->keep = !c->client_close;
    client_flush(c);
}

/* тело из буфера провайдера: длина, куски или до закрытия */
static void up_body(up *u, int eof) {
    cl *c = u->c;
    size_t pos = 0;
    while (c && !c->dead && pos < u->in_n) {
        if (!u->chunked) {
            size_t take = u->in_n - pos;
            if (u->length >= 0 && (long long)take > u->length - u->received) take = (size_t)(u->length - u->received);
            client_deliver(c, u, u->in + pos, take);
            if (!c || c->dead) return;   /* ответ уже завершён/плеер закрыт — u->in мог быть освобождён */
            pos += take;
            u->received += (long long)take;
            if (u->length >= 0 && u->received >= u->length) break;
            continue;
        }
        if (u->phase == 0 || u->phase == 3) {
            char *e = memmem(u->in + pos, u->in_n - pos, "\r\n", 2);
            if (!e) break;
            size_t ll = (size_t)(e - (u->in + pos));
            if (u->phase == 0) {
                u->chunk_left = strtoll(u->in + pos, NULL, 16);
                u->phase = u->chunk_left > 0 ? 1 : 3;
            } else if (!ll) { pos += 2; u->phase = 4; break; }   /* пустая строка после трейлеров — конец */
            pos += ll + 2;
        } else if (u->phase == 1) {
            size_t take = u->in_n - pos;
            if ((long long)take > u->chunk_left) take = (size_t)u->chunk_left;
            client_deliver(c, u, u->in + pos, take);
            if (!c || c->dead) return;   /* ответ уже завершён/плеер закрыт — u->in мог быть освобождён */
            pos += take;
            u->chunk_left -= (long long)take;
            if (!u->chunk_left) u->phase = 2;
        } else if (u->phase == 2) {
            if (u->in_n - pos < 2) break;
            pos += 2;
            u->phase = 0;
        }
    }
    if (!c || c->dead) return;
    memmove(u->in, u->in + pos, u->in_n - pos);
    u->in_n -= pos;
    int done = u->chunked ? u->phase == 4 : u->length >= 0 && u->received >= u->length;
    if (done) { client_upstream_done(c, 1); return; }
    if (eof) {
        if (!u->chunked && u->length < 0) { u->server_close = 1; client_upstream_done(c, 0); }   /* до закрытия — норма */
        else up_fail(u, 502);
        return;
    }
    /* плеер не успевает — провайдера пока не читать */
    if (!c->playlist && out_pending(c) > OUT_HIGH && !u->paused) { u->paused = 1; set_ev(u->fd, &u->t, &u->ev, EPOLLRDHUP); }
    client_flush(c);
}

/* читать у провайдера, пока есть; потом — заголовки или тело */
static void up_readable(up *u) {
    int eof = 0;
    for (;;) {
        if (u->in_cap - u->in_n < 16384) {
            size_t cap = u->in_cap ? u->in_cap * 2 : 65536;
            if (cap > 1 << 20 && u->in_n + 16384 > 1 << 20) break;   /* хватит в этот раз */
            char *n = realloc(u->in, cap);
            if (!n) break;
            u->in = n;
            u->in_cap = cap;
        }
        ssize_t r;
        size_t room = u->in_cap - u->in_n;
        if (room > 65536) room = 65536;
        if (u->ssl) {
            int x = SSL_read(u->ssl, u->in + u->in_n, (int)room);
            if (x <= 0) {
                int e = SSL_get_error(u->ssl, x);
                if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) break;
                if (e == SSL_ERROR_ZERO_RETURN) { eof = 1; break; }
                ERR_clear_error();
                eof = 1;
                break;
            }
            r = x;
        } else {
            r = recv(u->fd, u->in + u->in_n, room, 0);
            if (r < 0) { if (errno == EAGAIN || errno == EINTR) break; eof = 1; break; }
            if (r == 0) { eof = 1; break; }
        }
        u->in_n += (size_t)r;
        if (u->st == U_BODY && u->in_n >= 65536) break;   /* отдать плееру, потом читать дальше */
    }
    if (u->st == U_IDLE) { up_free(u); return; }   /* в пуле: сервер закрыл или прислал лишнее */
    /* TLS держит расшифрованное, о чём epoll не знает: дочитать на следующем проходе цикла */
    u->again = !eof && u->ssl && SSL_pending(u->ssl) > 0;
    u->deadline = mono_now() + T_BODY;
    if (u->st == U_HEAD) {
        int h = up_parse_head(u);
        if (h < 0 || (!h && eof)) { up_fail(u, 502); return; }
        u->reused = 0;   /* ответ пошёл — повтора уже не будет */
        if (!h) return;
        cl *c = u->c;
        /* редирект — тем же клиентом по новому адресу */
        if (u->status >= 300 && u->status < 400 && u->status != 304 && u->location[0] && c && c->redirects < 8) {
            char next[4096];
            http_resolve(c->url, u->location, next, sizeof next);
            snprintf(c->url, sizeof c->url, "%s", next);
            c->redirects++;
            c->u = NULL;
            u->c = NULL;
            up_free(u);
            client_attach(c);
            return;
        }
        if (u->status >= 400 || (u->status != 200 && u->status != 206 && u->status != 304)) {
            fprintf(stderr, "relay_open_failed http_%d\n", u->status);
            up_fail(u, u->status >= 300 && u->status < 600 ? u->status : 502);
            return;
        }
        if (c) {
            const char *ct = u->headers.s ? strcasestr(u->headers.s, "Content-Type:") : NULL;
            if (ct) {   /* только в строке Content-Type */
                const char *e2 = strstr(ct, "\r\n");
                char line[200];
                snprintf(line, sizeof line, "%.*s", (int)(e2 ? (size_t)(e2 - ct) : strlen(ct)), ct);
                c->content_mpegurl = strcasestr(line, "mpegurl") != NULL;
            }
        }
        u->st = U_BODY;
        if (u->status == 304 || u->length == 0) { u->length = 0; if (c) client_upstream_done(c, !eof); return; }
    }
    if (u->st == U_BODY) up_body(u, eof);
}

/* ---------- UDP / RTP и RTSP ----------
 * Потоки не по HTTP плееру отдаются как HTTP с MPEG-TS без длины (кусками) — тем же путём, что и HTTP, с разбором
 * TS по дороге (телетекст, DVB, CEA-608). UDP / RTP: мультикаст — вход в группу (с источником — SSM), иначе — приём
 * на порт; заголовок RTP снимается. Плеер читать не успевает — датаграммы сверх очереди выбрасываются (UDP не
 * приостановить). RTSP: DESCRIBE → SETUP (RTP поверх того же TCP, interleaved) → PLAY, авторизация Basic / Digest;
 * берётся медиа с MPEG-TS (тип 33 или rtpmap MP2T) — других (отдельные H.264 / AAC) нет: ответ 415, и tv.c открывает
 * канал напрямую. Сеанс поддерживается GET_PARAMETER (не умеет — OPTIONS) раз в половину таймаута, при закрытии —
 * TEARDOWN. */
#define UDP_OUT_MAX (4u << 20)
#define T_UDP_FIRST 10.0

static const uint8_t *rtp_strip(const uint8_t *p, size_t n, size_t *out) {
    if (n && p[0] == 0x47) { *out = n; return p; }   /* голый TS */
    if (n < 12 || (p[0] >> 6) != 2) return NULL;
    size_t h = 12 + 4 * (size_t)(p[0] & 15);
    if ((p[0] & 0x10) && n >= h + 4) h += 4 + 4 * (size_t)(p[h + 2] << 8 | p[h + 3]);
    size_t pad = (p[0] & 0x20) ? p[n - 1] : 0;
    if (h + pad >= n) return NULL;
    *out = n - h - pad;
    return p + h;
}

static void stream_head(up *u) {
    u->status = 200;
    u->length = -1;
    sb_clear(&u->headers);
    sb_add(&u->headers, "Content-Type: video/mp2t\r\n");
}

/* кусок TS от UDP / RTSP — плееру (очередь переполнена — выбросить) */
static void stream_data(up *u, const uint8_t *d, size_t n) {
    cl *c = u->c;
    if (!c || c->dead || !n) return;
    if (!u->got_data) { u->got_data = 1; fprintf(stderr, "relay_stream_started proto=%d\n", u->proto); }
    u->deadline = mono_now() + T_BODY;
    if (out_pending(c) > UDP_OUT_MAX) return;
    client_deliver(c, u, (const char *)d, n);
}

/* udp://[источник@]группа:порт[?…], rtp://… ; «@группа» — то же без источника */
static int udp_parse(const char *url, struct in_addr *grp, struct in_addr *src, int *has_src, int *port_) {
    const char *s = strstr(url, "://") + 3;
    char a[300];
    size_t l = strcspn(s, "?/#");
    if (l >= sizeof a) return -1;
    memcpy(a, s, l);
    a[l] = 0;
    char *at = strchr(a, '@'), *hp = a;
    *has_src = 0;
    if (at) {
        *at = 0;
        if (*a) { if (inet_pton(AF_INET, a, src) != 1) return -1; *has_src = 1; }
        hp = at + 1;
    }
    char *colon = strrchr(hp, ':');
    if (!colon) return -1;
    *colon = 0;
    *port_ = atoi(colon + 1);
    if (*port_ <= 0 || *port_ > 65535) return -1;
    if (!*hp) { grp->s_addr = htonl(INADDR_ANY); return 0; }
    return inet_pton(AF_INET, hp, grp) == 1 ? 0 : -1;
}

static int udp_attach(cl *c) {
    struct in_addr grp, src;
    int has_src, prt;
    if (udp_parse(c->url, &grp, &src, &has_src, &prt)) { fprintf(stderr, "relay_udp_bad_url\n"); return -1; }
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1, rb = 4 << 20;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);
    int mc = IN_MULTICAST(ntohl(grp.s_addr));
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)prt)};
    sa.sin_addr = mc ? grp : (struct in_addr){htonl(INADDR_ANY)};   /* группа в bind — чужие группы на том же порту не придут */
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa)) {
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(fd, (struct sockaddr *)&sa, sizeof sa)) { fprintf(stderr, "relay_udp_bind %s\n", strerror(errno)); close(fd); return -1; }
    }
    if (mc) {
        int r;
        if (has_src) {
            struct ip_mreq_source m = {.imr_multiaddr = grp, .imr_sourceaddr = src, .imr_interface = {htonl(INADDR_ANY)}};
            r = setsockopt(fd, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, &m, sizeof m);
        } else {
            struct ip_mreq m = {.imr_multiaddr = grp, .imr_interface = {htonl(INADDR_ANY)}};
            r = setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m);
        }
        if (r) { fprintf(stderr, "relay_udp_join %s\n", strerror(errno)); close(fd); return -1; }
    }
    up *u = calloc(1, sizeof *u);
    if (!u) { close(fd); return -1; }
    u->t = (tag){K_UP, u};
    u->fd = fd;
    u->ev = -1;
    u->st = U_UDP;
    u->proto = P_UDP;
    snprintf(u->key, sizeof u->key, "udp");
    u->next = ups;
    ups = u;
    u->c = c;
    c->u = u;
    stream_head(u);
    u->deadline = mono_now() + T_UDP_FIRST;
    set_ev(fd, &u->t, &u->ev, EPOLLIN);
    fprintf(stderr, "relay_udp_open multicast=%d ssm=%d\n", mc, has_src);
    return 0;
}

static void udp_event(up *u) {
    static uint8_t buf[65536];
    for (int k = 0; k < 512 && !u->dead; k++) {
        ssize_t r = recv(u->fd, buf, sizeof buf, 0);
        if (r < 0) { if (errno != EAGAIN && errno != EINTR) up_fail(u, 502); break; }
        size_t pn;
        const uint8_t *pl = rtp_strip(buf, (size_t)r, &pn);
        if (pl) stream_data(u, pl, pn);
    }
    if (!u->dead && u->c && !u->c->dead) client_flush(u->c);
}

/* ---- RTSP ---- */
enum { R_DESCRIBE, R_SETUP, R_PLAY, R_PLAYING };
struct rtsp_st {
    int phase, cseq, auth_tries, rtp_ch, ka_period, ka_options, digest;
    double ka_at;
    char user[128], pass[128], realm[200], nonce[300], opaque[200], qop[32];
    char url[4096], base[4096], media[4096], aggr[4096], session[200], ua[300];
    char method[24], req_url[4096], extra[300];
};

static void md5hex(const char *s, char out[33]) {
    unsigned char d[EVP_MAX_MD_SIZE];
    unsigned l = 0;
    EVP_Digest(s, strlen(s), d, &l, EVP_md5(), NULL);
    for (unsigned i = 0; i < 16; i++) sprintf(out + 2 * i, "%02x", d[i]);
    out[32] = 0;
}

static void rtsp_unpct(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && hexv(p[1]) >= 0 && hexv(p[2]) >= 0) { *o++ = (char)(hexv(p[1]) * 16 + hexv(p[2])); p += 2; }
        else *o++ = *p;
    }
    *o = 0;
}

static void rtsp_send(up *u, const char *method, const char *url, const char *extra) {
    rtsp_st *r = u->rt;
    snprintf(r->method, sizeof r->method, "%s", method);
    snprintf(r->req_url, sizeof r->req_url, "%s", url);
    snprintf(r->extra, sizeof r->extra, "%s", extra ? extra : "");
    sb_printf(&u->req, "%s %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: %s\r\n", method, url, ++r->cseq, r->ua);
    if (r->session[0]) sb_printf(&u->req, "Session: %s\r\n", r->session);
    if (r->nonce[0] && r->digest) {
        char a1[600], a2[4200], h1[33], h2[33], rs[33], all[1200];
        snprintf(a1, sizeof a1, "%s:%s:%s", r->user, r->realm, r->pass);
        snprintf(a2, sizeof a2, "%s:%s", method, url);
        md5hex(a1, h1);
        md5hex(a2, h2);
        if (r->qop[0]) {
            snprintf(all, sizeof all, "%s:%s:%08x:%s:auth:%s", h1, r->nonce, r->cseq, "0a4f113b", h2);
            md5hex(all, rs);
            sb_printf(&u->req, "Authorization: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\", qop=auth, nc=%08x, cnonce=\"0a4f113b\"",
                      r->user, r->realm, r->nonce, url, rs, r->cseq);
        } else {
            snprintf(all, sizeof all, "%s:%s:%s", h1, r->nonce, h2);
            md5hex(all, rs);
            sb_printf(&u->req, "Authorization: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"", r->user, r->realm,
                      r->nonce, url, rs);
        }
        if (r->opaque[0]) sb_printf(&u->req, ", opaque=\"%s\"", r->opaque);
        sb_add(&u->req, "\r\n");
    } else if (r->realm[0] && !r->digest && u->u.auth[0]) sb_printf(&u->req, "Authorization: Basic %s\r\n", u->u.auth);
    sb_add(&u->req, r->extra);
    sb_add(&u->req, "\r\n");
    if (u->req_off == 0) up_send(u);
}

static void rtsp_attach(cl *c) {
    /* адрес как http://, чтобы разобрать общим разборщиком; порт по умолчанию — 554 */
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "http://%s", c->url + 7);
    http_url hu;
    if (http_split_url(tmp, &hu)) { client_upstream_failed(c, 404); return; }
    const char *auth_end = c->url + 7 + strcspn(c->url + 7, "/?#");
    const char *hs = c->url + 7, *at = NULL;
    for (const char *q = hs; q < auth_end; q++) if (*q == '@') at = q;
    const char *hp = at ? at + 1 : hs;
    int explicit_port = 0;
    for (const char *q = hp; q < auth_end; q++) if (*q == ':') explicit_port = 1;
    if (*hp == '[') explicit_port = memchr(hp, ']', (size_t)(auth_end - hp)) && strstr(hp, "]:") && strstr(hp, "]:") < auth_end;
    if (!explicit_port) snprintf(hu.port, sizeof hu.port, "554");
    up *u = calloc(1, sizeof *u);
    rtsp_st *r = calloc(1, sizeof *r);
    if (!u || !r) { free(u); free(r); client_upstream_failed(c, 502); return; }
    u->rt = r;
    if (at) {
        char ui[300];
        size_t l = (size_t)(at - hs);
        if (l >= sizeof ui) l = sizeof ui - 1;
        memcpy(ui, hs, l);
        ui[l] = 0;
        char *colon = strchr(ui, ':');
        if (colon) *colon = 0;
        snprintf(r->user, sizeof r->user, "%s", ui);
        snprintf(r->pass, sizeof r->pass, "%s", colon ? colon + 1 : "");
        rtsp_unpct(r->user);
        rtsp_unpct(r->pass);
    }
    /* адрес в запросах — без имени и пароля */
    snprintf(r->url, sizeof r->url, "rtsp://%.*s%s", (int)(auth_end - hp), hp, *auth_end ? auth_end : "/");
    char *hash = strchr(r->url, '#');
    if (hash) *hash = 0;
    snprintf(r->base, sizeof r->base, "%s", r->url);
    const char *ua = strcasestr(c->chan, "User-Agent:");
    if (ua) {
        ua += 11;
        while (*ua == ' ') ua++;
        snprintf(r->ua, sizeof r->ua, "%.*s", (int)strcspn(ua, "\r\n"), ua);
    } else snprintf(r->ua, sizeof r->ua, "PerfectTV/" PTV_VERSION " (VIDAA; native)");
    r->rtp_ch = 0;
    u->t = (tag){K_UP, u};
    u->fd = -1;
    u->ev = -1;
    u->st = -1;
    u->proto = P_RTSP;
    snprintf(u->key, sizeof u->key, "rtsp");
    u->u = hu;
    u->next = ups;
    ups = u;
    u->c = c;
    c->u = u;
    stream_head(u);
    up_resolve(u);
}

static void rtsp_connected(up *u) {
    u->st = U_RTSP;
    u->deadline = mono_now() + T_CONNECT;
    u->rt->phase = R_DESCRIBE;
    rtsp_send(u, "DESCRIBE", u->rt->url, "Accept: application/sdp\r\n");
}

static void rtsp_teardown(up *u) {
    rtsp_st *r = u->rt;
    if (u->fd < 0 || !r->session[0] || u->st != U_RTSP) return;
    char b[4600];
    int n = snprintf(b, sizeof b, "TEARDOWN %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: %s\r\nSession: %s\r\n\r\n", r->aggr[0] ? r->aggr : r->url, r->cseq + 1,
                     r->ua, r->session);
    if (n > 0 && n < (int)sizeof b && send(u->fd, b, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT) < 0) {}   /* как получится: сокет закрывается следом */
}

/* относительный a=control: к базе (как делают серверы и клиенты RTSP — дописать через «/») */
static void rtsp_ctl(const char *base, const char *ctl, char *out, size_t cap) {
    if (!ctl[0] || !strcmp(ctl, "*")) { snprintf(out, cap, "%s", base); return; }
    if (starts_with_ci(ctl, "rtsp://")) { snprintf(out, cap, "%s", ctl); return; }
    size_t bl = strlen(base);
    snprintf(out, cap, "%s%s%s", base, bl && base[bl - 1] == '/' ? "" : "/", ctl);
}

/* SDP: первое медиа с MPEG-TS; адреса для SETUP (медиа) и PLAY (весь сеанс) */
int relay_sdp_pick(const char *sdp, const char *base, char *media, char *aggr, size_t cap) {
    char sess_ctl[2048] = "", med_ctl[2048] = "", fmts[256] = "";
    int in_media = 0, ok = 0, found = 0;
    for (const char *p = sdp; p && *p && !found;) {
        size_t l = strcspn(p, "\r\n");
        char line[2048];
        snprintf(line, sizeof line, "%.*s", (int)(l < sizeof line - 1 ? l : sizeof line - 1), p);
        p += l;
        while (*p == '\r' || *p == '\n') p++;
        int last = !*p;
        if (!strncmp(line, "m=", 2) || last) {
            if (last && in_media) {
                if (!strncmp(line, "a=control:", 10)) snprintf(med_ctl, sizeof med_ctl, "%s", line + 10);
                if (!strncmp(line, "a=rtpmap:", 9) && strcasestr(line, "MP2T")) ok = 1;
            }
            if (in_media && ok) { found = 1; break; }
            if (!strncmp(line, "m=", 2)) {
                in_media = 1;
                ok = 0;
                med_ctl[0] = 0;
                /* m=video 0 RTP/AVP 33 */
                const char *f = line;
                for (int k = 0; k < 3 && f; k++) { f = strchr(f, ' '); if (f) f++; }
                snprintf(fmts, sizeof fmts, " %s ", f ? f : "");
                if (strstr(fmts, " 33 ")) ok = 1;
            }
            continue;
        }
        if (!strncmp(line, "a=control:", 10)) snprintf(in_media ? med_ctl : sess_ctl, sizeof med_ctl, "%s", line + 10);
        else if (in_media && !strncmp(line, "a=rtpmap:", 9) && strcasestr(line, "MP2T")) ok = 1;
    }
    if (!found && in_media && ok) found = 1;
    if (!found) return -1;
    rtsp_ctl(base, sess_ctl, aggr, cap);
    rtsp_ctl(base, med_ctl[0] ? med_ctl : sess_ctl, media, cap);
    return 0;
}

static void hdr_val(const char *head, const char *name, char *out, size_t cap) {
    out[0] = 0;
    size_t nl = strlen(name);
    for (const char *p = head; p && *p;) {
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ') v++;
            snprintf(out, cap, "%.*s", (int)strcspn(v, "\r\n"), v);
            return;
        }
        p = strstr(p, "\r\n");
        if (p) p += 2;
    }
}

/* Content-Length ответа/запроса RTSP: 0 — заголовка нет, -1 — битый/слишком большой. */
#define RTSP_BODY_MAX (4u << 20)
static int rtsp_body_len(const char *head, size_t *out) {
    char cl_[32];
    hdr_val(head, "Content-Length", cl_, sizeof cl_);
    if (!cl_[0]) { *out = 0; return 0; }
    char *end;
    long long v = strtoll(cl_, &end, 10);
    while (*end == ' ' || *end == '\t') end++;
    if (end == cl_ || *end || v < 0 || v > RTSP_BODY_MAX) return -1;
    *out = (size_t)v;
    return 1;
}
static void auth_param(const char *h, const char *name, char *out, size_t cap) {
    out[0] = 0;
    const char *p = strcasestr(h, name);
    while (p && p > h && p[-1] != ' ' && p[-1] != ',') p = strcasestr(p + 1, name);
    if (!p || p[strlen(name)] != '=') return;
    p += strlen(name) + 1;
    if (*p == '"') { p++; snprintf(out, cap, "%.*s", (int)strcspn(p, "\""), p); }
    else snprintf(out, cap, "%.*s", (int)strcspn(p, ", \r\n"), p);
}

static void rtsp_fail(up *u, int code, const char *why) {
    fprintf(stderr, "relay_rtsp_failed %s\n", why);
    up_fail(u, code);
}

/* ответ на запрос: head — заголовки (со строкой статуса), body — тело */
static void rtsp_response(up *u, int status, const char *head, const char *body) {
    rtsp_st *r = u->rt;
    if (r->phase == R_PLAYING) {   /* ответ на поддержку сеанса: не умеет GET_PARAMETER — дальше OPTIONS */
        if (status == 405 || status == 501 || status == 551) r->ka_options = 1;
        return;
    }
    if (status == 401 && r->auth_tries < 2 && r->user[0]) {
        char all[1024] = "";
        /* несколько WWW-Authenticate: Digest важнее */
        for (const char *p = head; p && *p;) {
            if (!strncasecmp(p, "WWW-Authenticate:", 17)) {
                const char *v = p + 17;
                while (*v == ' ') v++;
                size_t l = strcspn(v, "\r\n");
                if (!all[0] || !strncasecmp(v, "Digest", 6)) snprintf(all, sizeof all, "%.*s", (int)l, v);
            }
            p = strstr(p, "\r\n");
            if (p) p += 2;
        }
        r->auth_tries++;
        r->digest = !strncasecmp(all, "Digest", 6);
        auth_param(all, "realm", r->realm, sizeof r->realm);
        if (!r->realm[0]) snprintf(r->realm, sizeof r->realm, "-");
        auth_param(all, "nonce", r->nonce, sizeof r->nonce);
        auth_param(all, "opaque", r->opaque, sizeof r->opaque);
        auth_param(all, "qop", r->qop, sizeof r->qop);
        if (r->qop[0] && !strcasestr(r->qop, "auth")) r->qop[0] = 0;
        char m[24], url[4096], ex[300];
        snprintf(m, sizeof m, "%s", r->method);
        snprintf(url, sizeof url, "%s", r->req_url);
        snprintf(ex, sizeof ex, "%s", r->extra);
        rtsp_send(u, m, url, ex);
        return;
    }
    if (status < 200 || status >= 300) {
        char why[40];
        snprintf(why, sizeof why, "%s_%d", r->method, status);
        rtsp_fail(u, status == 401 || status == 403 || status == 404 ? status : 502, why);
        return;
    }
    char v[600];
    if (r->phase == R_DESCRIBE) {
        hdr_val(head, "Content-Base", v, sizeof v);
        if (!v[0]) hdr_val(head, "Content-Location", v, sizeof v);
        if (v[0]) snprintf(r->base, sizeof r->base, "%s", v);
        if (relay_sdp_pick(body ? body : "", r->base, r->media, r->aggr, sizeof r->media)) { rtsp_fail(u, 415, "no_mp2t"); return; }
        r->phase = R_SETUP;
        rtsp_send(u, "SETUP", r->media, "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n");
    } else if (r->phase == R_SETUP) {
        hdr_val(head, "Session", v, sizeof v);
        if (!v[0]) { rtsp_fail(u, 502, "no_session"); return; }
        char *semi = strchr(v, ';');
        int timeout = 60;
        if (semi) { char *t = strcasestr(semi, "timeout="); if (t) timeout = atoi(t + 8); *semi = 0; }
        snprintf(r->session, sizeof r->session, "%s", v);
        r->ka_period = timeout > 10 ? timeout / 2 : 5;
        hdr_val(head, "Transport", v, sizeof v);
        char *il = strcasestr(v, "interleaved=");
        if (!il) { rtsp_fail(u, 502, "no_tcp"); return; }   /* сервер не дал TCP — пусть плеер сам */
        r->rtp_ch = atoi(il + 12);
        r->phase = R_PLAY;
        rtsp_send(u, "PLAY", r->aggr, "Range: npt=0.000-\r\n");
    } else if (r->phase == R_PLAY) {
        r->phase = R_PLAYING;
        r->ka_at = mono_now() + r->ka_period;
        u->deadline = mono_now() + T_CONNECT;   /* до первых данных */
        fprintf(stderr, "relay_rtsp_playing\n");
    }
}

static void rtsp_keepalive(up *u, double now) {
    rtsp_st *r = u->rt;
    if (!r || r->phase != R_PLAYING || now < r->ka_at) return;
    r->ka_at = now + r->ka_period;
    if (r->ka_options) rtsp_send(u, "OPTIONS", r->aggr, NULL);
    else rtsp_send(u, "GET_PARAMETER", r->aggr, NULL);
}

/* разбор входящего: $-кадры (RTP поверх TCP) и ответы RTSP вперемешку */
static void rtsp_parse(up *u) {
    rtsp_st *r = u->rt;
    size_t pos = 0;
    while (!u->dead && pos < u->in_n) {
        uint8_t *p = (uint8_t *)u->in + pos;
        size_t n = u->in_n - pos;
        if (p[0] == '$') {
            if (n < 4) break;
            size_t l = (size_t)(p[2] << 8 | p[3]);
            if (n < 4 + l) break;
            if (p[1] == r->rtp_ch) {
                size_t pn;
                const uint8_t *pl = rtp_strip(p + 4, l, &pn);
                if (pl) stream_data(u, pl, pn);
            }
            pos += 4 + l;
            continue;
        }
        if (n >= 5 && !memcmp(p, "RTSP/", 5)) {
            char *e = memmem(p, n, "\r\n\r\n", 4);
            if (!e) { if (n > 64 * 1024) { rtsp_fail(u, 502, "head"); return; } break; }
            size_t hl = (size_t)(e - (char *)p) + 4;
            char *head = xstrndup((char *)p, hl);
            if (!head) { rtsp_fail(u, 502, "memory"); return; }
            size_t bl;
            if (rtsp_body_len(head, &bl) < 0) { free(head); rtsp_fail(u, 502, "content_length"); return; }
            if (bl > n - hl) { free(head); break; }
            char *body = xstrndup((char *)p + hl, bl);
            if (!body) { free(head); rtsp_fail(u, 502, "memory"); return; }
            const char *sp = strchr(head, ' ');
            int status = sp ? atoi(sp + 1) : 0;
            pos += hl + bl;
            rtsp_response(u, status, head, body);
            free(head);
            free(body);
            continue;
        }
        /* запрос сервера (ANNOUNCE, GET_PARAMETER…) — пропустить с телом; мусор — до следующего '$' или «RTSP/» */
        if (n >= 4 && p[0] >= 'A' && p[0] <= 'Z') {
            char *e = memmem(p, n, "\r\n\r\n", 4);
            if (!e) break;
            size_t hl = (size_t)(e - (char *)p) + 4;
            char *head = xstrndup((char *)p, hl);
            if (!head) { rtsp_fail(u, 502, "memory"); return; }
            size_t bl;
            int bad = rtsp_body_len(head, &bl) < 0;
            free(head);
            if (bad) { rtsp_fail(u, 502, "content_length"); return; }
            if (bl > n - hl) break;
            pos += hl + bl;
            continue;
        }
        pos++;
    }
    if (u->dead) return;   /* rtsp_fail/stream_data могли освободить u->in */
    memmove(u->in, u->in + pos, u->in_n - pos);
    u->in_n -= pos;
}

static void rtsp_event(up *u, uint32_t ev) {
    if ((ev & EPOLLOUT) && u->req_off < u->req.n) { up_send(u); if (u->dead) return; }
    if (!(ev & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR))) return;
    int eof = 0;
    for (;;) {
        if (u->in_cap - u->in_n < 16384) {
            size_t cap = u->in_cap ? u->in_cap * 2 : 65536;
            if (cap > (2u << 20)) { if (u->in_n > (1u << 20)) { rtsp_fail(u, 502, "overflow"); return; } break; }
            char *nb = realloc(u->in, cap);
            if (!nb) break;
            u->in = nb;
            u->in_cap = cap;
        }
        ssize_t rr = recv(u->fd, u->in + u->in_n, u->in_cap - u->in_n, 0);
        if (rr < 0) { if (errno == EAGAIN || errno == EINTR) break; eof = 1; break; }
        if (rr == 0) { eof = 1; break; }
        u->in_n += (size_t)rr;
        rtsp_parse(u);
        if (u->dead) return;
    }
    if (u->c && !u->c->dead) client_flush(u->c);
    if (u->dead) return;
    if (eof) rtsp_fail(u, 502, "closed");
}

/* ---------- плеер: запросы ---------- */
static void client_start(cl *c, char *req) {
    char path[12000] = "";
    if (sscanf(req, "GET %11999s", path) != 1 || strncmp(path, "/s?", 3)) { client_reply_simple(c, 404); return; }
    c->client_close = strcasestr(req, "\nConnection: close") != NULL;
    query_get(path + 3, "u", c->url, sizeof c->url);
    query_get(path + 3, "h", c->chan, sizeof c->chan);
    sb_clear(&c->fwd);
    sb_add(&c->fwd, c->chan);
    char *h0 = strstr(req, "\r\n");
    if (h0) each_line(h0 + 2, req_fn, &c->fwd, c->chan);
    pthread_mutex_lock(&D.mu);
    c->gen = D.gen;
    pthread_mutex_unlock(&D.mu);
    c->busy = 1;
    c->no_pool = 0;
    c->redirects = c->head_sent = c->decided = c->playlist = c->chunked_out = c->content_mpegurl = 0;
    sb_clear(&c->body);
    c->deadline = 0;
    set_ev(c->fd, &c->t, &c->ev, EPOLLRDHUP);   /* пока идёт ответ — только закрытие (и запись, если есть что) */
    client_attach(c);
}

/* в буфере — полный запрос? тогда начать */
static void client_next(cl *c) {
    if (c->busy || c->finishing || c->dead) return;
    c->in[c->in_n] = 0;
    char *end = strstr(c->in, "\r\n\r\n");
    if (!end) {
        if (c->in_n >= sizeof c->in - 1) { client_free(c); return; }
        c->deadline = mono_now() + T_IDLE;
        set_ev(c->fd, &c->t, &c->ev, EPOLLIN | EPOLLRDHUP);
        return;
    }
    size_t used = (size_t)(end - c->in) + 4;
    char req[sizeof c->in];
    memcpy(req, c->in, used - 4);
    req[used - 4] = 0;
    memmove(c->in, c->in + used, c->in_n - used);
    c->in_n -= used;
    client_start(c, req);
}

static void client_flush(cl *c) {
    while (out_pending(c)) {
        ssize_t w = send(c->fd, c->out.s + c->out_off, out_pending(c), MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EAGAIN || errno == EINTR) break;
            client_free(c);   /* плеер закрыл */
            return;
        }
        c->out_off += (size_t)w;
    }
    if (!out_pending(c)) { sb_clear(&c->out); c->out_off = 0; }
    /* провайдер стоял — плеер догнал */
    if (c->u && c->u->paused && out_pending(c) < OUT_HIGH / 2) {
        up *u = c->u;
        u->paused = 0;
        set_ev(u->fd, &u->t, &u->ev, EPOLLIN | EPOLLRDHUP);
        up_readable(u);   /* у TLS могли остаться расшифрованные данные, epoll о них не скажет */
        if (c->dead) return;
    }
    if (out_pending(c)) { set_ev(c->fd, &c->t, &c->ev, EPOLLOUT | EPOLLRDHUP | (c->busy ? 0 : EPOLLIN)); return; }
    if (c->finishing) {
        c->finishing = 0;
        if (!c->keep) { client_free(c); return; }
        client_next(c);   /* keep-alive: следующий запрос (мог уже прийти) */
        return;
    }
    set_ev(c->fd, &c->t, &c->ev, c->busy ? EPOLLRDHUP : EPOLLIN | EPOLLRDHUP);
}

static void client_event(cl *c, uint32_t ev) {
    if (ev & EPOLLIN) {
        ssize_t r = recv(c->fd, c->in + c->in_n, sizeof c->in - 1 - c->in_n, 0);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) { client_free(c); return; }
        if (r > 0) c->in_n += (size_t)r;
    }
    if ((ev & (EPOLLERR | EPOLLHUP)) || ((ev & EPOLLRDHUP) && !(ev & EPOLLIN))) { client_free(c); return; }
    if (ev & EPOLLOUT) { client_flush(c); if (c->dead) return; }
    client_next(c);
}

static void accept_all(void) {
    for (;;) {
        int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        if (nclients >= MAX_CLIENTS) {
            const char *busy = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            if (send(fd, busy, strlen(busy), MSG_NOSIGNAL) < 0) {}
            close(fd);
            continue;
        }
        cl *c = calloc(1, sizeof *c);
        if (!c) { close(fd); continue; }
        c->t = (tag){K_CLIENT, c};
        c->fd = fd;
        c->ev = -1;
        c->next = clients;
        clients = c;
        nclients++;
        c->deadline = mono_now() + T_IDLE;
        set_ev(fd, &c->t, &c->ev, EPOLLIN | EPOLLRDHUP);
    }
}

static void up_event(up *u, uint32_t ev) {
    if (u->st == U_CONNECT) {
        int e = 0;
        socklen_t l = sizeof e;
        getsockopt(u->fd, SOL_SOCKET, SO_ERROR, &e, &l);
        if (e || (ev & (EPOLLERR | EPOLLHUP))) {   /* следующий адрес */
            epoll_ctl(ep, EPOLL_CTL_DEL, u->fd, NULL);
            close(u->fd);
            u->fd = -1;
            u->ev = -1;
            up_connect_next(u);
            return;
        }
        up_after_connect(u);
        return;
    }
    if (u->proto == P_UDP) { udp_event(u); return; }
    if (u->proto == P_RTSP) { rtsp_event(u, ev); return; }
    if (u->st == U_TLS) {
        int r = up_tls_step(u);
        if (r < 0) up_fail(u, 502);
        else if (r) { u->st = U_SEND; up_send(u); }
        return;
    }
    if (u->st == U_SEND) { up_send(u); return; }
    if (u->st == U_IDLE) { up_free(u); return; }   /* в пуле что-то пришло (обычно — закрытие) */
    if (ev & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) up_readable(u);
}

static void timeouts(void) {
    double now = mono_now();
    for (up *u = ups, *n; u; u = n) {
        n = u->next;
        if (!u->dead && u->proto == P_RTSP) rtsp_keepalive(u, now);
        if (!u->dead && !u->paused && u->deadline && now > u->deadline) {
            if (u->st == U_IDLE) up_free(u);
            else { fprintf(stderr, "relay_timeout state=%d\n", u->st); up_fail(u, 504); }
        }
    }
    for (cl *c = clients, *n; c; c = n) {
        n = c->next;
        if (!c->dead && !c->busy && !out_pending(c) && c->deadline && now > c->deadline) client_free(c);
    }
}

static void *loop(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "ptv-relay");
    struct epoll_event ev[64];
    for (;;) {
        int pending = 0;
        for (up *u = ups; u; u = u->next) pending |= u->again && !u->paused && !u->dead;
        int n = epoll_wait(ep, ev, 64, pending ? 0 : 500);
        for (int i = 0; i < n; i++) {
            tag *t = ev[i].data.ptr;
            if (t->kind == K_LISTEN) accept_all();
            else if (t->kind == K_WAKE) dns_done();
            else if (t->kind == K_CLIENT) { cl *c = t->p; if (!c->dead) client_event(c, ev[i].events); }
            else if (t->kind == K_UP) { up *u = t->p; if (!u->dead) up_event(u, ev[i].events); }
        }
        for (up *u = ups, *nx; u; u = nx) {   /* дочитать TLS (список мог измениться — следующий берём заранее) */
            nx = u->next;
            if (u->again && !u->paused && !u->dead && (u->st == U_HEAD || u->st == U_BODY)) { u->again = 0; up_readable(u); }
        }
        timeouts();
        for (int i = 0; i < ndead; i++) free(dead[i]);   /* после пачки: на них могли ссылаться её события */
        ndead = 0;
    }
    return NULL;
}

int relay_start(void) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    if (lfd >= 0) { pthread_mutex_unlock(&mu); return 0; }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    int one = 1, p[2] = {-1, -1};
    if (fd >= 0) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    for (int pp = PORT; fd >= 0 && pp < PORT + 10 && !port; pp++) {
        struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)pp)};
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (!bind(fd, (struct sockaddr *)&sa, sizeof sa)) port = pp;
    }
    ep = epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0 || !port || listen(fd, 32) || ep < 0 || pipe2(p, O_NONBLOCK | O_CLOEXEC)) {
        if (fd >= 0) close(fd);
        port = 0;
        pthread_mutex_unlock(&mu);
        return -1;
    }
    lfd = fd;
    wake_r = p[0];
    wake_w = p[1];
    struct epoll_event e = {.events = EPOLLIN, .data.ptr = &t_listen};
    epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &e);
    e.data.ptr = &t_wake;
    epoll_ctl(ep, EPOLL_CTL_ADD, wake_r, &e);
    pthread_mutex_lock(&D.mu);
    init_once();
    pthread_mutex_unlock(&D.mu);
    http_init(NULL, 0);
    pthread_t t;
    if (pthread_create(&t, NULL, loop, NULL)) { close(lfd); lfd = -1; port = 0; pthread_mutex_unlock(&mu); return -1; }
    pthread_detach(t);
    pthread_mutex_unlock(&mu);
    fprintf(stderr, "relay_start port=%d\n", port);
    return 0;
}
