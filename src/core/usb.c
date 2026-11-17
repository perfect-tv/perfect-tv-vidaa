#define _GNU_SOURCE
#include "usb.h"
#include "util.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* \040 и т. п. в путях /proc/mounts */
static void unoct(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' && s[2] >= '0' && s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
            *o++ = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + (s[3] - '0'));
            s += 3;
        } else *o++ = *s;
    }
    *o = 0;
}

static int removable_fs(const char *t) {
    static const char *const fs[] = {"vfat", "msdos", "exfat", "ntfs", "ntfs3", "fuseblk", "texfat", "tntfs", "ufsd", "hfsplus", "sdcardfs"};
    for (size_t i = 0; i < sizeof fs / sizeof fs[0]; i++) if (!strcmp(t, fs[i])) return 1;
    return 0;
}
static int media_path(const char *m) {
    if (!starts_with(m, "/mnt/") && !starts_with(m, "/media/") && !starts_with(m, "/run/media/") && !starts_with(m, "/storage/") &&
        !starts_with(m, "/tmp/usb") && !starts_with(m, "/usb"))
        return 0;
    char low[USB_PATH];
    u8_lower_str(low, sizeof low, m);
    return strstr(low, "usb") || strstr(low, "/sd") || strstr(low, "media") || strstr(low, "disk") || strstr(low, "storage");
}
static int system_path(const char *m) {
    return !strcmp(m, "/") || starts_with(m, "/boot") || starts_with(m, "/proc") || starts_with(m, "/sys") || starts_with(m, "/dev") ||
           starts_with(m, "/run/user") || starts_with(m, "/mnt/tvroot") || starts_with(m, "/var/lib") || starts_with(m, "/snap");
}

static int add_root(usb_root *out, int n, int max, const char *path) {
    if (n >= max) return n;
    for (int i = 0; i < n; i++) if (!strcmp(out[i].path, path)) return n;
    struct stat st;
    if (stat(path, &st) || !S_ISDIR(st.st_mode)) return n;
    snprintf(out[n].path, sizeof out[n].path, "%s", path);
    const char *b = strrchr(path, '/');
    snprintf(out[n].name, sizeof out[n].name, "%s", b && b[1] ? b + 1 : path);
    return n + 1;
}

int usb_roots(usb_root *out, int max) {
    int n = 0;
    const char *env = getenv("PTV_USB_ROOT");
    if (env && *env) n = add_root(out, n, max, env);
    FILE *f = fopen("/proc/mounts", "r");
    char line[1024], cand[600] = "";
    while (f && fgets(line, sizeof line, f)) {
        char dev[256], mnt[USB_PATH], type[64];
        if (sscanf(line, "%255s %511s %63s", dev, mnt, type) != 3) continue;
        unoct(mnt);
        if (system_path(mnt)) continue;
        int fsok = removable_fs(type), pathok = media_path(mnt);
        if (pathok && strlen(cand) + strlen(mnt) + strlen(type) + 4 < sizeof cand) {   /* для журнала */
            strcat(cand, " ");
            strcat(cand, mnt);
            strcat(cand, ":");
            strcat(cand, type);
        }
        if (!fsok && !pathok) continue;
        /* внутренние разделы ТВ (ext4/ubifs/squashfs) — только со «usb»/«sd» в пути */
        if (!fsok && (!strcmp(type, "squashfs") || !strcmp(type, "ubifs") || !strcmp(type, "tmpfs") || !strcmp(type, "overlay"))) continue;
        n = add_root(out, n, max, mnt);
    }
    if (f) fclose(f);
    fprintf(stderr, "usb_roots=%d", n);
    for (int i = 0; i < n; i++) fprintf(stderr, " %s", out[i].path);
    if (!n) fprintf(stderr, " candidates=%s", cand[0] ? cand : "-");
    fprintf(stderr, "\n");
    return n;
}

static int ext_ok(const char *n) {
    static const char *const ext[] = {".m3u", ".m3u8", ".txt", ".gz", ".zip", ".xml", ".xmltv"};
    for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++) if (ends_with_ci(n, ext[i])) return 1;
    return 0;
}
int usb_is_epg(const char *n) {
    return ends_with_ci(n, ".xml") || ends_with_ci(n, ".xmltv") || ends_with_ci(n, ".xml.gz") || ends_with_ci(n, ".xml.zip") ||
           ends_with_ci(n, ".xmltv.gz");
}

static int cmp_entry(const void *a, const void *b) {
    const usb_entry *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}

int usb_list(const char *dir, usb_entry **out) {
    *out = NULL;
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0, cap = 0;
    usb_entry *v = NULL;
    struct dirent *e;
    char p[USB_PATH + 260];
    while ((e = readdir(d)) && n < 5000) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        int isdir = S_ISDIR(st.st_mode);
        if (!isdir && (!S_ISREG(st.st_mode) || !ext_ok(e->d_name))) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            usb_entry *nv = realloc(v, (size_t)cap * sizeof *nv);
            if (!nv) break;
            v = nv;
        }
        snprintf(v[n].name, sizeof v[n].name, "%s", e->d_name);
        v[n].dir = isdir;
        v[n].size = isdir ? 0 : (long long)st.st_size;
        n++;
    }
    closedir(d);
    if (n) qsort(v, (size_t)n, sizeof *v, cmp_entry);
    *out = v;
    return n;
}
