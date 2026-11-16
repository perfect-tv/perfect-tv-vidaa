/* Время логики — unix-секунды с поправкой по серверу (часы дешёвых ТВ уплывают): clock_now(),
 * а не time(NULL). Часовой пояс — системный или из настройки «Часовой пояс». */
#ifndef PTV_CLOCK_H
#define PTV_CLOCK_H
#include <stdint.h>

int64_t clock_now(void);
/* Время сервера (заголовок Date): поправка, если расхождение больше 30 с. */
void clock_server(int64_t server_unix);
/* Смещение местного времени от UTC в секундах для момента t. */
int clock_tz(int64_t t);
/* Для тестов: зафиксировать «сейчас» (0 — снять). */
void clock_fake(int64_t t);

/* Начало суток (местное время), в которые попадает t. */
int64_t day_start(int64_t t);
/* Разбор местного времени: часы, минуты, день недели (0 — пн), число, месяц (1..12), год. */
typedef struct { int h, m, s, wd, d, mo, y; } ltm;
ltm local_tm(int64_t t);

/* Форматы: «21:40»; «Сегодня» / «Вчера» / «Завтра» / «Пн, 02.11»; «пн, 2 нояб.»; «−0:45» */
void fmt_hm(int64_t t, char out[8]);
void fmt_day(int64_t t, char *out, int cap);
void fmt_day_short(int64_t t, char *wd, int wcap, char *dm, int dcap);   /* «пн» и «2 нояб.» */
void fmt_dur(int64_t secs, char *out, int cap);                         /* 1:05:03 или 5:03 */
#endif
