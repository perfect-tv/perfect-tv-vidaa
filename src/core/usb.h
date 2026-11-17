/* Съёмные накопители и список файлов для выбора плейлиста / программы с флешки. Точки монтирования VIDAA заранее
 * не известны, поэтому корни берутся из /proc/mounts: файловые системы флешек (vfat, exfat, ntfs, fuseblk…) и любые
 * точки под /mnt, /media, /run/media, /storage со «usb» / «sd» в пути. PTV_USB_ROOT — свой корень (ПК, тесты). */
#ifndef PTV_USB_H
#define PTV_USB_H
#include <stddef.h>

#define USB_PATH 512
typedef struct { char path[USB_PATH]; char name[128]; } usb_root;
/* Корни; журнал — строкой usb_roots= (и кандидаты, если ничего не подошло). */
int usb_roots(usb_root *out, int max);

typedef struct { char name[256]; int dir; long long size; } usb_entry;
/* Папки и файлы плейлистов/программы в dir (без скрытых): папки первыми, по имени без учёта регистра.
 * *out — в куче (free). -1 — не открыть. */
int usb_list(const char *dir, usb_entry **out);
/* Файл программы XMLTV (по расширению), а не плейлист */
int usb_is_epg(const char *name);
#endif
