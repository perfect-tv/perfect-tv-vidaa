/* Субтитры WebVTT из HLS (#EXT-X-MEDIA:TYPE=SUBTITLES): свой поток читает плейлист субтитров и сегменты .vtt
 * (блокирующий клиент core/http.c с заголовками канала), реплики привязываются к меткам потока через
 * X-TIMESTAMP-MAP (MPEGTS ↔ LOCAL; нет — MPEGTS:0). Эфир — плейлист перечитывается раз в полдлительности сегмента,
 * берутся новые сегменты; запись (#EXT-X-ENDLIST) — только сегменты около позиции плеера (webvtt_clock).
 * Показывает интерфейс: webvtt_text_at — под замком. */
#ifndef PTV_WEBVTT_H
#define PTV_WEBVTT_H
#include <stddef.h>
#include <stdint.h>

/* Начать для плейлиста субтитров url (абсолютный адрес провайдера) с заголовками канала; прежний — остановить. */
void webvtt_start(const char *url, const char *headers);
void webvtt_stop(void);
int webvtt_running(void);
/* время кадра (метки 90 кГц) и позиция в ссылке (с) — для выбора сегментов записи; главный поток, каждый кадр */
void webvtt_clock(int64_t frame_pts, double position);
/* текст реплик на кадре pts (строки через \n; несколько реплик — подряд); длина, 0 — ничего */
size_t webvtt_text_at(int64_t pts, char *out, size_t cap);
int webvtt_cues(void);

/* разбор (тесты): сегмент .vtt -> реплики через cb (начало, конец — метки 90 кГц) */
typedef void (*webvtt_cue_fn)(void *ctx, int64_t start, int64_t end, const char *text);
int webvtt_parse(const char *text, size_t n, webvtt_cue_fn cb, void *ctx);
#endif
