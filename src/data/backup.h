/* Резервная копия (как у Android-версии): один JSON `backup-ГГГГММДДЧЧММСС.perfecttv` — настройки и пользовательские
 * данные (всё хранилище settings.json: избранное, недавние, скрытое, блокировки, очки, PIN, недавние запросы),
 * плейлисты (без каналов), напоминания. Пишется на флешку (папка PerfectTV в корне первого накопителя) и в каталог
 * данных (backups/, последние 5). Восстановление заменяет всё перечисленное; каналы скачиваются заново. */
#ifndef PTV_BACKUP_H
#define PTV_BACKUP_H
#include <stddef.h>
#include <stdint.h>

typedef struct { char path[600]; int64_t created; long long size; int usb; } backup_item;

/* Сохранить; where — куда (путь на флешке или в каталоге данных). 0 — готово. */
int backup_save(char *where, size_t cap, char *err, size_t errn);
/* Копии на флешках и в каталоге данных, новые первыми. */
int backup_list(backup_item *out, int max);
/* Проверить файл: 0 — копия Perfect TV (created — её дата). */
int backup_check(const char *path, int64_t *created);
/* Записать данные копии на место текущих (хранилище, playlists.json, reminders.json; файлы каналов удаляются).
 * Вызывающий затем перезагружает состояние (tv_restart). 0 — готово. */
int backup_restore(const char *path, char *err, size_t errn);
#endif
