#define _POSIX_C_SOURCE 200809L
#include "player.h"
#include "app.h"
#include "net.h"
#include "player-hmc.h"
#include "sw_player.h"
#include "core/prefs.h"
#include "core/util.h"
#ifndef PTV_HOST
#include <libavutil/log.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { struct hmc_player *old, *h; struct swp *old_sw, *sw; char url[4096], headers[1024], why[96]; double start; int arc; } pjob;

static struct {
    int busy, want;
    char url[4096], headers[1024];
    double start;
    int arc;
    struct hmc_player *h;
    struct swp *sw;
    int state;
    char why[96];
    struct pl_info snap;
    double snap_at;
    int aspect_applied;
    char codecs[96];
} S;

#ifndef PTV_HOST
/* журнал FFmpeg без адресов: в ссылке плейлиста бывают логин и ключ */
static void ff_log(void *avcl, int level, const char *fmt, va_list vl) {
    char line[512];
    int pp = 1;
    if (level > AV_LOG_WARNING) return;
    av_log_format_line(avcl, level, fmt, vl, line, sizeof line, &pp);
    for (char *p = strstr(line, "://"); p; p = strstr(p + 3, "://")) {
        char *e = p + 3;
        while (*e && *e != ' ' && *e != '\'' && *e != '"' && *e != '\n') e++;
        memmove(p + 6, e, strlen(e) + 1);
        memcpy(p + 3, "***", 3);
    }
    fprintf(stderr, "ffmpeg: %s", line);
}
#endif

static int software_reason(const char *why) {
    return !strcmp(why, "create_player") || !strncmp(why, "manager", 7) || !strcmp(why, "dlopen") || !strcmp(why, "symbols") ||
           !strncmp(why, "video_codec=", 12) || !strncmp(why, "audio_codec=", 12) || !strncmp(why, "open_video=", 11) ||
           !strncmp(why, "open_audio=", 11) || !strncmp(why, "prepare_error=", 14);
}

static void job_work(void *p) {
    pjob *j = p;
    if (j->old) { hmc_player_destroy(j->old); j->old = NULL; }
    if (j->old_sw) { swp_destroy(j->old_sw); j->old_sw = NULL; }
    if (!j->url[0]) return;
    j->h = hmc_player_open(j->url, j->headers[0] ? j->headers : NULL, j->start, SCREEN_W, SCREEN_H, j->why, sizeof j->why);
    if (!j->h && software_reason(j->why)) {
        if (j->arc) {
            /* swp_open не принимает start и не умеет seek: архив с начала ссылки — неверный участок */
            snprintf(j->why, sizeof j->why, "archive_software");
            fprintf(stderr, "player=software_archive_skip\n");
        } else {
            char why2[96];
            fprintf(stderr, "player=software reason=%s\n", j->why);
            j->sw = swp_open(j->url, why2, sizeof why2);
            if (!j->sw) fprintf(stderr, "player=software_failed reason=%s\n", why2);
        }
    }
}

static void launch(void);

static void job_done(void *p) {
    pjob *j = p;
    S.busy = 0;
    S.h = j->h;
    S.sw = j->sw;
    if (j->url[0] && !S.want) {
        if (j->h || j->sw) { S.state = PL_OPENING; S.why[0] = 0; S.aspect_applied = 0; S.snap_at = 0; }
        else { S.state = PL_FAILED; str_copy(S.why, sizeof S.why, j->why); }
    }
    free(j);
    app_dirty();
    launch();
}

static void launch(void) {
    if (S.busy) return;
    pjob *j = NULL;
    if (S.want) {
        j = calloc(1, sizeof *j);
        if (!j) return;
        j->old = S.h;
        j->old_sw = S.sw;
        S.h = NULL;
        S.sw = NULL;
        str_copy(j->url, sizeof j->url, S.url);
        str_copy(j->headers, sizeof j->headers, S.headers);
        j->start = S.start;
        j->arc = S.arc;
        S.want = 0;
        S.state = S.url[0] ? PL_OPENING : PL_IDLE;
    } else return;
    S.busy = 1;
    memset(&S.snap, 0, sizeof S.snap);
    S.codecs[0] = 0;
    pool_submit(job_work, job_done, j);
}

void pl_open(const char *url, const char *headers, double start, int archive) {
#ifndef PTV_HOST
    static int log_set;
    if (!log_set) { av_log_set_callback(ff_log); av_log_set_level(AV_LOG_WARNING); log_set = 1; }
#endif
    str_copy(S.url, sizeof S.url, url);
    str_copy(S.headers, sizeof S.headers, headers ? headers : "");
    S.start = start;
    S.arc = archive;
    S.want = 1;
    S.state = PL_OPENING;
    S.why[0] = 0;
    launch();
}

void pl_close(void) {
    S.url[0] = 0;
    S.want = S.h || S.sw || S.busy;
    S.state = PL_IDLE;
    launch();
}

int pl_state(void) { return S.state; }
const char *pl_why(void) { return S.why; }

static int raw_state(void) {
    if (S.h) return (int)hmc_player_state(S.h);
    if (S.sw) { int s = swp_state(S.sw); return s == SWP_PLAYING ? HMC_PLAYING : s == SWP_ENDED ? HMC_ENDED : HMC_STARTING; }
    return -1;
}

void pl_tick(void) {
    if (S.busy || S.want || (!S.h && !S.sw)) return;
    int st = raw_state();
    double now = app_now();
    if (st == HMC_PLAYING) {
        if (S.state != PL_PLAYING) { S.state = PL_PLAYING; app_dirty(); }
        if (S.h && !S.aspect_applied) {
            S.aspect_applied = 1;
            if (P.aspect) pl_set_aspect(P.aspect);
        }
        if (now >= S.snap_at) {
            S.snap_at = now + 1.0;
            if (S.h) hmc_player_snapshot(S.h, &S.snap, "", 1);   /* кодек, пиксели, fps и битрейт — раз в секунду */
            else {
                int snd = 0;
                swp_info(S.sw, &S.snap.width, &S.snap.height, S.snap.video_codec, sizeof S.snap.video_codec, S.snap.audio_codec, sizeof S.snap.audio_codec, &snd);
            }
            snprintf(S.codecs, sizeof S.codecs, "%s%s%s", S.snap.video_codec, S.snap.audio_codec[0] ? " · " : "", S.snap.audio_codec);
        }
    } else if (st == HMC_FAILED) {
        if (S.state != PL_FAILED) {
            int err = hmc_player_error(S.h);
            const char *stage = hmc_player_error_stage(S.h);
            fprintf(stderr, "player_state=failed error=%d stage=%s\n", err, stage);
            S.state = PL_FAILED;
            snprintf(S.why, sizeof S.why, "player_%s_%d", stage, err);
            app_dirty();
        }
    } else if (st == HMC_ENDED) {
        if (S.state != PL_ENDED) { fprintf(stderr, "player_state=ended\n"); S.state = PL_ENDED; app_dirty(); }
    }
    if (S.sw && swp_poll(S.sw)) app_dirty();
}

double pl_position(void) {
    if (S.h && !S.busy) {
        struct pl_info v;
        hmc_player_snapshot(S.h, &v, "", 0);
        return v.position;
    }
    return 0;
}
double pl_duration(void) { return S.h && !S.busy ? S.snap.duration : 0; }
int pl_pause(int on) {
    if (!S.h || S.busy) return -1;
    struct pl_action a = {PL_PAUSE, on ? 1 : 0};
    int rc = hmc_player_action(S.h, a);
    S.snap.paused = on && rc >= 0;
    return rc < 0 ? -1 : 0;
}
int pl_paused(void) { return S.h && S.snap.paused; }
int pl_seek(double s) {
    if (!S.h || S.busy) return -1;
    struct pl_action a = {PL_SEEK_ABS, s};
    return hmc_player_action(S.h, a) < 0 ? -1 : 0;
}
int pl_software(void) { return S.sw != NULL; }
int pl_rebuffering(void) { return S.h && S.snap.rebuffering; }
int pl_video_size(int *w, int *h) {
    if (S.snap.height <= 0) return -1;
    *w = S.snap.width;
    *h = S.snap.height;
    return 0;
}
const char *pl_codecs(void) { return S.codecs; }
int pl_video_bitrate(void) { return S.snap.stats_valid & PL_STAT_VIDEO_RATE ? (int)(S.snap.video_bitrate + 0.5) : 0; }
int pl_set_aspect(int a) {
    static const int map[] = {PL_ASPECT_FIT, PL_ASPECT_STRETCH, PL_ASPECT_16_9, PL_ASPECT_4_3};
    if (!S.h || S.busy || a < 0 || a > 3) return -1;
    struct pl_action act = {PL_ASPECT, map[a]};
    return hmc_player_action(S.h, act) < 0 ? -1 : 0;
}
int pl_audio_count(void) { return S.h ? (S.snap.audio_count < PL_TRACKS ? S.snap.audio_count : PL_TRACKS) : 0; }
const char *pl_audio_label(int i) {
    static char b[200];
    if (i < 0 || i >= pl_audio_count()) return "";
    const struct pl_track *t = &S.snap.audio_tracks[i];
    snprintf(b, sizeof b, "%s%s%s", t->label[0] ? t->label : "—", t->meta[0] ? "  ·  " : "", t->meta);
    return b;
}
int pl_audio_selected(void) {
    for (int i = 0; i < pl_audio_count(); i++) if (S.snap.audio_tracks[i].selected) return i;
    return -1;
}
int pl_audio_select(int i) {
    if (!S.h || i < 0 || i >= pl_audio_count()) return -1;
    struct pl_action a = {PL_AUDIO, S.snap.audio_tracks[i].id};
    int rc = hmc_player_action(S.h, a);
    S.snap_at = 0;
    return rc < 0 ? -1 : 0;
}

/* дорожки субтитров, которые видит системный плеер (сам он их не показывает): подпись — язык или название,
 * кодек — имя FFmpeg (dvb_teletext, dvb_subtitle, webvtt…) */
int pl_sub_count(void) { return S.h ? (S.snap.sub_count < PL_TRACKS ? S.snap.sub_count : PL_TRACKS) : 0; }
const char *pl_sub_label(int i) { return i >= 0 && i < pl_sub_count() ? S.snap.sub_tracks[i].label : ""; }
const char *pl_sub_codec(int i) { return i >= 0 && i < pl_sub_count() ? S.snap.sub_tracks[i].meta : ""; }

void pl_draw_video(gfx_t *g) {
    if (S.sw && !S.busy && S.state == PL_PLAYING) swp_draw(S.sw, g->px, g->w, g->h, g->stride);
    else for (int y = 0; y < g->h; y++) memset(g->px + (size_t)y * g->stride, 0, (size_t)g->w * 4);
}
int pl_has_picture(void) { return S.state == PL_PLAYING && !S.busy && !S.want; }

void pl_shutdown(void) {
    S.want = 0;
    S.url[0] = 0;
    for (int i = 0; i < 150 && S.busy; i++) { app_service(); struct timespec w = {0, 20000000}; nanosleep(&w, NULL); }
    if (!S.busy && S.h) { hmc_player_destroy(S.h); S.h = NULL; }
    if (!S.busy && S.sw) { swp_destroy(S.sw); S.sw = NULL; }
}
