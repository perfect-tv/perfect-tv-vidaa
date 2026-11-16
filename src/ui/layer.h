/* Стек слоёв экрана ТВ: верхний слой получает клавиши, без слоёв — плеер (tv.c). Рисуются снизу вверх
 * поверх видео и инфопанели. */
#ifndef PTV_LAYER_H
#define PTV_LAYER_H
#include "gfx.h"

typedef struct layer layer;
struct layer {
    const char *name;
    int (*key)(layer *l, int key, int ev);       /* 1 — обработано */
    int (*has_long)(layer *l, int key);          /* есть долгое действие: короткое — при отпускании */
    void (*draw)(layer *l, gfx_t *g);
    void (*tick)(layer *l, double now);
    void (*closed)(layer *l);                    /* снят со стека */
    int no_hold;                                 /* удержание стрелок не повторяет (PIN) */
    int covers;                                  /* закрывает весь экран: слои и инфопанель под ним не рисовать */
    void *st;
};

void layer_push(layer *l);
void layer_pop(layer *l);           /* снять этот слой (и всё, что над ним) */
void layer_pop_all(void);
layer *layer_top(void);
int layer_count(void);
layer *layer_at(int i);             /* 0 — нижний */
int layer_has(const layer *l);
/* в долгих действиях клавиши верхнего слоя */
int layer_dispatch(int key, int ev);
int layer_top_has_long(int key);
void layer_draw_all(gfx_t *g);
void layer_tick_all(double now);
#endif
