/* Плеер: системный плеер ТВ (vendor/player-hmc.c, FFmpeg разбирает поток, кадры — в HMC, видео — «дырой»
 * под окном интерфейса), при отказе по кодекам — программный (vendor/sw_player.c: MPEG-2, MP2 и т. п.).
 * Открытие и закрытие — в рабочем потоке (open идёт секунды); одновременно не больше одного задания,
 * быстрые переключения сливаются в одно (последний запрос побеждает). */
#ifndef PTV_PLAYER_H
#define PTV_PLAYER_H
#include "gfx.h"

enum { PL_IDLE = 0, PL_OPENING, PL_PLAYING, PL_FAILED, PL_ENDED };

/* Открыть поток (заменяет текущий); start — позиция в секундах внутри ссылки (VOD), 0 — с начала/края. */
void pl_open(const char *url, const char *headers, double start, int archive);
void pl_close(void);
int pl_state(void);
const char *pl_why(void);            /* код отказа (open_input=…, video_codec=h264…) */
void pl_tick(void);                  /* обслуживание; вызывать каждый кадр */
double pl_position(void);            /* секунды от начала ссылки */
double pl_duration(void);            /* 0 — эфир / неизвестно */
int pl_pause(int on);                /* 0 — получилось */
int pl_paused(void);
int pl_seek(double abs_seconds);
int pl_software(void);
int pl_rebuffering(void);
int pl_video_size(int *w, int *h);   /* 0 — известно */
const char *pl_codecs(void);         /* «h264 · AAC» */
int pl_set_aspect(int aspect);       /* ASPECT_* из prefs.h */
int pl_audio_count(void);
const char *pl_audio_label(int i);
int pl_audio_selected(void);
int pl_audio_select(int i);
int pl_sub_count(void);              /* дорожки субтитров в потоке (системный плеер их не показывает) */
const char *pl_sub_label(int i);     /* язык или название */
const char *pl_sub_codec(int i);     /* dvb_teletext, dvb_subtitle, webvtt… */
/* Кадр программного плеера в холст (под интерфейсом); для системного — прозрачная «дыра». */
void pl_draw_video(gfx_t *g);
int pl_has_picture(void);            /* видео уже идёт (первый кадр) */
void pl_shutdown(void);              /* закрыть перед выходом: системный плеер держит экран */
#endif
