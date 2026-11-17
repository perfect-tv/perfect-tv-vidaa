/* Экранная клавиатура 11 × 5 (форма и поиск): три ряда букв (ЙЦУКЕН / QWERTY / символы с кусками адресов), ряд цифр
 * и служебный ряд (⇧, язык, символы, пробел, ⌫ — удержание OK очищает, «Готово»). Цифры пульта вводятся сразу,
 * CH+ — стереть. С edges стрелки у края не заворачивают, а выводят из клавиатуры (KB_OUT_*). */
#ifndef PTV_KBD_H
#define PTV_KBD_H
#include "gfx.h"

#define KB_COLS 11
typedef struct {
    char text[2048];
    int row, col, lang, shift, sym;   /* lang 0 en, 1 ru */
    int edges;
} kb;
enum { KB_NONE = 0, KB_DONE, KB_OUT_RIGHT, KB_OUT_DOWN, KB_OUT_LEFT };

int kb_key(kb *k, int key, int ev);
int kb_long(const kb *k, int key);        /* у клавиши в фокусе есть долгое действие (⌫) */
void kb_type(kb *k, const char *s);
void kb_back(kb *k);
void kb_draw(gfx_t *g, const kb *k, int x, int y, int w, int focused);   /* без фокуса — клавиша в рамке */
int kb_height(void);
#endif
