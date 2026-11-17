/* Поиск (docs/controls.md, «Поиск»): каналы — сразу, по названию (все слова) или номеру; передачи — по названию,
 * через 0,3 с после набора и от двух букв, в рабочем потоке (epg_search по выгрузкам источников). Разделы:
 * «Сейчас в эфире», «Скоро» (неделя вперёд), «В архиве» (в пределах архива канала); по 30 находок в разделе.
 * Новый запрос отменяет идущий поиск. Только главный поток (кроме работы задачи). */
#ifndef PTV_SEARCH_H
#define PTV_SEARCH_H
#include <stdint.h>
#include "data/repo.h"

#define SEARCH_PER_SECTION 30
enum { SR_NOW = 0, SR_SOON, SR_ARCHIVE, SR_SECTIONS };
typedef struct { channel *c; int64_t start, stop; char title[200]; } search_hit;

void search_set(const char *query);       /* текст поля */
void search_tick(double now);
void search_clear(void);                  /* закрыли поиск / сменился плейлист */
int search_busy(void);                    /* идёт поиск передач (или ждёт паузы в наборе) */
int search_rev(void);                     /* растёт при новых результатах */
int search_channels(channel ***out);      /* найденные каналы */
int search_hits(int section, search_hit **out);
/* подсказки — названия из найденных передач, без повторов */
int search_suggest(char out[][200], int max);
/* недавние запросы (до 8, новые первыми) */
int search_recent(char out[][200], int max);
void search_remember(const char *query);
void search_forget(const char *query);
#endif
