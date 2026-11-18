/* Потоковый HTTP(S) для плейлистов и программы: тело читается кусками, в память целиком не попадает
 * (docs/TZ.md, §4–5). Вызывать только из рабочих потоков (pool_submit). Поддерживает редиректы,
 * chunked, Basic из user:pass@host, If-None-Match / If-Modified-Since, file:// и абсолютные пути. */
#ifndef PTV_HTTP_H
#define PTV_HTTP_H
#include <stddef.h>
#include <stdint.h>

typedef struct hs hs;
typedef struct {
    const char *url;
    const char *headers;        /* "Name: v\r\n"..., или NULL */
    const char *etag, *last_modified;   /* условный запрос: 304 — данные не менялись */
    int timeout_ms;             /* чтение; по умолчанию 30000 */
    volatile int *cancel;       /* != 0 — прервать */
    int raw;                    /* без сжатия (Accept-Encoding: identity) — ретранслятор */
    int keepalive;              /* соединение — из пула и обратно в пул, если ответ дочитан */
    int cookies;                /* хранилище cookie потока канала (core/cookies.h): ставить и запоминать */
} hs_req;

void http_init(const char *ca_pem, size_t ca_len);   /* один раз в главном потоке */
/* NULL — ошибка, причина в err («dns: …», «connect failed», «tls: …», «http_404», «timeout»). */
hs *hs_open(const hs_req *rq, char *err, size_t errn);
int hs_status(const hs *h);
const char *hs_url(const hs *h);          /* адрес после редиректов */
/* Разбор адреса http(s): host без скобок IPv6, port — всегда (80/443 по умолчанию), auth — Basic в base64. */
typedef struct { char scheme[8], host[256], port[8], path[4096], auth[300]; } http_url;
int http_split_url(const char *url, http_url *out);
/* Контекст TLS (SSL_CTX *): any — без проверки сертификата («Принимать любые сертификаты»). */
void *http_ssl_ctx(int any);
const char *hs_headers(const hs *h);      /* заголовки ответа «Имя: значение\r\n…» */
/* Относительный адрес ref от base (как браузер). */
void http_resolve(const char *base, const char *ref, char *out, size_t cap);
long long hs_length(const hs *h);         /* Content-Length или -1 */
long long hs_received(const hs *h);       /* байтов тела прочитано */
const char *hs_etag(const hs *h);
const char *hs_last_modified(const hs *h);
/* >0 — байты, 0 — конец, <0 — ошибка (текст в hs_error) */
long hs_read(hs *h, void *buf, size_t n);
const char *hs_error(const hs *h);
void hs_close(hs *h);
/* Всё тело в память (небольшие ответы); NULL — ошибка. */
char *http_get_small(const char *url, size_t max, size_t *len, char *err, size_t errn);
/* Разбор даты HTTP («Sun, 06 Nov 1994 08:49:37 GMT»), 0 — не разобрано. */
int64_t http_date(const char *s);
#endif
