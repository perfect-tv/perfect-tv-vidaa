/* Экран ТВ: видео, инфопанель, карусель, набор номера, тосты и стек слоёв поверх (гид, окна, меню).
 * Без слоёв клавиши получает плеер (docs/controls.md, «Плеер»). */
#ifndef PTV_TV_H
#define PTV_TV_H
#include <stdint.h>
#include "gfx.h"
#include "data/repo.h"

typedef struct {
    channel *cur;
    char group[160];                 /* ключ категории, в которой листаем */
    /* архив */
    int arc;                         /* 0 — эфир */
    int64_t arc_url_start;           /* время кадра в позиции 0 ссылки */
    int64_t arc_target;              /* куда просили (пока поток не пошёл) */
    int arc_range;                   /* ссылка — окно с началом и длительностью (можно перематывать внутри) */
    int64_t arc_url_end;
    /* перемотка */
    int seeking;
    int64_t seek_to;
    double seek_apply_at, seek_started;
    /* пауза */
    int paused;
    int64_t pause_at;
    /* инфопанель */
    double info_until;
    int info_pinned, info_expanded;
    double carousel_until;
    /* набор номера */
    char dig[8];
    double dig_at;
    /* воспроизведение */
    double open_at;                  /* открыть поток в этот момент (задержка переключения) */
    int attempts;
    double retry_at, playing_since, opening_since;
    int failed;
    char why[96];
    int scored;
    double back_at;
    double sleep_until;
    int sleep_warned;
    /* PIN */
    int pin_passed;
    char last_unlocked[160];
} tv_t;

extern tv_t T;

void tv_start(void);
int tv_key(int key, int ev);
int tv_has_long(int key);
void tv_draw(gfx_t *g);
void tv_tick(double now);

/* Включить канал (эфир): с проверкой PIN; group — категория для листания (NULL — родная). */
void tv_play(channel *c, const char *group);
/* Архив канала с момента t (unix). */
void tv_play_archive(channel *c, int64_t t, const char *group);
void tv_go_live(void);
int64_t tv_position(void);           /* время кадра (эфир — сейчас) */
int64_t tv_behind(void);             /* отставание от эфира, с */
void tv_toast(const char *msg);
void tv_show_info(int pin);
void tv_set_sleep(int minutes);      /* 0 — выкл. */
int tv_sleep_minutes(void);
/* После загрузки плейлиста / смены плейлиста. */
void tv_playlist_changed(int keep_channel);
/* Шаг по категории: +1 / -1, с пропуском закрытых при отмене PIN. */
channel *tv_neighbour(int dir);
/* окна */
void tv_open_guide(void);
void tv_open_schedule(channel *c, int reminders);
void tv_open_menu(channel *c, const char *group, int from_guide);
void tv_open_recent(void);
void tv_open_settings(void);
#endif
