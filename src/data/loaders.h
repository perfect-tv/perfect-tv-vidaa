/* Фоновая загрузка плейлиста и программы (рабочие потоки pool_submit): поток HTTP -> распаковка -> разбор ->
 * файл; по окончании (главный поток) данные подменяются и интерфейс перерисовывается. Одновременно — одна
 * загрузка плейлиста и одна загрузка программы (источники по очереди). */
#ifndef PTV_LOADERS_H
#define PTV_LOADERS_H

/* Плейлист pl_index; force — даже если не устарел. Готово — callback (может быть NULL). */
void load_playlist(int pl_index, int force);
/* Программа текущего плейлиста: все источники по очереди; force — без условного запроса и срока. */
void load_epg(int force);
/* При запуске и раз в минуту: устаревшее — перекачать. */
void loaders_tick(void);
void loaders_cancel(void);
int loaders_playlist_busy(void);
/* Прогресс для строки состояния: 0..100 или -1 (размер неизвестен); каналов разобрано. */
int loaders_playlist_progress(int *channels);
int loaders_epg_progress(long *programmes);
/* Вызывается после загрузки плейлиста (текущего) и после программы: интерфейс подхватывает изменения. */
extern void (*loaders_on_playlist)(int pl_index, int ok, const char *err);
extern void (*loaders_on_epg)(int ok, const char *err);
/* Текст ошибки загрузки для показа (no_url, network, http_<код>, format, empty). */
const char *loaders_error_text(const char *code);
#endif
