/* Хранилище программы (docs/TZ.md, §3, §5): на каждый источник два файла в data_dir —
 *   epg_<хэш>.idx  заголовок, каналы XMLTV (id, имена, иконка, диапазон передач) и передачи (по 28 байт, по каналу и времени);
 *   epg_<хэш>.str  строки (названия, описания), передачи ссылаются на смещения.
 * Файлы отображаются в память (mmap): программа на сотни тысяч передач не занимает кучу, ядро подгружает нужные страницы.
 * Строит их epg_build_* в рабочем потоке; старое прошлое канала (архив) переносится в новую выгрузку. */
#ifndef PTV_EPG_H
#define PTV_EPG_H
#include <stddef.h>
#include <stdint.h>
#include "core/util.h"

typedef struct { uint32_t start, stop, title, sub, desc, cat, icon; } epg_prog;
typedef struct { uint32_t id, names, icon, first, count; } epg_chan;

typedef struct {
    char url[2048];
    uint32_t hash;
    /* отображение */
    void *idx; size_t idx_len;
    const char *str; size_t str_len;
    uint32_t nch, nprog;
    const epg_chan *ch;
    const epg_prog *pr;
    smap ids;                 /* xml id -> канал */
    smap names;               /* ключ сопоставления display-name -> канал */
    /* состояние загрузки */
    int64_t loaded_at;
    char etag[160], last_modified[64], error[96];
    long programmes;          /* передач в файле */
    int loading;              /* 1 — идёт загрузка */
    long long got, total;     /* байты */
    long parsed;              /* передач разобрано (прогресс) */
} epg_src;

/* Открыть файлы источника (если есть) и прочитать метаданные. */
void epg_open(epg_src *s, const char *url);
void epg_close(epg_src *s);
const char *epg_s(const epg_src *s, uint32_t off);
int epg_find_id(const epg_src *s, const char *xml_id);      /* -1 — нет */
int epg_find_name(const epg_src *s, const char *match_key); /* -1 — нет */
void epg_paths(uint32_t hash, char *idx, char *str, char *meta, size_t cap);
void epg_save_meta(const epg_src *s);
void epg_remove_files(uint32_t hash);

/* ---- построение (рабочий поток) ---- */
typedef struct {
    const char *tvg_id, *tvg_name_key, *name_key;   /* ключи — names_match_key */
    int archive_days;
} epg_want;   /* канал плейлиста для фильтра передач */

typedef struct epg_build epg_build;
epg_build *epg_build_new(uint32_t hash, const epg_want *w, int nw, int64_t now, const char *lang);
/* Данные XMLTV (уже распакованные): разбор и запись */
void epg_build_push(epg_build *b, const char *data, size_t n);
/* 0 — файлы готовы (переименованы на место); -1 — ошибка (старые файлы не тронуты). */
int epg_build_finish(epg_build *b, char *err, size_t errn);
long epg_build_programmes(const epg_build *b);
long epg_build_matched(const epg_build *b);
void epg_build_free(epg_build *b);
#endif
