/* Инфопанель (docs/design.md, «Инфопанель»): затемнение во всю ширину снизу, логотип, канал и бейджи,
 * передача, шкала, «ещё N мин» (в архиве — время кадра), «Далее»; закреплённая — подсказки клавиш;
 * раскрытая (↓↓) — описание с картинкой и «Далее на канале». При перемотке — пузырь с целевым временем. */
#include "screens.h"
#include "tv.h"
#include "u.h"
#include "data/tmdb.h"
#include "keys.h"
#include "app.h"
#include "core/clock.h"
#include "core/lang.h"
#include "core/prefs.h"
#include "player/player.h"
#include <stdio.h>
#include <string.h>

static int panel_top;
int info_top(void) { return panel_top ? panel_top : SH - E(20); }

static int quality_badge(char *out, size_t cap) {
    int w, h;
    if (pl_video_size(&w, &h)) return 0;
    snprintf(out, cap, "%s", h >= 2000 ? "4K" : h >= 1000 ? "FHD" : h >= 700 ? "HD" : "SD");
    return 1;
}

static void draw_bar(gfx_t *g, int x, int y, int w, int h, double frac, uint32_t col) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    d_round(g, x, y, w, h, h / 2, C_FILL_MARKED);
    if (frac > 0) d_round(g, x, y, (int)(w * frac), h, h / 2, col);
}

void info_draw(gfx_t *g) {
    channel *c = T.cur;
    if (!c) return;
    int64_t now = clock_now(), pos = T.seeking ? T.seek_to : tv_position();
    int x = E(4), right = SW - E(4), w = right - x;
    int logo_w = E(9.6), logo_h = E(6.6);
    int pi = repo_prog_at(c, pos);
    prog p = repo_prog(c, pi);
    int has = pi >= 0;
    /* высота: шапка + название + шкала + «Далее» (+ описание) (+ подсказки) */
    int h = E(2.4) + logo_h + E(1.0) + E(2.9) + E(2.2) + E(2.6);
    int exp_h = E(13);
    if (T.info_expanded) h += exp_h + E(1.2);
    if (T.info_pinned && P.hints) h += E(3.2);
    int top = SH - h - E(2.0);
    panel_top = top;
    int fade = E(5.6);
    d_vgrad(g, 0, top - fade, SW, fade, C_SCRIM & 0x00ffffffu, (C_SCRIM & 0x00ffffffu) | 0xf0000000u);
    d_fill(g, 0, top, SW, SH - top, C_SCRIM);
    int y = top + E(0.6);
    /* логотип и строка канала */
    d_logo(g, c, x, y, logo_w, logo_h, E(0.6), 1, 0);
    int tx = x + logo_w + E(1.6);
    char name[300];
    if (P.number_mode == NUM_NEAR_NAME) snprintf(name, sizeof name, "%d. %s", c->num, c->name);
    else snprintf(name, sizeof name, "%s", c->name);
    char clock[8];
    fmt_hm(now, clock);
    int clock_w = d_text_w(2.2, F_S, clock);
    d_text_r(g, right, y - E(0.2), 2.2, F_S, C_WHITE, clock);
    int ny = y + E(1.4);
    int nw = d_text_mid(g, tx, ny, 1.6, F_B, C_WHITE, name, right - tx - clock_w - E(20));
    int bx = tx + nw + E(1.0);
    char b[64], d[32];
    if (T.arc || (T.paused && tv_behind() > 3)) {
        fmt_dur(tv_behind(), d, sizeof d);
        snprintf(b, sizeof b, "%s · −%s", tr(S_ARCHIVE), d);
        bx += d_badge(g, bx, ny, 1.1, C_ARCHIVE, C_ON_ARCHIVE, -1, b) + E(0.5);
    } else bx += d_badge(g, bx, ny, 1.1, C_LIVE, C_WHITE, -1, tr(S_LIVE)) + E(0.5);
    if (T.paused) bx += d_badge(g, bx, ny, 1.1, C_FILL_BTN, C_WHITE, I_PAUSE, tr(S_PAUSED)) + E(0.5);
    if (quality_badge(b, sizeof b)) bx += d_badge(g, bx, ny, 1.1, C_FILL_BTN, C_WHITE, -1, b) + E(0.5);
    if (pl_software() && pl_has_picture()) bx += d_badge(g, bx, ny, 1.1, C_FILL_BTN, C_WHITE, -1, "SW") + E(0.5);
    if (pl_audio_count() > 1) { snprintf(b, sizeof b, tr(S_AUDIO_N), pl_audio_count()); bx += d_badge(g, bx, ny, 1.1, C_FILL_BTN, C_WHITE, -1, b) + E(0.5); }
    /* как у Android-версии: субтитры и телетекст в потоке (известно, когда поток идёт через ретранслятор) */
    if (tv_has_subtitles()) bx += d_badge(g, bx, ny, 1.1, T.subs ? C_WHITE : C_FILL_BTN, T.subs ? 0xff101010u : C_WHITE, -1, "SUB") + E(0.5);
    if (tv_has_teletext()) bx += d_badge(g, bx, ny, 1.1, C_FILL_BTN, C_WHITE, -1, "TTX") + E(0.5);
    if (repo_has_archive(c)) bx += d_badge(g, bx, ny, 1.1, C_FILL_BTN, C_WHITE, I_ARCHIVE, trp(S_DAYS_SHORT, c->archive_days));
    /* передача */
    int py = y + logo_h - E(2.6);
    const char *title = has ? p.title : tr(S_NO_EPG);
    d_text(g, tx, py - E(0.3), 2.2, F_B, has ? C_WHITE : C_TEXT3, title, right - tx);
    y += logo_h + E(1.0);
    /* шкала */
    int bar_x = x, bar_w = w - E(14), bar_y = y + E(0.6), bar_h = E(0.5);
    uint32_t col = T.arc || T.paused ? C_ARCHIVE : C_WHITE;
    if (has) {
        char a1[8], a2[8];
        fmt_hm(p.start, a1);
        fmt_hm(p.stop, a2);
        double frac = (double)(pos - p.start) / (double)(p.stop - p.start);
        draw_bar(g, bar_x, bar_y, bar_w, bar_h, frac, col);
        /* эфир внутри шкалы, когда смотрим архив идущей передачи */
        if ((T.arc || T.paused) && now < p.stop) {
            int lx = bar_x + (int)(bar_w * (double)(now - p.start) / (double)(p.stop - p.start));
            d_fill(g, lx - E(0.1), bar_y - (T.seeking ? E(0.5) : 0), E(0.2), bar_h + (T.seeking ? E(1.0) : 0), C_LIVE);
        }
        if (T.seeking) {
            int mx = bar_x + (int)(bar_w * (frac < 0 ? 0 : frac > 1 ? 1 : frac));
            d_circle(g, (float)mx, (float)(bar_y + bar_h / 2), (float)E(0.75), C_ARCHIVE);
        }
        char left[64];
        if (T.arc || T.paused) { char hm[8]; fmt_hm(pos, hm); snprintf(left, sizeof left, "%s", hm); }
        else snprintf(left, sizeof left, "%s", trp(S_MIN_LEFT, (long)((p.stop - now + 59) / 60)));
        d_text_mid(g, bar_x + bar_w + E(1.2), bar_y + bar_h / 2, 1.25, F_S, C_TEXT2, left, E(13));
        y = bar_y + bar_h + E(0.6);
        d_text(g, bar_x, y, 1.1, F_R, C_TEXT3, a1, 0);
        d_text_r(g, bar_x + bar_w, y, 1.1, F_R, C_TEXT3, a2);
    } else {
        draw_bar(g, bar_x, bar_y, bar_w, bar_h, 0, col);
        y = bar_y + bar_h + E(0.6);
    }
    y += E(1.8);
    /* «Далее» или пузырь перемотки */
    if (T.seeking) {
        char hm[8], bh[32], txt[64];
        fmt_hm(T.seek_to, hm);
        fmt_dur(now - T.seek_to, bh, sizeof bh);
        snprintf(txt, sizeof txt, "%s   −%s", hm, bh);
        int bw = d_text_w(1.3, F_B, txt) + E(2);
        double frac = has ? (double)(T.seek_to - p.start) / (double)(p.stop - p.start) : 0.5;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        int bxx = bar_x + (int)(bar_w * frac) - bw / 2;
        if (bxx < x) bxx = x;
        if (bxx + bw > right) bxx = right - bw;
        d_round(g, bxx, y - E(0.2), bw, E(2.4), E(0.5), C_ARCHIVE);
        d_text_mid(g, bxx + E(1), y + E(1.0), 1.3, F_B, C_ON_ARCHIVE, txt, 0);
    } else if (has) {
        int nx = x;
        nx += d_text_mid(g, nx, y + E(1.0), 1.25, F_S, C_TEXT3, tr(S_NEXT), 0) + E(1.2);
        for (int k = 1; k <= 2; k++) {
            prog q = repo_prog(c, pi + k);
            if (!q.start) break;
            char hm[8], t[300];
            fmt_hm(q.start, hm);
            snprintf(t, sizeof t, "%s  %s", hm, q.title);
            int lim = (right - nx) / (3 - k) - E(2);
            nx += d_text_mid(g, nx, y + E(1.0), 1.25, F_R, C_TEXT2, t, lim) + E(2.4);
        }
    }
    y += E(2.6);
    /* описание */
    if (T.info_expanded) {
        d_fill(g, x, y, w, 1, C_LINE_STRONG);
        y += E(1.2);
        int iw = w * 25 / 100, ih = exp_h - E(0.4), nwid = w * 27 / 100;
        /* TMDB (если задан ключ): картинка — после EPG, описание — если в EPG его нет */
        int tst = TMDB_OFF;
        const tmdb_result *tm = has ? tmdb_get(p.title, p.desc, &tst) : NULL;
        int shown = has && p.icon[0] && d_image(g, p.icon, x, y, iw, ih, E(0.6), 1);
        if (!shown && tm && tm->backdrop[0]) shown = d_image(g, tm->backdrop, x, y, iw, ih, E(0.6), 1);
        if (!shown && tm && tm->poster[0]) shown = d_image(g, tm->poster, x, y, iw, ih, E(0.6), 0);
        if (!shown) {
            d_round(g, x, y, iw, ih, E(0.6), C_FILL_SOFT);
            int lw = iw * 60 / 100, lh = lw * 66 / 96;
            d_logo(g, c, x + (iw - lw) / 2, y + (ih - lh) / 2, lw, lh, E(0.6), 0, 0);
        }
        int dx = x + iw + E(2), dw = w - iw - nwid - E(4);
        if (has) {
            char tags[300] = "";
            char a1[8], a2[8];
            fmt_hm(p.start, a1);
            fmt_hm(p.stop, a2);
            snprintf(tags, sizeof tags, "%s – %s%s%s%s%s", a1, a2, p.cat[0] ? "  ·  " : "", p.cat, p.sub[0] ? "  ·  " : "", p.sub);
            if (tm) {
                size_t tl = strlen(tags);
                if (tm->year[0]) snprintf(tags + tl, sizeof tags - tl, "  ·  %s", tm->year);
                tl = strlen(tags);
                if (tm->vote > 0) snprintf(tags + tl, sizeof tags - tl, "  ·  TMDB %.1f", tm->vote);
            }
            d_text(g, dx, y, 1.15, F_S, C_TEXT3, tags, dw);
            int lines = (ih - E(2.2)) / d_line_h(1.25, F_R);
            const char *desc = p.desc[0] ? p.desc : tm && tm->overview[0] ? tm->overview : tst == TMDB_LOADING ? "…" : p.title;
            d_lines(g, dx, y + E(2.2), dw, 1.25, F_R, C_TEXT2, desc, lines > 1 ? lines : 1, 0);
        }
        int nx = x + w - nwid;
        d_text(g, nx, y, 1.15, F_S, C_TEXT3, tr(S_NEXT), 0);
        int ly = y + E(2.2);
        for (int k = 1; k <= 4 && has; k++) {
            prog q = repo_prog(c, pi + k);
            if (!q.start) break;
            char hm[8];
            fmt_hm(q.start, hm);
            d_text(g, nx, ly, 1.2, F_S, C_TEXT2, hm, 0);
            d_text(g, nx + E(4.4), ly, 1.2, F_R, C_TEXT2, q.title, nwid - E(4.4));
            ly += E(2.4);
        }
        y += exp_h + E(1.2);
    }
    /* подсказки закреплённой панели */
    if (T.info_pinned && P.hints) {
        hint hs[6];
        int n = 0;
        hs[n++] = (hint){-1, tr(S_K_OK), T.paused ? tr(S_H_PLAY) : tr(S_H_PAUSE)};
        hs[n++] = (hint){I_DOWN, NULL, T.info_expanded ? tr(S_H_COLLAPSE) : tr(S_H_ABOUT)};
        if (T.info_expanded) hs[n - 1] = (hint){I_UP, NULL, tr(S_H_COLLAPSE)};
        hs[n++] = (hint){T.arc ? I_LEFT : I_RIGHT, NULL, T.arc || T.paused ? tr(S_H_SEEK) : tr(S_H_CHANNEL)};
        hs[n++] = (hint){-1, tr(S_K_MENU), tr(S_H_MENU)};
        d_hints(g, x, y + E(1.2), hs, n);
    }
}
