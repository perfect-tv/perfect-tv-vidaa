/* Текстовые субтитры из HLS (#EXT-X-MEDIA:TYPE=SUBTITLES): WebVTT, TTML / IMSC (текстовый профиль) — сегментами
 * XML или fMP4 (stpp), WebVTT в fMP4 (wvtt); вид сегмента определяется по содержимому, у fMP4 шкала времени — из
 * #EXT-X-MAP. Время fMP4 и TTML — по шкале потока (секунды × 90000 = метки PTS; так требует HLS для дорожек одной
 * программы), TTML внутри образца может быть и от начала образца — это видно по длинам. Свой поток читает плейлист
 * субтитров и сегменты
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
/* TTML-документ: base_sec — начало отсчёта времени документа */
int ttml_parse(const char *xml, size_t n, double base_sec, webvtt_cue_fn cb, void *ctx);
enum { MP4SUB_STPP = 1, MP4SUB_WVTT = 2 };
typedef struct { int kind; uint32_t timescale; } mp4sub_init;
int mp4sub_init_parse(const uint8_t *d, size_t n, mp4sub_init *in);   /* -1 — нет дорожки stpp / wvtt */
int mp4sub_parse(const uint8_t *d, size_t n, const mp4sub_init *in, webvtt_cue_fn cb, void *ctx);
/* сегмент любого из видов (in — инициализация fMP4 или NULL) */
int webvtt_segment(const char *d, size_t n, const mp4sub_init *in, webvtt_cue_fn cb, void *ctx);
#endif
