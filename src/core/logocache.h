/* Логотипы каналов: память и диск (docs/TZ.md, §9). img_get каркаса кэширует только в памяти и ходит в сеть
 * после каждого запуска; здесь те же картинки лежат в data_dir/logos и декодируются из файла, пока сеть недоступна
 * или медленная. Ключ памяти — адрес и размер, файла — только адрес (одна и та же картинка на разные размеры). */
#ifndef PTV_LOGOCACHE_H
#define PTV_LOGOCACHE_H
#include "gfx.h"

/* Готовый логотип (NULL — ещё грузится или не загрузился; app_dirty() позовёт ldone). */
const gfx_image *logo_img(const char *url, int w, int h, int mode);
void logo_trim(void);    /* раз в несколько секунд: освободить память сверх бюджета */
/* Путь файла кэша адреса; 0 — каталог данных недоступен (кэш только в памяти). */
int logo_cache_path(const char *url, char *out, size_t cap);
void logo_clear(void);   /* «Очистить кэш логотипов»: память и файлы */
#endif
