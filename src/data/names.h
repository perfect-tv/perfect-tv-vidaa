/* Нормализация названий каналов (docs/TZ.md, §6): ключ канала для пользовательских данных,
 * ключ сопоставления с EPG (короткое имя + транслит похожих букв) и монограмма для плашки без логотипа. */
#ifndef PTV_NAMES_H
#define PTV_NAMES_H
#include <stddef.h>
#include <stdint.h>

/* Ключ: нижний регистр без пробелов и знаков ASCII. На нём держатся избранное, история, скрытое, блокировки. */
void names_key(const char *name, char *out, size_t cap);
/* «Короткое имя»: без «(архив)», «телеканал», «канал»/«тв» в конце, «orig»/«50»; «(+2)» -> «+2»; знаки -> пробел; «канал 5» -> «канал5». */
void names_short(const char *name, char *out, size_t cap);
/* Короткое имя с транслитом похожих кириллических букв в латиницу (НТВ == HTB). */
void names_match_key(const char *name, char *out, size_t cap);
/* Монограмма: «НТВ», «Р24», «МТ». */
void names_monogram(const char *name, char *out, size_t cap);
/* Приглушённый цвет плашки монограммы — от ключа канала (всегда один и тот же). */
uint32_t names_color(const char *key);
/* Убрать служебные значки ⓢⓖⓥⓞ и лишние пробелы (на месте). */
void names_clean(char *s);
#endif
