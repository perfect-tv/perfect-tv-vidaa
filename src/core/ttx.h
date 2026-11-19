/* Телетекст DVB (EN 300 472 — упаковка в PES, EN 300 706 — страницы), уровень 1.0: наборы G0 (латиница с
 * национальными вариантами, кириллица), мозаика, двойная высота, Fastext, страницы субтитров. Перенос
 * teletext/Ttx.kt Android-версии, включая расширения X/26 (G2 и диакритика). Подаёт данные поток ретранслятора (core/relay.c),
 * читает интерфейс — под замком. */
#ifndef PTV_TTX_H
#define PTV_TTX_H
#include <stdint.h>
#include <stddef.h>

#define TTX_COLS 40
#define TTX_ROWS 25
enum { TTXF_ERASE = 1, TTXF_NEWSFLASH = 2, TTXF_SUBTITLE = 4, TTXF_SUPPRESS_HEADER = 8, TTXF_SERIAL = 128 };

typedef struct {
    uint16_t number, subcode, flags, links[6];   /* number — BCD 0x100…0x8FF; links: красная, зелёная, жёлтая, голубая, …, индекс */
    uint8_t charset;
    uint8_t has[TTX_ROWS];
    uint8_t rows[TTX_ROWS][TTX_COLS];           /* 7 бит, как пришли */
    uint8_t enh_has;                             /* какие обозначения X/26 пришли (бит 0…15) */
    int32_t enh[16][13];                         /* X/26: слова «адрес, режим, данные» (18 бит) */
} ttx_page;

typedef struct ttx ttx;
ttx *ttx_new(void);
void ttx_free(ttx *t);
void ttx_reset(ttx *t);
/* дорожка из PMT: язык (кириллица по умолчанию для rus/ukr/bul…) и страницы субтитров из дескриптора */
void ttx_declare(ttx *t, const char *lang, const int *sub_pages, int n);
int ttx_declared(ttx *t);
/* Данные PES (с data_identifier); pts — 90 кГц (-1 — нет). */
void ttx_feed(ttx *t, const uint8_t *d, size_t n, int64_t pts);
int ttx_version(ttx *t);
int ttx_count(ttx *t);
/* Страница: sub — номер подстраницы по порядку (-1 — последняя принятая); *nsub — сколько их. 0 — есть. */
int ttx_get(ttx *t, int number, int sub, ttx_page *out, int *nsub);
/* Соседний принятый номер (dir ±1, по кругу); 0 — страниц нет. */
int ttx_neighbour(ttx *t, int number, int dir);
/* Страница субтитров, показанная на кадре pts (последняя не позже); 0 — есть. */
int ttx_subtitle_at(ttx *t, int64_t pts, ttx_page *out);
int ttx_has_subtitles(ttx *t);
/* номер страницы субтитров (0 — ещё не определена); объявлены ли субтитры в потоке (дескриптор / страница) */
int ttx_subtitle_number(ttx *t);
int ttx_subtitles_known(ttx *t);
/* принятые номера страниц по возрастанию (без повторов); сколько */
int ttx_numbers(ttx *t, uint16_t *out, int max);

/* ---- раскладка ---- */
enum { TA_MOSAIC = 1 << 6, TA_SEPARATED = 1 << 7, TA_DH = 1 << 8, TA_DH_BOTTOM = 1 << 9, TA_FLASH = 1 << 10, TA_CONCEAL = 1 << 11,
       TA_BOX = 1 << 12, TA_DW = 1 << 13, TA_DW_RIGHT = 1 << 14 };
typedef struct { uint32_t ch[TTX_COLS * TTX_ROWS]; uint16_t attr[TTX_COLS * TTX_ROWS]; } ttx_grid;   /* attr: fg 0–2, bg 3–5, TA_* */
/* row0 — строка 0 (заголовок «P100 …»), NULL — пустая */
void ttx_layout(const ttx_page *p, const uint8_t *row0, ttx_grid *g);
void ttx_header_row(int number, const ttx_page *p, const char *typed, uint8_t out[TTX_COLS]);
/* Текст субтитра: строки (только «окна», если есть) через \n, без пустых. Длина. */
size_t ttx_subtitle_text(const ttx_grid *g, char *out, size_t cap);
uint32_t ttx_glyph(int c, int charset);
uint32_t ttx_g2(int c);                        /* знак G2 (X/26), код 0x20…0x7F */
uint8_t ttx_rev(uint8_t b);
int ttx_ham84(uint8_t b);
#endif
