/* Настройки и «где остановились»: поля P, хранятся в data_dir/settings.json (store_* каркаса). */
#ifndef PTV_PREFS_H
#define PTV_PREFS_H

#define TZ_AUTO (-99)
enum { NUM_NONE = 0, NUM_NEAR_NAME = 1, NUM_BADGE = 2 };
enum { GUIDE_GRID = 0, GUIDE_LIST = 1 };
enum { UP_SCHEDULE = 0, UP_RECENT = 1 };
enum { PIN_EACH = 0, PIN_ONCE = 1 };
enum { PIN_IN_RING = 0, PIN_IN_ROWS = 1, PIN_IN_KEYS = 2 };
enum { ASPECT_FIT = 0, ASPECT_STRETCH = 1, ASPECT_16_9 = 2, ASPECT_4_3 = 3 };

typedef struct {
    int lang;           /* LANG_AUTO/EN/RU */
    int tz;             /* TZ_AUTO или смещение в четвертях часа */
    int ui_scale;       /* 0 нормальный (480), 1 меньше (540), 2 больше (432) — высота экрана в «dp» */
    int archive_days;   /* глубина архива по умолчанию */
    int info_hide;      /* секунды */
    int number_mode;
    int guide_mode;
    int guide_header;   /* 0 канал + передача, 1 канал, 2 скрыть */
    int up_key;
    int hints;
    int adaptive;
    int start_last;
    int epg_hours;      /* обновлять программу каждые N часов */
    int playlist_hours; /* плейлист устаревает через N часов */
    int any_cert;       /* принимать любые сертификаты */
    int logo_priority;  /* 0 плейлист, 1 программа */
    int aspect;
    int remind_before;  /* минут */
    int pin_ask;
    int pin_input;
    char pin[8];
    /* где остановились */
    int last_playlist;
    char last_channel[160];
    char last_group[160];
} prefs;

extern prefs P;
void prefs_load(void);
void prefs_save(void);
/* Высота экрана в «dp» по настройке «Размер интерфейса» (1 em = 10 dp). */
double prefs_screen_dp(void);
#endif
