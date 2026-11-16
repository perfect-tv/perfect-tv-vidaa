/* Мелочи общего назначения: строковый буфер, UTF-8, файлы, хэш-таблица строк. */
#ifndef PTV_UTIL_H
#define PTV_UTIL_H
#include <stddef.h>
#include <stdint.h>

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

/* Растущий буфер; s всегда оканчивается нулём (если s != NULL). */
typedef struct { char *s; size_t n, cap; } sbuf;
int sb_addn(sbuf *b, const char *s, size_t n);
int sb_add(sbuf *b, const char *s);
int sb_addc(sbuf *b, char c);
int sb_printf(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_clear(sbuf *b);
void sb_free(sbuf *b);
/* Забрать строку (буфер обнуляется); пустой — "" в куче. */
char *sb_take(sbuf *b);

/* Пул строк: много маленьких строк одним куском, освобождаются разом. */
typedef struct pool_blk pool_blk;
typedef struct { pool_blk *head; } strpool;
char *sp_dup(strpool *p, const char *s);
char *sp_dupn(strpool *p, const char *s, size_t n);
void sp_free(strpool *p);

/* UTF-8 */
uint32_t u8_next(const char **s);           /* следующий код (0 в конце строки) */
int u8_put(char *out, uint32_t cp);          /* 1..4 байта */
uint32_t u8_lower(uint32_t cp);              /* латиница, кириллица, греческий */
int u8_is_letter(uint32_t cp);               /* буква или цифра (латиница, кириллица, греческий, расширенная латиница) */
void u8_lower_str(char *dst, size_t cap, const char *src);
/* Обрезать по границе символа, не длиннее cap-1 байт */
void u8_copy(char *dst, size_t cap, const char *src);

char *xstrndup(const char *s, size_t n);
void trim(char *s);                          /* пробелы по краям, на месте */
int starts_with(const char *s, const char *prefix);
int starts_with_ci(const char *s, const char *prefix);
int ends_with_ci(const char *s, const char *suffix);
const char *strcasestr_ascii(const char *hay, const char *needle);

/* Файлы */
char *file_read(const char *path, size_t *len);           /* NULL при ошибке; с нулём на конце */
int file_write_atomic(const char *path, const void *data, size_t len);
int file_exists(const char *path);
long long file_size(const char *path);
const char *data_path(const char *name);                 /* data_dir/name (статический буфер на поток) */

/* Хэш */
uint32_t hash_str(const char *s);
uint32_t hash_bytes(const void *p, size_t n);

/* Таблица строка -> int (открытая адресация); ключи копируются. */
typedef struct { char **keys; int *vals; int cap, n; } smap;
void smap_init(smap *m, int expect);
void smap_free(smap *m);
int smap_get(const smap *m, const char *key, int def);
void smap_set(smap *m, const char *key, int val);

/* Список строк (через \n при сохранении). */
typedef struct { char **v; int n, cap; } strlist;
void sl_add(strlist *l, const char *s);
void sl_insert(strlist *l, int at, const char *s);
void sl_remove(strlist *l, int at);
int sl_find(const strlist *l, const char *s);
void sl_clear(strlist *l);
void sl_from_lines(strlist *l, const char *text);    /* пустые строки пропускаются */
char *sl_join(const strlist *l, const char *sep);     /* в куче */

double mono_now(void);

#endif
