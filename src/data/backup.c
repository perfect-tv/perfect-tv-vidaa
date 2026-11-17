#define _GNU_SOURCE
#include "backup.h"
#include "app.h"
#include "json.h"
#include "core/clock.h"
#include "core/usb.h"
#include "core/util.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FORMAT 1
#define KEEP_LOCAL 5
#define EXT ".perfecttv"

/* ---------- запись JSON (в каркасе только разбор) ---------- */
static void jstr(sbuf *b, const char *s) {
    size_t cap = strlen(s) * 6 + 3;
    char *e = malloc(cap);
    if (!e) return;
    json_escape(e, cap, s);
    sb_addc(b, '"');
    sb_add(b, e);
    sb_addc(b, '"');
    free(e);
}
static void jwrite(sbuf *b, const json *j) {
    if (!j) { sb_add(b, "null"); return; }
    switch (j->type) {
    case J_NULL: sb_add(b, "null"); break;
    case J_FALSE: sb_add(b, "false"); break;
    case J_TRUE: sb_add(b, "true"); break;
    case J_NUM:
        if (j->num == (double)(long long)j->num) sb_printf(b, "%lld", (long long)j->num);
        else sb_printf(b, "%.17g", j->num);
        break;
    case J_STR: jstr(b, j->str ? j->str : ""); break;
    case J_ARR:
    case J_OBJ: {
        sb_addc(b, j->type == J_ARR ? '[' : '{');
        int first = 1;
        for (const json *c = j->child; c; c = c->next) {
            if (!first) sb_addc(b, ',');
            first = 0;
            if (j->type == J_OBJ) { jstr(b, c->key ? c->key : ""); sb_addc(b, ':'); }
            jwrite(b, c);
        }
        sb_addc(b, j->type == J_ARR ? ']' : '}');
        break;
    }
    }
}

/* файл JSON из каталога данных как есть (нет — def) */
static void add_file(sbuf *b, const char *key, const char *name, const char *def) {
    size_t n;
    char *t = file_read(data_path(name), &n);
    json_doc *d = t ? json_parse(t, n) : NULL;
    sb_printf(b, ",\n\"%s\":", key);
    if (d) jwrite(b, json_root(d));
    else sb_add(b, def);
    json_free(d);
    free(t);
}

static int write_to(const char *dir, const char *name, const sbuf *b, char *where, size_t cap) {
    mkdir(dir, 0755);
    char p[900];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    if (file_write_atomic(p, b->s, b->n)) return -1;
    if (where) snprintf(where, cap, "%s", p);
    return 0;
}

/* в каталоге данных — только последние KEEP_LOCAL */
static int cmp_name_desc(const void *a, const void *b) { return strcmp(*(char *const *)b, *(char *const *)a); }
static void trim_local(void) {
    char dir[700];
    snprintf(dir, sizeof dir, "%s", data_path("backups"));
    DIR *d = opendir(dir);
    if (!d) return;
    char *names[256];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 256) if (starts_with(e->d_name, "backup-") && ends_with_ci(e->d_name, EXT)) names[n++] = strdup(e->d_name);
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], cmp_name_desc);
    char p[1000];
    for (int i = 0; i < n; i++) {
        if (i >= KEEP_LOCAL && names[i]) { snprintf(p, sizeof p, "%s/%s", dir, names[i]); unlink(p); }
        free(names[i]);
    }
}

int backup_save(char *where, size_t cap, char *err, size_t errn) {
    store_save();   /* хранилище каркаса — на диск, чтобы прочитать его файлом */
    int64_t now = clock_now();
    ltm m = local_tm(now);
    char name[80];
    snprintf(name, sizeof name, "backup-%04d%02d%02d%02d%02d%02d" EXT, m.y, m.mo, m.d, m.h, m.m, (int)(now % 60));
    sbuf b = {0};
    sb_printf(&b, "{\"app\":\"perfecttv\",\"format\":%d,\"platform\":\"vidaa\",\"created\":%lld", FORMAT, (long long)now);
    add_file(&b, "settings", "settings.json", "{}");
    add_file(&b, "playlists", "playlists.json", "[]");
    add_file(&b, "reminders", "reminders.json", "[]");
    sb_add(&b, "}\n");
    int ok_local = !write_to(data_path("backups"), name, &b, where, cap);
    if (ok_local) trim_local();
    /* на флешку: её копия переживёт удаление приложения и переносится на другой ТВ */
    usb_root r[4];
    int nr = usb_roots(r, 4), ok_usb = 0;
    for (int i = 0; i < nr && !ok_usb; i++) {
        char dir[700];
        snprintf(dir, sizeof dir, "%s/PerfectTV", r[i].path);
        ok_usb = !write_to(dir, name, &b, where, cap);
    }
    sb_free(&b);
    fprintf(stderr, "backup_save local=%d usb=%d\n", ok_local, ok_usb);
    if (!ok_local && !ok_usb) { snprintf(err, errn, "%s", strerror(errno)); return -1; }
    return 0;
}

int backup_check(const char *path, int64_t *created) {
    size_t n;
    char *t = file_read(path, &n);
    if (!t) return -1;
    json_doc *d = json_parse(t, n);
    free(t);
    if (!d) return -1;
    json *r = json_root(d);
    int ok = !strcmp(json_str(json_get(r, "app"), ""), "perfecttv") && json_is(json_get(r, "settings"), J_OBJ);
    if (created) *created = json_int(json_get(r, "created"), 0);
    json_free(d);
    return ok ? 0 : -1;
}

static int scan(const char *dir, int usb, backup_item *out, int n, int max) {
    DIR *d = opendir(dir);
    if (!d) return n;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (!ends_with_ci(e->d_name, EXT)) continue;
        backup_item *it = &out[n];
        snprintf(it->path, sizeof it->path, "%s/%s", dir, e->d_name);
        int dup = 0;
        for (int i = 0; i < n; i++) if (!strcmp(out[i].path, it->path)) dup = 1;
        if (dup || backup_check(it->path, &it->created)) continue;
        it->size = file_size(it->path);
        it->usb = usb;
        n++;
    }
    closedir(d);
    return n;
}
static int cmp_created(const void *a, const void *b) {
    const backup_item *x = a, *y = b;
    return x->created < y->created ? 1 : x->created > y->created ? -1 : y->usb - x->usb;
}
int backup_list(backup_item *out, int max) {
    int n = 0;
    usb_root r[4];
    int nr = usb_roots(r, 4);
    char dir[700];
    for (int i = 0; i < nr; i++) {
        snprintf(dir, sizeof dir, "%s/PerfectTV", r[i].path);
        n = scan(dir, 1, out, n, max);
        n = scan(r[i].path, 1, out, n, max);
    }
    snprintf(dir, sizeof dir, "%s", data_path("backups"));
    n = scan(dir, 0, out, n, max);
    qsort(out, (size_t)n, sizeof *out, cmp_created);
    return n;
}

int backup_restore(const char *path, char *err, size_t errn) {
    size_t n;
    char *t = file_read(path, &n);
    json_doc *d = t ? json_parse(t, n) : NULL;
    free(t);
    json *r = d ? json_root(d) : NULL;
    if (!r || strcmp(json_str(json_get(r, "app"), ""), "perfecttv") || !json_is(json_get(r, "settings"), J_OBJ)) {
        snprintf(err, errn, "format");
        json_free(d);
        return -1;
    }
    /* хранилище: сначала убрать все ключи (перечень — из файла), затем ключи копии */
    store_save();
    char *cur = file_read(data_path("settings.json"), &n);
    json_doc *cd = cur ? json_parse(cur, n) : NULL;
    free(cur);
    for (json *c = cd && json_root(cd) ? json_root(cd)->child : NULL; c; c = c->next) if (c->key) store_set(c->key, NULL);
    json_free(cd);
    char buf[64];
    for (json *c = json_get(r, "settings")->child; c; c = c->next) {
        if (!c->key) continue;
        if (c->type == J_STR) store_set(c->key, c->str);
        else if (c->type == J_NUM || c->type == J_TRUE || c->type == J_FALSE) store_set(c->key, json_text(c, buf, sizeof buf));
    }
    store_save();
    /* плейлисты (без времени загрузки и ETag — скачаются заново) и напоминания */
    sbuf b = {0};
    json *pl = json_get(r, "playlists");
    sb_add(&b, "[");
    int first = 1;
    for (json *p = json_is(pl, J_ARR) ? pl->child : NULL; p; p = p->next) {
        if (!json_is(p, J_OBJ)) continue;
        if (!first) sb_add(&b, ",\n");
        first = 0;
        sb_addc(&b, '{');
        int f2 = 1;
        for (json *c = p->child; c; c = c->next) {
            if (!c->key || !strcmp(c->key, "loaded_at") || !strcmp(c->key, "etag") || !strcmp(c->key, "last_modified") || !strcmp(c->key, "error")) continue;
            if (!f2) sb_addc(&b, ',');
            f2 = 0;
            jstr(&b, c->key);
            sb_addc(&b, ':');
            jwrite(&b, c);
        }
        sb_addc(&b, '}');
    }
    sb_add(&b, "]\n");
    int e1 = file_write_atomic(data_path("playlists.json"), b.s, b.n);
    sb_clear(&b);
    json *rem = json_get(r, "reminders");
    if (json_is(rem, J_ARR)) jwrite(&b, rem);
    else sb_add(&b, "[]");
    sb_addc(&b, '\n');
    int e2 = file_write_atomic(data_path("reminders.json"), b.s, b.n);
    sb_free(&b);
    json_free(d);
    /* каналы прежних плейлистов: id могли совпасть с другими плейлистами копии */
    DIR *dd = opendir(data_path("."));
    if (dd) {
        struct dirent *e;
        char p[1000];
        while ((e = readdir(dd))) {
            if (!starts_with(e->d_name, "channels_")) continue;
            snprintf(p, sizeof p, "%s", data_path(e->d_name));
            unlink(p);
        }
        closedir(dd);
    }
    fprintf(stderr, "backup_restore ok=%d\n", !e1 && !e2);
    if (e1 || e2) { snprintf(err, errn, "write"); return -1; }
    return 0;
}
