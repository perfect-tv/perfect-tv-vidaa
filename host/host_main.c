/* Запуск интерфейса на ПК без ТВ: тот же код экранов, заглушка плеера, клавиши — по сценарию, кадры — в PPM.
 *   build/host/perfecttv-host --data DIR --out DIR key:down wait:1 shot:info long:ok shot:menu ...
 *   build/host/perfecttv-host --window [шаги сценария] — окно X11, управление с клавиатуры (host/x11_window.c).
 * Сценарий: key:NAME (нажатие), long:NAME (удержание 1 с), hold:NAME:SEC, wait:SEC, shot:NAME, lang:ru|en.
 * Имена клавиш: left right up down ok back menu info guide chup chdown red green yellow blue play stop rew ff 0..9.
 * Видео под «дырой» холста (alpha 0) — тестовая картинка; так видно, что просвечивает через затемнения. */
#define _GNU_SOURCE
#include "app.h"
#include "net.h"
#include "core/http.h"
#include "core/lang.h"
#include "core/prefs.h"
#include "data/repo.h"
#include "ui/keys.h"
#include "ui/tv.h"
#include "ui/u.h"
#include "ui/screens.h"
#include "data/epg.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern const unsigned char ptv_font_regular[], ptv_font_regular_end[], ptv_font_bold[], ptv_font_bold_end[];
extern const unsigned char ptv_font_ui[], ptv_font_ui_end[], ptv_font_ui_semi[], ptv_font_ui_semi_end[], ptv_font_ui_bold[], ptv_font_ui_bold_end[];
extern const unsigned char ptv_ca_pem[], ptv_ca_pem_end[];
extern int host_video_on;

static uint32_t *last;
static int lw, lh, lstride;
static const char *out_dir;

/* папка рядом с программой (build/host/…), откуда бы ни запускали */
static void beside_exe(const char *name, char *out, size_t cap) {
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) { snprintf(out, cap, "build/host/%s", name); return; }
    exe[n] = 0;
    char *sl = strrchr(exe, '/');
    if (sl) *sl = 0;
    snprintf(out, cap, "%s/%s", exe, name);
}
static void mkdirs(const char *path) {
    char p[600];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}

#ifndef PTV_NO_X11
int host_window_open(void);
void host_window_present(const uint32_t *px, int w, int h, int stride);
int host_window_poll(void);
void host_keys_help(void);
void host_window_video_tick(void);
#endif
extern int host_real_video;
static int windowed;

static void present(const uint32_t *px, int w, int h, int stride) {
#ifndef PTV_NO_X11
    if (windowed) host_window_present(px, w, h, stride);
#endif
    if (!last) last = malloc((size_t)w * h * 4);
    lw = w;
    lh = h;
    lstride = w;
    for (int y = 0; y < h; y++) memcpy(last + (size_t)y * w, px + (size_t)y * stride, (size_t)w * 4);
    (void)lstride;
}

static void shot(const char *name) {
    if (!last) return;
    char path[600];
    snprintf(path, sizeof path, "%s/%s.ppm", out_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "shot: %s: %s\n", path, strerror(errno)); return; }
    fprintf(f, "P6\n%d %d\n255\n", lw, lh);
    unsigned char *row = malloc((size_t)lw * 3);
    for (int y = 0; y < lh; y++) {
        for (int x = 0; x < lw; x++) {
            uint32_t c = last[(size_t)y * lw + x];
            unsigned a = c >> 24;
            /* «видео» под окном: тёплый градиент с полосами */
            unsigned vr = 0, vg = 0, vb = 0;
            if (host_video_on == 1) {
                vr = (unsigned)(60 + x * 120 / lw);
                vg = (unsigned)(90 + y * 90 / lh);
                vb = (unsigned)(140 + ((x + y) / 40 % 2) * 30);
            }
            row[x * 3] = (unsigned char)((((c >> 16) & 255) * a + vr * (255 - a)) / 255);
            row[x * 3 + 1] = (unsigned char)((((c >> 8) & 255) * a + vg * (255 - a)) / 255);
            row[x * 3 + 2] = (unsigned char)(((c & 255) * a + vb * (255 - a)) / 255);
        }
        fwrite(row, 1, (size_t)lw * 3, f);
    }
    free(row);
    fclose(f);
    printf("shot %s\n", path);
}

static int code_of(const char *n) {
    static const struct { const char *n; int c; } t[] = {
        {"left", 105}, {"right", 106}, {"up", 103}, {"down", 108}, {"ok", 28}, {"back", 158}, {"menu", 139}, {"info", 358},
        {"guide", 365}, {"chup", 402}, {"chdown", 403}, {"red", 398}, {"green", 399}, {"yellow", 400}, {"blue", 401},
        {"play", 164}, {"stop", 128}, {"rew", 168}, {"ff", 208}, {"home", 102}, {"list", 395}};
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(t[i].n, n)) return t[i].c;
    if (n[0] >= '0' && n[0] <= '9' && !n[1]) return n[0] == '0' ? 11 : 1 + (n[0] - '0');
    return 0;
}

static void run(double secs) {
    double end = app_now() + secs;
    do {
        app_frame();
        struct timespec w = {0, 16000000};
        nanosleep(&w, NULL);
    } while (app_now() < end && !app_quitting());
}

static void root_draw(screen *s, gfx_t *g) { (void)s; epg_frame(); tv_draw(g); }
static void root_tick(screen *s, double now) { (void)s; tv_tick(now); }
static screen root = {.name = "tv", .draw = root_draw, .tick = root_tick};

int main(int argc, char **argv) {
    static char data[600], shots[600];
    beside_exe("data", data, sizeof data);
    beside_exe("shots", shots, sizeof shots);
    out_dir = shots;
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) snprintf(data, sizeof data, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
        else if (!strcmp(argv[i], "--window")) windowed = 1;
        else if (!strcmp(argv[i], "--no-video")) host_real_video = -1;
        else break;
    }
#ifdef PTV_NO_X11
    if (windowed) { fprintf(stderr, "собрано без X11: окна нет (нужен пакет libx11-dev)\n"); return 1; }
#else
    host_real_video = windowed && host_real_video >= 0;
    if (windowed) { if (host_window_open()) return 1; host_keys_help(); }
#endif
    mkdirs(data);
    mkdirs(out_dir);
    fprintf(stderr, "data_dir=%s\n", data);
    app_platform p = {.present = present, .data_dir = data};
    p.font_mem[TX_REGULAR] = ptv_font_regular; p.font_len[TX_REGULAR] = (size_t)(ptv_font_regular_end - ptv_font_regular);
    p.font_mem[TX_BOLD] = ptv_font_bold; p.font_len[TX_BOLD] = (size_t)(ptv_font_bold_end - ptv_font_bold);
    p.font_mem[TX_UI] = ptv_font_ui; p.font_len[TX_UI] = (size_t)(ptv_font_ui_end - ptv_font_ui);
    p.font_mem[TX_UI_SEMI] = ptv_font_ui_semi; p.font_len[TX_UI_SEMI] = (size_t)(ptv_font_ui_semi_end - ptv_font_ui_semi);
    p.font_mem[TX_UI_BOLD] = ptv_font_ui_bold; p.font_len[TX_UI_BOLD] = (size_t)(ptv_font_ui_bold_end - ptv_font_ui_bold);
    net_set_ca_pem((const char *)ptv_ca_pem, (size_t)(ptv_ca_pem_end - ptv_ca_pem));
    if (app_init(&p)) { fprintf(stderr, "app_init failed\n"); return 1; }
    http_init((const char *)ptv_ca_pem, (size_t)(ptv_ca_pem_end - ptv_ca_pem));
    prefs_load();
    repo_init();
    fprintf(stderr, "playlists=%d channels=%d epg_sources=%d\n", R.npl, R.nch, R.nepg);
    app_push(&root);
    tv_start();
    run(0.5);
    for (; i < argc && !app_quitting(); i++) {
        const char *a = argv[i];
        if (!strncmp(a, "wait:", 5)) run(atof(a + 5));
        else if (!strncmp(a, "shot:", 5)) { run(0.05); shot(a + 5); }
        else if (!strncmp(a, "open:", 5)) {
            if (!strcmp(a + 5, "settings")) tv_open_settings();
            else if (!strcmp(a + 5, "guide")) tv_open_guide();
            else if (!strcmp(a + 5, "schedule")) tv_open_schedule(T.cur, 0);
            else if (!strcmp(a + 5, "recent")) tv_open_recent();
            else if (!strcmp(a + 5, "menu")) tv_open_menu(T.cur, T.group, 0);
            else if (!strcmp(a + 5, "phone")) form_playlist_phone();
            else if (!strcmp(a + 5, "search")) search_open();
            else if (!strcmp(a + 5, "teletext")) tv_teletext();
            else if (!strcmp(a + 5, "subs")) tv_set_subtitles(1);
            else if (!strcmp(a + 5, "form")) form_playlist(-1);
            else if (!strcmp(a + 5, "files")) { form_playlist(-1); files_open(NULL, NULL); }
            run(0.1);
        } else if (!strncmp(a, "search:", 7)) { search_open_query(a + 7); run(0.1); }
        else if (!strncmp(a, "lang:", 5)) { P.lang = !strcmp(a + 5, "en") ? LANG_EN : LANG_RU; lang_set(P.lang); app_dirty(); }
        else if (!strncmp(a, "key:", 4) || !strncmp(a, "long:", 5) || !strncmp(a, "hold:", 5)) {
            char name[32];
            double hold = 0;
            if (a[0] == 'k') snprintf(name, sizeof name, "%s", a + 4);
            else {
                snprintf(name, sizeof name, "%s", a + 5);
                char *c = strchr(name, ':');
                hold = 1.0;
                if (c) { *c = 0; hold = atof(c + 1); }
            }
            int code = code_of(name);
            if (!code) { fprintf(stderr, "unknown key %s\n", name); continue; }
            keys_raw(code, 1);
            if (hold > 0) {
                double end = app_now() + hold;
                while (app_now() < end) { run(0.1); keys_raw(code, 2); }
            } else run(0.03);
            keys_raw(code, 0);
            run(0.15);
        } else fprintf(stderr, "unknown step %s\n", a);
    }
#ifndef PTV_NO_X11
    /* окно: работать, пока его не закрыли (или Home — выход приложения) */
    while (windowed && !app_quitting() && host_window_poll()) {
        app_frame();
        host_window_video_tick();
        struct timespec w = {0, 10000000};
        nanosleep(&w, NULL);
    }
#endif
    prefs_save();
    store_save();
    fflush(stdout);
    _exit(0);
}
