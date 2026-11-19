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
    /* телетекст и субтитры: поток через ретранслятор (core/relay.c) */
    int tap, subs;                   /* tap — поток через ретранслятор по просьбе (телетекст, субтитры); subs — субтитры вкл. */
    int audio_pref_done;             /* предпочитаемая звуковая дорожка уже применена к каналу */
    char sub_key[48];                /* выбранные субтитры: «ttx», «dvb:rus:0», «vtt:eng» (subs.c) */
    int relayed;                     /* текущий поток идёт через ретранслятор */
    int direct;                      /* у канала — прямое подключение (через ретранслятор не открылся) */
    int direct_pending;              /* через ретранслятор не открылся — запомнить, если заиграет напрямую */
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
/* Предпросмотр плейлиста без остановки текущего канала: гид показывает новый список,
 * видео играет до выбора канала; «Назад» возвращает прежний плейлист. */
int tv_preview_playlist(int index);
void tv_preview_back(void);
int tv_is_preview(void);
void tv_restart(void);               /* после восстановления копии */
void tv_teletext(void);              /* открыть телетекст (поток — через ретранслятор) */
void tv_set_subtitles(int on);       /* вкл. — запомненные или первые доступные субтитры, выкл. */
void tv_subtitles_need_relay(void);  /* выбраны субтитры — поток (если напрямую) через ретранслятор */
int tv_direct_count(void);           /* каналов с прямым подключением */
int64_t tv_frame_pts(void);          /* время кадра в метках потока (90 кГц); -1 — неизвестно */
/* в потоке есть телетекст / субтитры телетекста (поток через ретранслятор и уже разобран) */
int tv_has_teletext(void);
int tv_has_subtitles(void);
void tv_direct_reset(void);
/* Шаг по категории: +1 / -1, с пропуском закрытых при отмене PIN. */
channel *tv_neighbour(int dir);
/* окна */
void tv_open_guide(void);
void tv_open_schedule(channel *c, int reminders);
void tv_open_menu(channel *c, const char *group, int from_guide);
void tv_open_recent(void);
void tv_open_settings(void);
#endif
