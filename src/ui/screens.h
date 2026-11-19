/* Окна и слои экрана ТВ (каждое — свой файл в src/ui). */
#ifndef PTV_SCREENS_H
#define PTV_SCREENS_H
#include "gfx.h"
#include "data/repo.h"

/* info.c — инфопанель */
void info_draw(gfx_t *g);
int info_top(void);                       /* верх инфопанели (для карусели) */

/* guide.c — ТВ-гид */
void guide_open(int to_programmes);
int guide_is_open(void);

/* schedule.c — «Программа и архив», напоминания */
void schedule_open(channel *c, int reminders);

/* menu.c — меню канала */
void menu_open(channel *c, const char *group, int from_guide);

/* guide.c */
const char *guide_mode_name(void);                 /* название вида гида для настроек и меню */

/* popups.c */
void card_open(channel *c, const prog *p);          /* карточка передачи */
void recent_open(void);                             /* недавние каналы */
void help_open(void);                               /* «Управление» */
void hidden_open(void);                             /* скрытое */
void welcome_open(void);                            /* мастер: нет плейлистов */
void reminders_check(void);                         /* раз в несколько секунд: окно напоминания */
typedef void (*confirm_fn)(void *ctx);
void confirm_open(const char *text, const char *yes, confirm_fn yes_fn, void *ctx);
/* выбор из списка: OK — fn(index) */
typedef void (*choose_fn)(int index, void *ctx);
void choose_open(const char *title, const char *const *items, int n, int selected, choose_fn fn, void *ctx);
/* Ссылка QR-кодом (Настройки → О программе) и текст с переносом (лицензии) */
void qr_open(const char *title, const char *url);
void text_open(const char *title, const char *text);

/* pin.c */
typedef void (*pin_fn)(void *ctx);
void pin_ask(pin_fn ok, pin_fn cancel, void *ctx);
void pin_create(pin_fn done, void *ctx);

/* settings.c */
void settings_open(void);

/* form.c — формы с экранной клавиатурой */
void form_playlist(int index);                      /* -1 — новый */
typedef void (*text_fn)(const char *text, void *ctx);
void form_text(const char *title, const char *initial, int multiline, text_fn done, void *ctx);
void form_playlist_phone(void);                     /* новый плейлист и сразу окно ввода с телефона */

/* files.c — файл с флешки; epg — файл программы XMLTV (по расширению) */
typedef void (*file_fn)(const char *path, int epg, void *ctx);
void files_open(file_fn done, void *ctx);

/* subs.c — субтитры (телетекст, DVB, WebVTT) */
enum { ST_TTX = 1, ST_DVB, ST_VTT, ST_CC, ST_OTHER };
typedef struct { int kind, idx, supported; char key[48], label[160]; } subtrack;   /* idx — в списке ретранслятора, -1 — из плеера */
int subs_tracks(subtrack *out, int max);
int subs_available(void);
void subs_load(void);                 /* выбор канала из хранилища */
void subs_select(const char *key);    /* NULL — выкл. */
const char *subs_current_label(void);
void subs_apply(void);                /* каждый кадр */
void subs_stream_changed(void);
void subs_draw(gfx_t *g, int bottom);
void subs_menu_open(void);
void subs_toggle(void);

/* teletext.c — телетекст */
void teletext_open(void);

/* search.c — поиск */
void search_open(void);
void search_open_query(const char *q);              /* с готовым запросом */

/* phone.c — ввод с телефона по QR */
#include "core/phone.h"
typedef void (*phone_fn)(const phone_result *r, void *ctx);
void phone_open(phone_fn done, void *ctx);          /* плейлист */
void phone_open_kind(int kind, phone_fn done, void *ctx);
#endif
