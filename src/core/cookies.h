/* Cookie провайдера для потока канала (RFC 6265, без списка публичных суффиксов). Плеер ходит к ретранслятору на
 * 127.0.0.1 и не знает настоящих адресов, поэтому cookie хранит и подставляет сам ретранслятор (core/relay.c) и
 * загрузчик субтитров (core/webvtt.c через hs_req.cookies): Domain (с поддоменами) и без него — только тот же хост,
 * Path (без него — каталог адреса), Secure — только по https (и принимается только из ответа по https), Max-Age / Expires (время — по Date ответа, если есть),
 * удаление прошедшим сроком. Плееру Set-Cookie не отдаются, его Cookie не пересылаются. Хранилище — в памяти, одно на
 * поток канала: relay_reset очищает. Потокобезопасно (свой замок). */
#ifndef PTV_COOKIES_H
#define PTV_COOKIES_H
#include <stddef.h>
#include <stdint.h>
#include "util.h"

/* Set-Cookie (значение заголовка) из ответа на url; date — заголовок Date ответа (unix, 0 — нет) */
void cookies_set(const char *url, const char *set_cookie, int64_t date);
/* Cookie для запроса на url: «a=1; b=2» в out (длиннее путь — раньше); длина, 0 — нечего */
size_t cookies_header(const char *url, char *out, size_t cap);
/* Set-Cookie из блока заголовков ответа («Имя: значение\r\n…») */
void cookies_from_headers(const char *url, const char *headers);
/* заголовки запроса headers («Имя: значение\r\n…») в out, их Cookie — вместе с подходящими к url одной строкой */
void cookies_merge(sbuf *out, const char *headers, const char *url);
void cookies_clear(void);
int cookies_count(void);
#endif
