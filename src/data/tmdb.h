/* TMDB: картинка (backdrop), год, рейтинг и описание передачи для раскрытой инфопанели (↓↓) — как у Android-версии.
 * Запрос — очищенное название (без серий, сезонов, возраста), год — из «(2019)» в названии или начала описания EPG.
 * Совпадение — по сходству названий (Левенштейн) с бонусом за год; без года — только единственный результат с почти
 * точным названием (новости «Сегодня» ≠ шоу «Today»). Ключ — v3 (api_key) или v4 (токен, заголовок Bearer); адреса
 * API и картинок заменяемы (прокси). Поиск — в рабочем потоке по одному, кэш — в памяти до выхода. Ключ в журнал не
 * пишется. */
#ifndef PTV_TMDB_H
#define PTV_TMDB_H
#include <stddef.h>

#define TMDB_API_DEFAULT "https://api.themoviedb.org/3"
#define TMDB_IMG_DEFAULT "https://image.tmdb.org"

typedef struct {
    char title[256], original[256], date[16], backdrop[256], poster[256], overview[3000], type[16];
    double vote;
} tmdb_hit;

typedef struct {
    char title[256], year[8], overview[3000], backdrop[600], poster[600];
    double vote;
    int unique;
} tmdb_result;

enum { TMDB_OFF = 0, TMDB_LOADING, TMDB_FOUND, TMDB_NONE };

/* ---- подбор (чистые функции, тесты) ---- */
void tmdb_query(const char *title, char *out, size_t cap);    /* очищенное название, в нижнем регистре */
int tmdb_year(const char *title, const char *desc);           /* 0 — не найден */
double tmdb_similarity(const char *a, const char *b);         /* 0..1 */
int tmdb_pick(const tmdb_hit *h, int n, const char *query, int year);   /* индекс или -1 */
/* Ответ /search/multi -> кандидаты (без person); *out — в куче. */
int tmdb_parse(const char *json, size_t len, tmdb_hit **out);

/* ---- главный поток ---- */
int tmdb_enabled(void);
/* Результат для передачи: state — TMDB_*; NULL, пока не найдено. Запускает поиск, когда нужно (app_dirty по готовности). */
const tmdb_result *tmdb_get(const char *title, const char *desc, int *state);
/* Проверка ключа: done(NULL) — работает, иначе текст ошибки (в главном потоке). */
void tmdb_test(void (*done)(const char *err));
void tmdb_cache_clear(void);
#endif
