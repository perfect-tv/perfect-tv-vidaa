/* Данные приложения в памяти (только главный поток): плейлисты, каналы текущего плейлиста, категории,
 * избранное, история, скрытое, блокировки, очки сортировки, напоминания и связь каналов с программой.
 * Пользовательские данные привязаны к ключу канала names_key(name), а не к номеру (переживают перезагрузку). */
#ifndef PTV_REPO_H
#define PTV_REPO_H
#include <stdint.h>
#include "core/util.h"
#include "data/epg.h"

#define MAX_EPG 8
#define HISTORY_SIZE 11
/* «Избранное» и «Все каналы» — ключи, которые не совпадут с категорией плейлиста */
#define FAV_KEY "\x01" "fav"
#define ALL_KEY "\x01" "all"

enum { EPG_PLAYLIST = 0, EPG_BOTH = 1, EPG_MINE = 2 };

typedef struct {
    int id;
    char name[160];
    char url[2048];
    int epg_mode;
    char *user_epg;      /* через \n */
    char *header_epg;    /* из #EXTM3U, через \n */
    int64_t loaded_at;
    int count;
    char error[120];
    char etag[160], last_modified[64];
} playlist;

typedef struct channel {
    int idx, num;
    char *key, *name, *url, *group, *logo, *tvg_id, *tvg_name;
    char *mkey, *tkey;          /* ключи сопоставления имени и tvg-name */
    double tvg_shift;
    char *cu_type;              /* NULL — архива нет */
    char *cu_source;
    int archive_days;
    char *ua, *ref, *headers_json;
    int epg_src, epg_ch;        /* -1 — программы нет */
    char mono[16];
    uint32_t color;
} channel;

enum { GK_PLAIN = 0, GK_FAV = 1, GK_ALL = 2 };
typedef struct { char *key, *title; channel **ch; int n, cap, kind; } group;

typedef struct {
    int64_t start, stop;
    const char *title, *sub, *desc, *cat, *icon;
} prog;

typedef struct {
    char key[160], name[200], logo[600];
    int num, playlist;
    int64_t start, stop;
    char title[200];
    int64_t snooze;              /* показать не раньше */
    int shown;                   /* окно уже показывали (пропущенное — один раз) */
} reminder;

typedef struct {
    playlist *pl; int npl, cur;          /* cur — индекс в pl, -1 — нет */
    channel *ch; int nch; strpool pool;
    group *groups; int ngroups;          /* видимые: избранное (если не пусто), все каналы, категории */
    group *raw; int nraw;                /* все категории плейлиста */
    strlist fav, history, locked_ch, locked_gr;
    smap hidden_ch, hidden_gr, scores;
    epg_src epg[MAX_EPG]; int nepg;
    reminder *rem; int nrem;
    int rev;                             /* растёт при любой смене каналов/категорий (гид перечитывает) */
} repo;

extern repo R;

void repo_init(void);
void repo_reload(void);                  /* всё заново из файлов (после восстановления копии) */
int repo_select(int pl_index);           /* загрузить каналы плейлиста из файла; 0 — ок */
void repo_save_playlists(void);
playlist *repo_cur(void);
playlist *repo_add_playlist(const char *name, const char *url);
void repo_delete_playlist(int index);
void repo_move_playlist(int index, int dir);
void repo_epg_urls(const playlist *p, strlist *out);   /* по приоритету и режиму */
void repo_channels_path(int pl_id, char *out, size_t cap);

/* категории */
void repo_apply_view(void);
group *repo_group(const char *key);      /* NULL — нет (или скрыта) */
group *repo_group_of(const channel *c, const char *prefer_key);
int repo_group_index(const group *g);
int repo_index_in(const group *g, const channel *c);   /* -1 — нет */
channel *repo_by_key(const char *key);
channel *repo_by_num(int num);           /* без скрытых */

/* избранное, история, скрытое, блокировки, очки */
int repo_is_fav(const channel *c);
void repo_toggle_fav(const channel *c);
void repo_move_fav(const channel *c, int dir);
void repo_add_history(const channel *c);
int repo_recent(const channel *cur, channel **out, int max);
int repo_is_hidden(const channel *c);
void repo_hide_channel(const channel *c);
void repo_hide_group(const char *key);
void repo_unhide(int is_group, const char *key);
int repo_hidden_count(void);
int repo_is_locked(const channel *c);    /* сам или категория; действует, только если PIN задан */
int repo_channel_locked(const channel *c);
int repo_group_locked(const char *key);
void repo_set_locked(int is_group, const char *key, int on);
void repo_add_score(const channel *c);
void repo_reset_scores(void);

/* программа */
void repo_map_epg(void);                 /* сопоставить каналы с открытыми источниками */
void repo_open_epg(void);                /* открыть источники текущего плейлиста */
void repo_reopen_epg(int i);             /* после загрузки источника */
int repo_prog_count(const channel *c);
prog repo_prog(const channel *c, int i);
int repo_prog_at(const channel *c, int64_t t);        /* индекс передачи, идущей в t; -1 */
int repo_prog_after(const channel *c, int64_t t);     /* первая, начинающаяся не раньше t; -1 */
const char *repo_epg_icon(const channel *c);
const char *repo_logo(const channel *c);              /* по приоритету логотипов; "" — нет */
int repo_epg_loading(void);
void repo_gc_epg(void);                  /* удалить программу источников, не нужных ни одному плейлисту */
int repo_epg_users(const char *url, int *ids, int max);   /* id плейлистов с этим источником */
uint32_t repo_epg_users_sig(const char *url);
/* каналы плейлиста из файла (для сборки программы; можно в рабочем потоке) */
int repo_read_wants(int pl_id, int default_days, strpool *pool, epg_want **w, int *nw, int *cap);

/* воспроизведение */
void repo_headers(const channel *c, char *out, size_t cap);     /* "Name: v\r\n"... */
int repo_has_archive(const channel *c);
/* адрес архива: start — начало, dur — длительность (сек); 0 — готово */
int repo_archive_url(const channel *c, int64_t start, int64_t dur, char *out, size_t cap);

/* напоминания */
int repo_reminder_find(const char *key, int64_t start);
void repo_reminder_add(const channel *c, const prog *p);
void repo_reminder_remove(int i);
void repo_reminders_save(void);
#endif
