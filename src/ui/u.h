/* Масштаб, палитра и рисование (docs/design.md). Холст — 1920×1080 ARGB: пиксели с alpha 0 — «дыра», под ней
 * видео системного плеера. Полупрозрачное рисуется d_* с честным сложением прозрачности (gfx_* каркаса
 * делает пиксель непрозрачным — годится только поверх непрозрачного).
 * 1 em = высота экрана / 48 (настройка «Размер интерфейса»: 54 / 48 / 43,2) — то же, что 10 dp у Android-версии. */
#ifndef PTV_U_H
#define PTV_U_H
#include "gfx.h"
#include "text.h"
#include <stdint.h>

#define SW 1920
#define SH 1080
extern double EM;
#define E(x) ((int)((x) * EM + 0.5))
void u_init(void);

/* «Белый» — #EBEBEB: чистый белый на ТВ режет глаза */
#define C_WHITE 0xffebebebu
#define WA(a) ((((uint32_t)((a) * 255.0 + 0.5)) << 24) | 0xebebebu)
#define BA(a) (((uint32_t)((a) * 255.0 + 0.5)) << 24)
#define C_BLACK 0xff000000u
#define C_FILL WA(0.05)
#define C_FILL_SOFT WA(0.06)
#define C_FILL_CELL WA(0.09)
#define C_FILL_BTN WA(0.12)
#define C_FILL_NOW WA(0.17)
#define C_FILL_MARKED WA(0.18)
#define C_FILL_ACTIVE WA(0.22)
#define C_FILL_PLAYING WA(0.30)
#define C_LINE WA(0.08)
#define C_LINE_STRONG WA(0.10)
#define C_PANEL 0xeb1a1b1cu
#define C_POPUP 0xf01e1f20u
#define C_OVERLAY 0x73000000u
#define C_SCRIM 0xf708090bu
#define C_GUIDE_TOP 0xf70c0d0eu
#define C_GUIDE_BOTTOM 0xcc0c0d0eu
#define C_SETTINGS_BG 0xff121314u
#define C_LOGO_PLATE 0xff2a2c2fu
#define C_LIVE 0xffe53935u
#define C_ARCHIVE 0xffffb84du
#define C_ON_ARCHIVE 0xff201400u
#define C_NUM_PLATE 0xc71a1b1cu
#define C_TEXT2 WA(0.70)
#define C_TEXT3 WA(0.55)
#define C_HINT WA(0.50)

/* шрифты: UI — Onest, F_B — жирный */
enum { F_R = TX_UI, F_S = TX_UI_SEMI, F_B = TX_UI_BOLD };

/* --- заливки с честной прозрачностью --- */
void d_fill(gfx_t *g, int x, int y, int w, int h, uint32_t c);
void d_bitmap(gfx_t *g, const uint32_t *src, int sw, int sh, int x, int y, int w, int h);   /* ARGB, масштаб */
void d_round(gfx_t *g, int x, int y, int w, int h, int r, uint32_t c);
void d_stroke(gfx_t *g, int x, int y, int w, int h, int r, int t, uint32_t c);
void d_vgrad(gfx_t *g, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void d_hgrad(gfx_t *g, int x, int y, int w, int h, uint32_t left, uint32_t right);
void d_clear(gfx_t *g, uint32_t c);       /* весь холст, c может быть 0 — «дыра» */
void d_poly(gfx_t *g, const float *xy, int n, uint32_t c);   /* выпуклый или нет — чётно-нечётное правило, сглаживание */
void d_line(gfx_t *g, float x0, float y0, float x1, float y1, float w, uint32_t c);
void d_circle(gfx_t *g, float cx, float cy, float r, uint32_t c);
void d_arc(gfx_t *g, float cx, float cy, float r, float w, float a0, float a1, uint32_t c);   /* углы в градусах, 0 — вправо, по часовой */

/* --- текст: размер в em --- */
int d_text(gfx_t *g, int x, int y, double em, int face, uint32_t c, const char *s, int maxw);
int d_text_r(gfx_t *g, int right, int y, double em, int face, uint32_t c, const char *s);
int d_text_c(gfx_t *g, int x, int w, int y, double em, int face, uint32_t c, const char *s);
/* по вертикальному центру cy (по высоте строчных и прописных букв) */
int d_text_mid(gfx_t *g, int x, int cy, double em, int face, uint32_t c, const char *s, int maxw);
int d_text_w(double em, int face, const char *s);
int d_lines(gfx_t *g, int x, int y, int w, double em, int face, uint32_t c, const char *s, int max_lines, double gap_em);
int d_line_h(double em, int face);

/* --- значки (векторные: в шрифтах нет ▶ ❚❚ ⟲ ★) --- */
enum { I_PLAY, I_PAUSE, I_ARCHIVE, I_CLOCK, I_LOCK, I_HEART, I_STAR, I_CHECK, I_LEFT, I_RIGHT, I_UP, I_DOWN, I_CHEVRON, I_SEARCH,
       I_CLOSE, I_LIST, I_GRID, I_LIVE, I_GEAR, I_STOP, I_BACKSPACE, I_HISTORY, I_INFO, I_REFRESH, I_EYE_OFF, I_PLUS };
void d_icon(gfx_t *g, int kind, int cx, int cy, int size, uint32_t c);

/* --- элементы --- */
void d_spinner(gfx_t *g, int cx, int cy, int r, double now);
/* Бейдж: скруглённая плашка с полями; возвращает ширину. icon -1 — без значка. */
int d_badge(gfx_t *g, int x, int cy, double em, uint32_t bg, uint32_t fg, int icon, const char *text);
int d_badge_w(double em, int icon, const char *text);
/* Подсказки клавиш: пары «клавиша — действие» в строку от x; клавиша — текст или стрелка (I_LEFT…). */
typedef struct { int icon; const char *key; const char *action; } hint;
int d_hints(gfx_t *g, int x, int cy, const hint *h, int n);
int d_hints_w(const hint *h, int n);
/* Затемнение краёв списка, если в ту сторону есть ещё строки (поверх уже нарисованного, фоном bg). */
void d_fade_edges(gfx_t *g, int x, int y, int w, int h, int top, int bottom, uint32_t bg);

/* Логотип канала: подложка, картинка с отступом 14%, монограмма, если нет картинки; номер — бейджем. */
struct channel;
void d_logo(gfx_t *g, const struct channel *c, int x, int y, int w, int h, int r, int badge, int on_white);
/* Картинка по адресу (вписать/обрезать), NULL — ещё нет. */
int d_image(gfx_t *g, const char *url, int x, int y, int w, int h, int r, int cover);
#endif
