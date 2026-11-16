/* Потоковый разбор M3U/M3U8 (docs/TZ.md, §4): куски байтов подаются в m3u_push(), строки разбираются
 * по мере поступления, каналы отдаются по одному. Весь файл в памяти не держится. */
#ifndef PTV_M3U_H
#define PTV_M3U_H
#include <stddef.h>

typedef struct {
    int num;
    const char *name, *url, *group, *logo, *tvg_id, *tvg_name;
    double tvg_shift;
    int has_catchup;               /* 0 — архива нет */
    const char *cu_type, *cu_source;
    int cu_days;                   /* 0 — «по умолчанию из настроек» */
    const char *user_agent, *referrer, *headers_json;
} m3u_channel;                     /* строки живут до возврата из колбэка */

typedef void (*m3u_cb)(const m3u_channel *c, void *ctx);
typedef struct m3u m3u;

m3u *m3u_new(m3u_cb cb, void *ctx);
void m3u_push(m3u *p, const char *data, size_t n);
void m3u_end(m3u *p);
int m3u_count(const m3u *p);
int m3u_rejected(const m3u *p);    /* первая строка не #EXTM3U */
int m3u_is_m3u(const m3u *p);
/* Адреса программы из заголовка (url-tvg, x-tvg-url, tvg-url; несколько через запятую или пробел), через \n. */
const char *m3u_epg_urls(const m3u *p);
void m3u_free(m3u *p);

/* Для тестов */
const char *m3u_attr(const char *attrs, const char *key, char *out, size_t cap);   /* NULL — нет */
#endif
