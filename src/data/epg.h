/* Хранилище программы (docs/TZ.md, §3, §5). Программа в 1–3 ГБ распакованного XMLTV не помещается ни в память ТВ,
 * ни в адресное пространство 32-битного процесса, поэтому в память ничего не отображается и целиком не читается.
 *
 * Источник — папка data_dir/epg/<хэш адреса>/, в ней одна «выгрузка» (номер gen в meta.json) из трёх файлов:
 *   c<gen>.tsv  каналы XMLTV (id, иконка, имена) — все, для сопоставления; читается построчно;
 *   d<gen>.bin  куски «канал + сутки» (UTC по началу передачи), каждый сжат zlib: записи по 28 байт и строки;
 *   i<gen>.bin  оглавление: заголовок, каталог корзин (до 256, по хэшу id канала), в корзине — каналы и их куски
 *               (смещение, длины, число передач, начало первой и конец последней).
 * Хранятся только передачи сопоставленных каналов (всех плейлистов с этим адресом) в окне «архив + 1 сутки» … +8 суток.
 *
 * Чтение (главный поток): корзина оглавления разбирается при первом обращении к её каналу, кусок читается pread и
 * распаковывается в общий кэш. Строки передачи живут в кэше: кусок, к которому обращались в этом или прошлом кадре
 * (epg_frame), не вытесняется; держать строку дольше — копировать.
 * Сборка (рабочий поток): передачи канала копятся в памяти до смены канала (XMLTV идёт по каналам подряд), затем
 * режутся на сутки и дописываются в d<gen>.new; прошлое прежней выгрузки переносится тут же. В памяти — один канал
 * и оглавление. В конце — оглавление и переименование; прежнюю выгрузку удаляет epg_prune. */
#ifndef PTV_EPG_H
#define PTV_EPG_H
#include <stddef.h>
#include <stdint.h>
#include "core/util.h"

typedef struct epg_store epg_store;
typedef struct { char *id, *icon; void *ch; int resolved; } epg_slot;

typedef struct {
    char url[2048];
    uint32_t hash;
    uint32_t gen;             /* 0 — выгрузки нет */
    epg_store *st;            /* открытая выгрузка */
    epg_slot *slots;          /* каналы XMLTV, сопоставленные с текущим плейлистом */
    int nslots, cap_slots;
    /* метаданные (meta.json) */
    int64_t loaded_at;
    char etag[160], last_modified[64], error[96];
    uint32_t want_hash;       /* каналы плейлистов, для которых собрана выгрузка */
    uint32_t users_sig;       /* плейлисты источника при последней проверке (repo_epg_users_sig) */
    long programmes;          /* передач в выгрузке */
    /* состояние загрузки */
    int loading;              /* 1 — идёт загрузка */
} epg_src;

typedef struct {
    int64_t start, stop;
    const char *title, *sub, *desc, *cat, *icon;
} epg_item;

/* Открыть выгрузку источника (если есть) и прочитать метаданные. */
void epg_open(epg_src *s, const char *url);
void epg_close(epg_src *s);
int epg_has_data(const epg_src *s);
void epg_save_meta(const epg_src *s);
void epg_dir(uint32_t hash, char *out, size_t cap);
void epg_remove_files(uint32_t hash);             /* вся папка источника */
void epg_prune(uint32_t hash, uint32_t keep_gen);  /* файлы других выгрузок и недописанные */
/* Удалить папки источников, которых нет в keep, и файлы прежнего формата. */
void epg_gc(const uint32_t *keep, int n);

/* Каналы XMLTV выгрузки по строке: names — display-name по порядку. 0 — прочитано. */
typedef void (*epg_chan_cb)(void *ctx, const char *id, const char *icon, const char *const *names, int n);
int epg_scan(const epg_src *s, epg_chan_cb cb, void *ctx);

/* Слоты — каналы XMLTV, которые нужны текущему плейлисту (заполняет repo_map_epg). */
void epg_slots_clear(epg_src *s);
int epg_slot_add(epg_src *s, const char *id, const char *icon);
const char *epg_slot_icon(const epg_src *s, int slot);
int epg_count(epg_src *s, int slot);
int epg_get(epg_src *s, int slot, int i, epg_item *out);   /* 0 — есть; строки — до вытеснения куска */
int epg_lower(epg_src *s, int slot, int64_t t);            /* первая передача с stop > t; = count — нет */
/* Новый кадр интерфейса: куски, не нужные два кадра, можно вытеснять. */
void epg_frame(void);
/* Освободить кэш кусков (выход, тесты). */
void epg_cache_free(void);
long epg_cache_bytes(void);

/* ---- построение (рабочий поток) ---- */
typedef struct {
    const char *tvg_id, *tvg_name_key, *name_key;   /* ключи — names_match_key */
    int archive_days;
} epg_want;   /* канал плейлиста для фильтра передач */

typedef struct epg_build epg_build;
/* old_gen — выгрузка, из которой переносится прошлое (0 — нет); пишется new_gen. */
epg_build *epg_build_new(uint32_t hash, uint32_t old_gen, uint32_t new_gen, const epg_want *w, int nw, int64_t now,
                         const char *lang);
/* Данные XMLTV (уже распакованные): разбор и запись */
void epg_build_push(epg_build *b, const char *data, size_t n);
/* 0 — файлы готовы (переименованы на место); -1 — ошибка (прежняя выгрузка не тронута). */
int epg_build_finish(epg_build *b, char *err, size_t errn);
long epg_build_programmes(const epg_build *b);
long epg_build_matched(const epg_build *b);
long epg_build_stored(const epg_build *b);       /* передач в выгрузке (после finish) */
void epg_build_free(epg_build *b);              /* без finish — недописанные файлы удаляются */

/* Хэш списка нужных каналов: изменился — условный запрос не годится (304 оставил бы новые каналы без программы). */
uint32_t epg_want_hash(const epg_want *w, int nw);
#endif
