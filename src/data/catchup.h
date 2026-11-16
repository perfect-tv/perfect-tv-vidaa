/* Шаблоны адресов архива (docs/TZ.md, §8). Подстановки:
 *   ${start} ${utc} ${end} ${utcend} ${offset} ${duration} ${now} ${lutc} ${timestamp}
 *   ${(b)yyyy-MM-dd:HH-mm} начало; ${(e)…} конец; ${(n)…} сейчас; (bu) — UTC; (b+3) — сдвиг пояса
 *   ${(d)M} ${(d)S} ${(d)h} ${(d)m} — длительность */
#ifndef PTV_CATCHUP_H
#define PTV_CATCHUP_H
#include <stddef.h>
#include <stdint.h>

/* Смещение пояса в секундах для момента t (Москва: 10800) */
typedef int (*tz_fn)(int64_t t);

/* Шаблон архива канала; 0 — готово, -1 — архив не поддерживается (тип disabled/неизвестный). */
int catchup_template(const char *url, const char *type, const char *source, char *out, size_t cap);
/* Flussonic с HLS-окном archive-/mono-/video-…-${start}-${duration}.m3u8 */
int catchup_flussonic_hls(const char *type, const char *tpl);
/* EVENT-ссылка Flussonic: …-${start}-now.m3u8 */
void catchup_flussonic_now(const char *tpl, char *out, size_t cap);
/* Подставить значения: start < 0 — без диапазона (эфир). */
void catchup_prepare(const char *tpl, int64_t start, int64_t duration, int64_t now, tz_fn tz, char *out, size_t cap);
#endif
