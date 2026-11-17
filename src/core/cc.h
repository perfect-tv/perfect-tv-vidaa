/* Закрытые субтитры CEA-608 и CEA-708, вложенные в видео (ATSC A/53: cc_data в SEI H.264 / HEVC типа 4
 * «user_data_registered_itu_t_t35» с GA94 и в user_data MPEG-2). Системный плеер их не выдаёт даже дорожкой —
 * ретранслятор (core/relay.c) отдаёт сюда начало каждого PES видео, отсюда — строки для показа по времени кадра.
 * Кадры в потоке идут в порядке декодирования (B-кадры), а cc_data — в порядке показа: пары упорядочиваются по PTS
 * (ждут ~0,3 с). Каналы 608: CC1 / CC2 (поле 1), CC3 / CC4 (поле 2); 708 — сервисы 1…63 (DTVCC). Какие есть —
 * видно по данным (строки с текстом или команды субтитров), декодер работает только у выбранного.
 * Не потокобезопасно: зовёт ретранслятор под своим замком. */
#ifndef PTV_CC_H
#define PTV_CC_H
#include <stddef.h>
#include <stdint.h>

enum { CC_608 = 1, CC_708 = 2 };
enum { CC_VIDEO_MPEG2 = 1, CC_VIDEO_H264, CC_VIDEO_HEVC };

/* строка показа: положение — доли безопасной области (x — левый край или середина / правый при align 1 / 2,
 * y — верх строки), высота строки — доля 1/15 */
typedef struct { float x, y; int align; uint32_t color; int italic, underline; char text[256]; } cc_row;
typedef struct { int64_t pts; int n; cc_row row[16]; } cc_show;

typedef struct ccdec ccdec;
ccdec *cc_new(void);
void cc_free(ccdec *d);
void cc_reset(ccdec *d);
/* начало PES видео (данные после заголовка PES) и его PTS (-1 — нет) */
void cc_video(ccdec *d, int codec, const uint8_t *es, size_t n, int64_t pts);
/* тройки cc_data (флаги/тип, байт 1, байт 2) кадра с меткой pts — для тестов и MPEG-2 */
void cc_feed(ccdec *d, int64_t pts, const uint8_t *triplets, int n);
/* сбросить ожидающие упорядочивания (конец потока, тесты) */
void cc_flush(ccdec *d);
/* что есть в потоке: биты CC1…CC4 (1 << (n − 1)); сервисы 708 — биты 1 << n */
unsigned cc_present608(const ccdec *d);
uint64_t cc_present708(const ccdec *d);
/* выбор: CC_608 1…4 или CC_708 1…63; kind 0 — выключить */
void cc_select(ccdec *d, int kind, int num);
/* показ на кадре pts (последний не позже); 1 — есть, 0 — пусто */
int cc_at(const ccdec *d, int64_t pts, cc_show *out);
#endif
