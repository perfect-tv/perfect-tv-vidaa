/* Субтитры DVB (ETSI EN 300 743) — картинками: сегменты страницы (0x10), областей (0x11), палитры (0x12), объектов
 * (0x13: строки пикселей с кодами длин серий 2/4/8 бит, поля верхнее и нижнее), размера экрана (0x14), конец набора
 * (0x80). Набор становится «показом»: области с позицией и пикселями ARGB, время — PTS PES, держится до следующего
 * показа или page_time_out. Подаёт данные поток ретранслятора, читает интерфейс — под замком. Своё, без libavcodec:
 * FFmpeg в SDK другой версии, чем на ПК, а тесты идут на ПК. */
#ifndef PTV_DVBSUB_H
#define PTV_DVBSUB_H
#include <stddef.h>
#include <stdint.h>

#define DVBSUB_REGIONS 8
typedef struct { int x, y, w, h; uint32_t *px; } dvbsub_region;   /* px — ARGB, w*h */
typedef struct {
    int64_t pts, end;            /* 90 кГц; end — page_time_out (-1 — до следующего) */
    int dw, dh;                  /* размер экрана (по умолчанию 720×576) */
    int nreg;
    dvbsub_region reg[DVBSUB_REGIONS];
    int id;                      /* растёт с каждым показом */
} dvbsub_show;

typedef struct dvbsub dvbsub;
/* composition / ancillary — page_id из дескриптора 0x59 (0 — принимать любые) */
dvbsub *dvbsub_new(int composition_page, int ancillary_page);
void dvbsub_free(dvbsub *d);
/* Данные PES после заголовка (data_identifier 0x20, subtitle_stream_id 0x00, сегменты, 0xFF). */
void dvbsub_feed(dvbsub *d, const uint8_t *data, size_t n, int64_t pts);
/* Показ на кадре pts: копия в out (если его id не равен have — иначе 0 и out не трогается). 1 — новый показ (out
 * заполнен, пиксели в куче — dvbsub_show_clear), 0 — тот же, -1 — ничего не показывать. */
int dvbsub_at(dvbsub *d, int64_t pts, int have, dvbsub_show *out);
void dvbsub_show_clear(dvbsub_show *s);
int dvbsub_count(dvbsub *d);   /* принято показов */
#endif
