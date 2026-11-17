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
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include "core/http.h"

/* адрес ретранслятора: читать его, как читал бы плеер (телетекст разбирается по пути) */
static void *drain(void *arg) {
    char *url = arg;
    char err[120];
    hs_req rq = {.url = url, .timeout_ms = 10000};
    hs *h = hs_open(&rq, err, sizeof err);
    char buf[16384];
    while (h && hs_read(h, buf, sizeof buf) > 0) {}
    if (h) hs_close(h);
    free(url);
    return NULL;
}

static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* ---------- настоящее видео в окне ПК: ffmpeg -> кадры BGRA по трубе, звук — сам в PulseAudio ---------- */
#define VW 960
#define VH 540
int host_real_video;                 /* host_main --window: потоки по-настоящему (кроме demo.invalid) */
static pthread_mutex_t vmu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t *vframe;
static int vseq;

/* кадр видео для окна; 0 — нет */
int host_video_frame(const uint32_t **px, int *w, int *h) {
    *px = vframe;
    *w = VW;
    *h = VH;
    return vframe ? vseq : 0;
}
void host_video_lock(int on) { if (on) pthread_mutex_lock(&vmu); else pthread_mutex_unlock(&vmu); }

struct hmc_player {
    double opened, base, paused_at; int paused, vod; double dur; char url[16384]; char headers[2048];
    /* ffmpeg */
    int ff, fd, got, exited, stop, mute;
    int nsub;                     /* дорожки субтитров (ffprobe) — как их видит системный плеер ТВ */
    char sub_codec[8][32], sub_lang[8][32];
    pid_t pid;
    pthread_t th;
    double first_at;
};
int host_video_on;   /* host_main: рисовать тестовую картинку под интерфейсом */

static void *reader(void *arg) {
    struct hmc_player *h = arg;
    size_t sz = (size_t)VW * VH * 4, got = 0;
    uint8_t *buf = malloc(sz);
    while (buf && !h->stop) {
        ssize_t r = read(h->fd, buf + got, sz - got);
        if (r <= 0) break;
        got += (size_t)r;
        if (got < sz) continue;
        got = 0;
        pthread_mutex_lock(&vmu);
        if (!vframe) vframe = malloc(sz);
        if (vframe) memcpy(vframe, buf, sz);
        vseq++;
        pthread_mutex_unlock(&vmu);
        if (!h->got) { h->got = 1; h->first_at = mono(); }
    }
    free(buf);
    h->exited = 1;
    return NULL;
}

static void ff_stop(struct hmc_player *h) {
    if (!h->pid) return;
    h->stop = 1;
    kill(h->pid, SIGCONT);
    kill(h->pid, SIGKILL);
    waitpid(h->pid, NULL, 0);
    close(h->fd);
    pthread_join(h->th, NULL);
    h->pid = 0;
}

/* дорожки субтитров: ffprobe (как системный плеер — кодек и язык; язык — по-русски для известных) */
static void probe_subs(struct hmc_player *h) {
    char cmd[17000];
    /* у HLS ffprobe выводит поток дважды (ещё раз — в разделе программы) и пустые строки: номер потока — чтобы отсеять */
    snprintf(cmd, sizeof cmd, "env -u http_proxy -u HTTP_PROXY -u https_proxy -u HTTPS_PROXY ffprobe -v error -select_streams s "
             "-show_entries stream=index,codec_name:stream_tags=language -of csv=p=0 '%s' 2>/dev/null", h->url);
    FILE *f = popen(cmd, "r");
    char line[200];
    int seen[8], ns = 0;
    while (f && fgets(line, sizeof line, f) && h->nsub < 8) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *codec = strchr(line, ',');
        if (!line[0] || !codec || !codec[1]) continue;
        *codec++ = 0;
        int idx = atoi(line), dup = 0;
        for (int k = 0; k < ns; k++) dup |= seen[k] == idx;
        if (dup) continue;
        seen[ns++] = idx;
        char *comma = strchr(codec, ',');
        if (comma) *comma = 0;
        const char *lang = comma ? comma + 1 : "";
        snprintf(h->sub_codec[h->nsub], 32, "%s", codec);
        snprintf(h->sub_lang[h->nsub], 32, "%s", !strncmp(lang, "rus", 3) ? "Русский" : !strncmp(lang, "eng", 3) ? "Английский" : lang);
        h->nsub++;
    }
    if (f) pclose(f);
}

static int ff_start(struct hmc_player *h, double start) {
    int p[2];
    if (pipe(p)) return -1;
    char ss[32];
    snprintf(ss, sizeof ss, "%.2f", start);
    const char *argv[48];
    int n = 0;
    argv[n++] = "ffmpeg"; argv[n++] = "-hide_banner"; argv[n++] = "-loglevel"; argv[n++] = "error"; argv[n++] = "-nostdin";
    if (h->headers[0]) { argv[n++] = "-headers"; argv[n++] = h->headers; }
    argv[n++] = "-re";
    if (start > 0.5) { argv[n++] = "-ss"; argv[n++] = ss; }
    argv[n++] = "-i"; argv[n++] = h->url;
    argv[n++] = "-map"; argv[n++] = "0:v:0";
    argv[n++] = "-vf"; argv[n++] = "scale=960:540:force_original_aspect_ratio=decrease,pad=960:540:(ow-iw)/2:(oh-ih)/2,fps=25";
    argv[n++] = "-pix_fmt"; argv[n++] = "bgra"; argv[n++] = "-f"; argv[n++] = "rawvideo"; argv[n++] = "pipe:1";
    if (!getenv("PTV_NO_AUDIO") && !h->mute) { argv[n++] = "-map"; argv[n++] = "0:a:0?"; argv[n++] = "-f"; argv[n++] = "pulse"; argv[n++] = "Perfect TV"; }
    argv[n] = NULL;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (!pid) {
        /* как на ТВ: без прокси из окружения (ffmpeg иначе идёт через него и к 127.0.0.1) */
        unsetenv("http_proxy"); unsetenv("HTTP_PROXY"); unsetenv("https_proxy"); unsetenv("HTTPS_PROXY"); unsetenv("all_proxy"); unsetenv("ALL_PROXY");
        dup2(p[1], 1);
        close(p[0]);
        close(p[1]);
        execvp("ffmpeg", (char *const *)argv);
        _exit(127);
    }
    close(p[1]);
    h->fd = p[0];
    h->pid = pid;
    h->stop = h->got = h->exited = 0;
    h->opened = mono();
    h->base = start;
    if (pthread_create(&h->th, NULL, reader, h)) { ff_stop(h); return -1; }
    return 0;
}

struct hmc_player *hmc_player_open(const char *url, const char *headers, double start_s, int dw, int dh, char *why, size_t why_cap) {
    (void)dw; (void)dh;
    struct hmc_player *h = calloc(1, sizeof *h);
    snprintf(h->url, sizeof h->url, "%s", url);
    snprintf(h->headers, sizeof h->headers, "%s", headers ? headers : "");
    h->ff = host_real_video && !strstr(url, ".invalid");
    h->vod = strstr(url, "archive-") != NULL && !strstr(url, "-now");
    if (h->vod) {   /* archive-START-DURATION */
        const char *q = strchr(strstr(url, "archive-") + 8, '-');
        h->dur = q ? atof(q + 1) : 3600;
    }
    if (h->ff) {
        probe_subs(h);
        if (ff_start(h, start_s)) { snprintf(why, why_cap, "open_input=ffmpeg"); free(h); return NULL; }
        fprintf(stderr, "host_player ffmpeg start=%.0f\n", start_s);
        host_video_on = 1;
        return h;
    }
    usleep(300000);
    if (strstr(url, "fail")) { snprintf(why, why_cap, "open_input=-5"); free(h); return NULL; }
    if (strstr(url, "sw")) { snprintf(why, why_cap, "video_codec=mpeg2video"); free(h); return NULL; }
    h->opened = mono();
    h->base = start_s;
    if (!strncmp(url, "http://127.0.0.1:", 17)) {
        pthread_t t;
        if (!pthread_create(&t, NULL, drain, strdup(url))) pthread_detach(t);
    }
    fprintf(stderr, "stub_player open start=%.0f vod=%d\n", start_s, h->vod);
    host_video_on = 1;
    return h;
}
void hmc_player_destroy(struct hmc_player *h) {
    ff_stop(h);
    pthread_mutex_lock(&vmu);
    free(vframe);
    vframe = NULL;
    pthread_mutex_unlock(&vmu);
    free(h);
    host_video_on = 0;
}
static double pos(struct hmc_player *h) {
    double t0 = h->ff ? (h->got ? h->first_at : mono()) : h->opened;
    return h->base + ((h->paused ? h->paused_at : mono()) - t0);
}
enum hmc_state hmc_player_state(struct hmc_player *h) {
    if (h->ff) {
        /* нет PulseAudio — ffmpeg падает целиком: ещё раз без звука */
        if (!h->got && h->exited && !h->mute && mono() - h->opened < 5) {
            fprintf(stderr, "host_player: без звука\n");
            double b = h->base;
            ff_stop(h);
            h->mute = 1;
            if (!ff_start(h, b)) return HMC_STARTING;
        }
        if (!h->got) return h->exited || mono() - h->opened > 25 ? HMC_FAILED : HMC_STARTING;
        if (h->exited && !h->paused) return h->vod ? HMC_ENDED : HMC_FAILED;
        return HMC_PLAYING;
    }
    if (mono() - h->opened < 0.2) return HMC_STARTING;
    if (h->vod && h->dur > 0 && pos(h) >= h->dur) return HMC_ENDED;
    return HMC_PLAYING;
}
int hmc_player_error(const struct hmc_player *h) { (void)h; return 0; }
const char *hmc_player_error_stage(const struct hmc_player *h) { return h->ff && !h->got ? "open_input" : "none"; }
int hmc_player_control_error(struct hmc_player *h) { (void)h; return 0; }
void hmc_player_snapshot(struct hmc_player *h, struct pl_info *v, const char *fallback, int details) {
    (void)fallback; (void)details;
    memset(v, 0, sizeof *v);
    v->position = pos(h);
    v->duration = h->vod ? h->dur : 0;
    v->paused = h->paused;
    v->width = 1920;
    v->height = 1080;
    snprintf(v->video_codec, sizeof v->video_codec, h->ff ? "ffmpeg" : "h264");
    snprintf(v->audio_codec, sizeof v->audio_codec, "AAC");
    if (h->ff) {
        v->audio_count = 1;
        for (int i = 0; i < h->nsub; i++) {
            snprintf(v->sub_tracks[i].label, sizeof v->sub_tracks[i].label, "%s", h->sub_lang[i]);
            snprintf(v->sub_tracks[i].meta, sizeof v->sub_tracks[i].meta, "%s", h->sub_codec[i]);
        }
        v->sub_count = h->nsub;
        return;
    }
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
        if (a.value && !h->paused) {
            h->paused = 1;
            h->paused_at = mono();
            if (h->ff && h->pid) kill(h->pid, SIGSTOP);
        } else if (!a.value && h->paused) {
            double p = pos(h);
            h->paused = 0;
            /* ffmpeg с -re после остановки догонял бы часы: проще начать с места паузы */
            if (h->ff) { ff_stop(h); ff_start(h, h->vod || p > 0.5 ? p : 0); }
            else h->opened += mono() - h->paused_at;
        }
        return 0;
    }
    if (a.type == PL_SEEK_ABS) {
        if (h->ff) { ff_stop(h); h->paused = 0; return ff_start(h, a.value); }
        h->base = a.value; h->opened = mono(); if (h->paused) h->paused_at = h->opened;
        return 0;
    }
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
