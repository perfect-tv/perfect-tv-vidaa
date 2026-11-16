/* Заглушка плееров ТВ для запуска на ПК (python3 tools/build.py --host): «поток» открывается за 0,3 с и
 * «играет» (позиция идёт по часам), адрес с «fail» — ошибка открытия, с «sw» — программный плеер. Видео
 * под интерфейсом рисует host_main.c (тестовая картинка). */
#define _GNU_SOURCE
#include "player-hmc.h"
#include "sw_player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

struct hmc_player { double opened, base, paused_at; int paused, vod; double dur; char url[512]; };
int host_video_on;   /* host_main: рисовать тестовую картинку под интерфейсом */

struct hmc_player *hmc_player_open(const char *url, const char *headers, double start_s, int dw, int dh, char *why, size_t why_cap) {
    (void)headers; (void)dw; (void)dh;
    usleep(300000);
    if (strstr(url, "fail")) { snprintf(why, why_cap, "open_input=-5"); return NULL; }
    if (strstr(url, "sw")) { snprintf(why, why_cap, "video_codec=mpeg2video"); return NULL; }
    struct hmc_player *h = calloc(1, sizeof *h);
    h->opened = mono();
    h->base = start_s;
    snprintf(h->url, sizeof h->url, "%s", url);
    h->vod = strstr(url, "archive-") != NULL && !strstr(url, "-now");
    if (h->vod) {   /* archive-START-DURATION */
        const char *p = strstr(url, "archive-") + 8;
        long long s = atoll(p);
        const char *q = strchr(p, '-');
        h->dur = q ? atof(q + 1) : 3600;
        (void)s;
    }
    fprintf(stderr, "stub_player open start=%.0f vod=%d\n", start_s, h->vod);
    host_video_on = 1;
    return h;
}
void hmc_player_destroy(struct hmc_player *h) { free(h); host_video_on = 0; }
static double pos(struct hmc_player *h) { return h->base + ((h->paused ? h->paused_at : mono()) - h->opened); }
enum hmc_state hmc_player_state(struct hmc_player *h) {
    if (mono() - h->opened < 0.2) return HMC_STARTING;
    if (h->vod && h->dur > 0 && pos(h) >= h->dur) return HMC_ENDED;
    return HMC_PLAYING;
}
int hmc_player_error(const struct hmc_player *h) { (void)h; return 0; }
const char *hmc_player_error_stage(const struct hmc_player *h) { (void)h; return "none"; }
int hmc_player_control_error(struct hmc_player *h) { (void)h; return 0; }
void hmc_player_snapshot(struct hmc_player *h, struct pl_info *v, const char *fallback, int details) {
    (void)fallback; (void)details;
    memset(v, 0, sizeof *v);
    v->position = pos(h);
    v->duration = h->vod ? h->dur : 0;
    v->paused = h->paused;
    v->width = 1920;
    v->height = 1080;
    snprintf(v->video_codec, sizeof v->video_codec, "h264");
    snprintf(v->audio_codec, sizeof v->audio_codec, "AAC");
    v->audio_count = 2;
    snprintf(v->audio_tracks[0].label, sizeof v->audio_tracks[0].label, "Русский");
    snprintf(v->audio_tracks[0].meta, sizeof v->audio_tracks[0].meta, "AAC 2.0");
    v->audio_tracks[0].selected = 1;
    snprintf(v->audio_tracks[1].label, sizeof v->audio_tracks[1].label, "English");
    snprintf(v->audio_tracks[1].meta, sizeof v->audio_tracks[1].meta, "AAC 2.0");
    v->audio_tracks[1].id = 1;
}
int hmc_player_action(struct hmc_player *h, struct pl_action a) {
    if (a.type == PL_PAUSE) {
        if (a.value && !h->paused) { h->paused = 1; h->paused_at = mono(); }
        else if (!a.value && h->paused) { h->opened += mono() - h->paused_at; h->paused = 0; }
        return 0;
    }
    if (a.type == PL_SEEK_ABS) { h->base = a.value; h->opened = mono(); if (h->paused) h->paused_at = h->opened; return 0; }
    return 0;
}

struct swp { double opened; };
struct swp *swp_open(const char *url, char *why, size_t why_cap) {
    (void)url; (void)why; (void)why_cap;
    struct swp *s = calloc(1, sizeof *s);
    s->opened = mono();
    host_video_on = 2;
    return s;
}
void swp_destroy(struct swp *s) { free(s); host_video_on = 0; }
int swp_state(struct swp *s) { return mono() - s->opened > 0.3 ? SWP_PLAYING : SWP_STARTING; }
int swp_poll(struct swp *s) { (void)s; return 0; }
void swp_draw(struct swp *s, uint32_t *px, int w, int h, int stride) {
    (void)s;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) px[(size_t)y * stride + x] = 0xff000000u | (uint32_t)((x * 255 / w) << 8) | (uint32_t)(y * 255 / h);
}
void swp_info(struct swp *s, int *w, int *h, char *vc, size_t vcap, char *ac, size_t acap, int *sound) {
    (void)s;
    *w = 720; *h = 576;
    snprintf(vc, vcap, "mpeg2video");
    snprintf(ac, acap, "mp2");
    *sound = 1;
}
