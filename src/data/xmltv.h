/* Потоковый разборщик XMLTV на уровне байтов (docs/TZ.md, §5). Куски любой длины подаются в xmltv_push():
 * элемент на границе кусков доклеивается. У <programme> сначала читается открывающий тег; канал сравнивается
 * побайтно с каналом предыдущей передачи (XMLTV идёт по каналам подряд), ненужная передача пропускается
 * поиском «</programme>» без разбора. Кодировка — из пролога (UTF-8, windows-1251, koi8-r, iso-8859-1). */
#ifndef PTV_XMLTV_H
#define PTV_XMLTV_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *ctx;
    void (*on_channel)(void *ctx, const char *id, const char *const *names, int n, const char *icon);
    void (*before_programmes)(void *ctx);                   /* перед первой передачей и после <channel> среди передач */
    int (*wanted)(void *ctx, const char *id);               /* слот канала (>= 0) или -1 — пропустить */
    /* передача; stop всегда > start (нет stop — до следующей передачи канала, последняя — час) */
    void (*on_programme)(void *ctx, int slot, int64_t start, int64_t stop, const char *title, const char *sub,
                         const char *desc, const char *cat, const char *icon);
    /* до первой передачи: фильтр по времени до разбора содержимого; 0 — пропустить */
    int (*accept)(void *ctx, int slot, int64_t start, int64_t stop);
    const char *langs[4];                                   /* предпочитаемые языки по порядку */
    int nlangs;
} xmltv_cb;

typedef struct xmltv xmltv;
xmltv *xmltv_new(const xmltv_cb *cb);
void xmltv_push(xmltv *x, const char *data, size_t n);
void xmltv_end(xmltv *x);
long xmltv_channels(const xmltv *x);
long xmltv_programmes(const xmltv *x);
long xmltv_skipped(const xmltv *x);
void xmltv_free(xmltv *x);

/* «yyyyMMddHHmmss ±hhmm»; секунд, минут и смещения может не быть (тогда UTC). 0 — не разобрано. */
int64_t xmltv_time(const char *s, size_t n);
/* Текст элемента: CDATA как есть, вложенные теги убраны, сущности раскрыты. Результат в out (UTF-8). */
void xmltv_text(const char *s, size_t n, char *out, size_t cap);
#endif
