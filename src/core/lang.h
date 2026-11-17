/* Строки интерфейса: английский и русский (src/core/strings.def). Язык меняется на ходу:
 * строки не кэшировать в статических полях, брать tr() при каждом рисовании. */
#ifndef PTV_LANG_H
#define PTV_LANG_H
#include <stddef.h>

enum {
#define S(key, en, ru) key,
#include "strings.def"
#undef S
    S_COUNT
};

enum { LANG_AUTO = 0, LANG_EN = 1, LANG_RU = 2 };
void lang_set(int mode);        /* LANG_AUTO — по переменным окружения LANG/LC_ALL, иначе русский */
int lang_is_ru(void);
/* Растёт при каждой смене языка: окна, собравшие строки заранее (настройки, меню), по нему пересобирают их. */
int lang_rev(void);
const char *tr(int id);
/* Счётная форма: формы через «|» (en: one|other; ru: one|few|many), %d — число. Буфер на поток. */
const char *trp(int id, long n);
/* tr() с подстановкой printf-формата. Буфер на поток (по кругу из 4). */
const char *trf(int id, ...);
#endif
