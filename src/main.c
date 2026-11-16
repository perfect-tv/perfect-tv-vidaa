/* Perfect TV для VIDAA: точка входа. Оболочка ТВ — fw/tv_shell.c SDK (жизненный цикл APM, DirectFB-окно,
 * пульт из /dev/input/event*, журнал, обработчик падений, выход через _exit). Интерфейс — один экран каркаса
 * (root), внутри — свой стек слоёв (src/ui/layer.c); клавиши приходят сырыми (raw_key) — для долгих нажатий. */
#define _POSIX_C_SOURCE 200809L
#include "tv_shell.h"
#include "app.h"
#include "core/http.h"
#include "core/lang.h"
#include "core/prefs.h"
#include "data/loaders.h"
#include "data/repo.h"
#include "player/player.h"
#include "ui/keys.h"
#include "ui/tv.h"
#include "ui/u.h"
#include <stdio.h>
#include <string.h>

#ifndef PTV_VERSION
#define PTV_VERSION "dev"
#endif

extern const unsigned char ptv_font_regular[], ptv_font_regular_end[], ptv_font_bold[], ptv_font_bold_end[];
extern const unsigned char ptv_font_ui[], ptv_font_ui_end[], ptv_font_ui_semi[], ptv_font_ui_semi_end[], ptv_font_ui_bold[], ptv_font_ui_bold_end[];
extern const unsigned char ptv_ca_pem[], ptv_ca_pem_end[];

static void root_draw(screen *s, gfx_t *g) { (void)s; tv_draw(g); }
static void root_tick(screen *s, double now) { (void)s; tv_tick(now); }
static int root_key(screen *s, int k, int repeat) { (void)s; (void)k; (void)repeat; return 1; }
static screen root = {.name = "tv", .draw = root_draw, .tick = root_tick, .key = root_key};

static void ui_start(void) {
    http_init((const char *)ptv_ca_pem, (size_t)(ptv_ca_pem_end - ptv_ca_pem));
    prefs_load();
    repo_init();
    fprintf(stderr, "playlists=%d channels=%d epg_sources=%d\n", R.npl, R.nch, R.nepg);
    app_push(&root);
    tv_start();
}

/* каждое событие пульта — до каркаса: свои долгие нажатия и удержание */
static int raw_key(int code, int value) {
    if (value == 1) fprintf(stderr, "key code=%d\n", code);
    keys_raw(code, value);
    return 1;
}

static void on_shutdown(void) {
    loaders_cancel();
    pl_shutdown();
    prefs_save();
}

int main(int argc, char **argv) {
    static tv_shell_cfg c;
    c.register_name = "PerfectTVNative";   /* = registerName в app.json */
    c.binary = "perfecttv-app";            /* = binary в app.json */
    c.version = PTV_VERSION;
    c.env_prefix = "PTV";                  /* = envPrefix в app.json */
    c.user_agent = "PerfectTV/" PTV_VERSION " (VIDAA; native)";
    c.ca_pem = ptv_ca_pem;
    c.ca_len = (size_t)(ptv_ca_pem_end - ptv_ca_pem);
    c.font_mem[TX_REGULAR] = ptv_font_regular; c.font_len[TX_REGULAR] = (size_t)(ptv_font_regular_end - ptv_font_regular);
    c.font_mem[TX_BOLD] = ptv_font_bold;       c.font_len[TX_BOLD] = (size_t)(ptv_font_bold_end - ptv_font_bold);
    c.font_mem[TX_UI] = ptv_font_ui;           c.font_len[TX_UI] = (size_t)(ptv_font_ui_end - ptv_font_ui);
    c.font_mem[TX_UI_SEMI] = ptv_font_ui_semi; c.font_len[TX_UI_SEMI] = (size_t)(ptv_font_ui_semi_end - ptv_font_ui_semi);
    c.font_mem[TX_UI_BOLD] = ptv_font_ui_bold; c.font_len[TX_UI_BOLD] = (size_t)(ptv_font_ui_bold_end - ptv_font_ui_bold);
    c.ui_start = ui_start;
    c.raw_key = raw_key;
    c.on_shutdown = on_shutdown;
    return tv_shell_run(argc, argv, &c);
}
