/* Инфопанель (docs/design.md, «Инфопанель») — раскладка Android-версии (info_panel.xml, InfoPanel.kt), размеры в em
 * (10 dp = 1 em): затемнение во всю ширину снизу с плавным верхом 5,6 em, содержимое — в безопасной зоне (4 em по
 * бокам, 2,4 em снизу). Слева логотип, справа от него колонка: канал и бейджи, передача, шкала с временем начала и
 * конца по бокам и «ещё N мин» (в архиве — время кадра), «Далее: …» (при перемотке на её месте — пузырь с целевым
 * временем под маркером шкалы); часы — справа сверху. Раскрытая (↓↓) — под полосой картинка, теги, описание и
 * «Далее на канале»; закреплённая — строка подсказок клавиш. */
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

#define SLOT 1800   /* без программы шкала — получасовой слот вокруг позиции */

static int panel_top;
int info_top(void) { return panel_top ? panel_top : SH - E(20); }

static int quality_badge(char *out, size_t cap) {
    int w, h;
    if (pl_video_size(&w, &h)) return 0;
    snprintf(out, cap, "%s", h >= 2000 ? "4K" : h >= 1000 ? "FHD" : h >= 700 ? "HD" : "SD");
    return 1;
}

static void hms(int64_t t, char *out, size_t cap) {
    ltm m = local_tm(t);
    snprintf(out, cap, "%02d:%02d:%02d", m.h, m.m, m.s);
}

static double frac_of(int64_t from, int64_t to, int64_t t) {
    if (to <= from) return 0;
    double f = (double)(t - from) / (double)(to - from);
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

/* «Сегодня, 16:00 – 16:45» */
static void day_range(int64_t s, int64_t e, char *out, size_t cap) {
    char d[32], a[8], b[8];
    fmt_day(s, d, sizeof d);
    fmt_hm(s, a);
    fmt_hm(e, b);
    snprintf(out, cap, "%s, %s – %s", d, a, b);
}

/* Шкала (InfoBar): полоса 0,35 em по центру области высотой 1,6 em; значение — белое (архив — янтарное, при
 * перемотке приглушено), красная черта «эфир» в архиве идущей передачи, маркер перемотки — на всю высоту. */
static void draw_bar(gfx_t *g, int x, int cy, int w, double value, double live, double seek, int arch) {
    int h = E(1.6), top = cy - h / 2, t = E(0.35), ty = cy - t / 2;
    d_round(g, x, ty, w, t, t / 2, C_FILL_MARKED);
    uint32_t on = arch ? C_ARCHIVE : C_WHITE;
    if (seek >= 0) on = (on & 0x00ffffffu) | 0x99000000u;
    if (value > 0) d_round(g, x, ty, (int)(w * value), t, t / 2, on);
    if (live >= 0) {
        int half = E(0.12) > 1 ? E(0.12) : 1, lx = x + (int)(w * live), edge = E(0.1);
        if (lx < x + half) lx = x + half;
        if (lx > x + w - half) lx = x + w - half;
        if (seek >= 0) d_round(g, lx - half, top + edge, half * 2, h - 2 * edge, half, C_LIVE);
        else d_round(g, lx - half, ty, half * 2, t, half, C_LIVE);
    }
    if (seek >= 0) {
        int half = E(0.25), edge = E(0.1) > 1 ? E(0.1) : 1, mx = x + (int)(w * seek);
        if (mx < x + half) mx = x + half;
        if (mx > x + w - half) mx = x + w - half;
        d_round(g, mx - half - edge, top, (half + edge) * 2, h, half, C_PANEL | 0xff000000u);
        d_round(g, mx - half, top + edge, half * 2, h - 2 * edge, half, C_WHITE);
    }
}

/* Пузырь под шкалой (перемотка — янтарный, «Эфир» — красный): по центру под долей f шкалы, в пределах колонки */
static int bubble_x(int bar_x, int bar_w, double f, int bw, int l, int r) {
    int bx = bar_x + (int)(bar_w * f) - bw / 2;
    if (bx > r - bw) bx = r - bw;
    if (bx < l) bx = l;
    return bx;
}
static void bubble(gfx_t *g, int bx, int cy, uint32_t bg, uint32_t fg, const char *t) {
    int bw = d_text_w(1.3, F_B, t) + E(1.6), bh = d_line_h(1.3, F_B) + E(0.3);
    d_round(g, bx, cy - bh / 2, bw, bh, E(0.6), bg);
    d_text_mid(g, bx + E(0.8), cy, 1.3, F_B, fg, t, 0);
}
static int bubble_w(const char *t) { return d_text_w(1.3, F_B, t) + E(1.6); }

/* бейджи после названия канала (порядок и цвета — InfoPanel.badgesOf); x — начало, max — правый край */
static void draw_badges(gfx_t *g, int x, int cy, int arch) {
    const double em = 1.1;
    const int gap = E(0.6);
    char b[64], d[32];
    if (T.failed) x += d_badge(g, x, cy, em, C_LIVE, C_WHITE, -1, tr(S_ERROR_BADGE)) + gap;
    if (T.paused) x += d_badge(g, x, cy, em, C_FILL_ACTIVE, C_WHITE, I_PAUSE, NULL) + gap;
    if (arch) {
        int64_t lag = tv_behind();
        if (lag > 0) { fmt_dur(lag, d, sizeof d); snprintf(b, sizeof b, "%s · −%s", tr(S_ARCHIVE), d); }
        else snprintf(b, sizeof b, "%s", tr(S_ARCHIVE));
        x += d_badge(g, x, cy, em, C_ARCHIVE, C_ON_ARCHIVE, -1, b) + gap;
    } else x += d_badge(g, x, cy, em, C_LIVE, C_WHITE, -1, tr(S_LIVE)) + gap;
    if (quality_badge(b, sizeof b)) x += d_badge(g, x, cy, em, C_FILL_ACTIVE, C_WHITE, -1, b) + gap;
    if (pl_software() && pl_has_picture()) x += d_badge(g, x, cy, em, C_FILL_ACTIVE, C_WHITE, -1, "SW") + gap;
    if (pl_audio_count() > 1) { snprintf(b, sizeof b, tr(S_AUDIO_N), pl_audio_count()); x += d_badge(g, x, cy, em, C_FILL_ACTIVE, C_WHITE, -1, b) + gap; }
    /* субтитры: включённые — белым */
    if (tv_has_subtitles()) x += d_badge(g, x, cy, em, T.subs ? C_WHITE : C_FILL_ACTIVE, T.subs ? 0xff101010u : C_WHITE, -1, "SUB") + gap;
    if (tv_has_teletext()) x += d_badge(g, x, cy, em, C_FILL_ACTIVE, C_WHITE, -1, "TTX") + gap;
    if (repo_has_archive(T.cur)) d_badge(g, x, cy, em, C_FILL_ACTIVE, C_WHITE, I_ARCHIVE, trp(S_DAYS_SHORT, T.cur->archive_days));
}

/* раскрытое описание: картинка 25 % ширины (16:9), теги и текст, «Далее на канале» 27 % */
static void draw_more(gfx_t *g, int x, int y, int cw, int bh, channel *c, int has, const prog *p, int pi) {
    int iw = cw * 25 / 100;
    /* TMDB (если задан ключ): картинка — TMDB, затем EPG; описание — если в EPG его нет или совпадение однозначно */
    int tst = TMDB_OFF;
    const tmdb_result *tm = has ? tmdb_get(p->title, p->desc, &tst) : NULL;
    int shown = 0;
    if (tm && tm->backdrop[0]) shown = d_image(g, tm->backdrop, x, y, iw, bh, E(0.6), 1);
    if (!shown && tm && tm->poster[0]) shown = d_image(g, tm->poster, x, y, iw, bh, E(0.6), 1);
    if (!shown && has && p->icon[0]) {
        d_round(g, x, y, iw, bh, E(0.6), C_FILL_SOFT);
        shown = d_image(g, p->icon, x, y, iw, bh, E(0.6), 1);
    }
    if (!shown) d_logo(g, c, x, y, iw, bh, E(0.6), 0, 0);   /* логотип со своей подложкой на всю область */
    /* «Далее на канале»: до 4 передач после идущей */
    prog up[4];
    int nup = 0;
    for (int k = 1; k <= 4 && pi >= 0; k++) {
        prog q = repo_prog(c, pi + k);
        if (!q.start) break;
        up[nup++] = q;
    }
    if (pi < 0) {
        int j = repo_prog_after(c, tv_position());
        for (int k = 0; k < 4 && j >= 0; k++) {
            prog q = repo_prog(c, j + k);
            if (!q.start) break;
            up[nup++] = q;
        }
    }
    int uw = nup ? cw * 27 / 100 : 0;
    int dx = x + iw + E(1.6), dw = x + cw - (nup ? uw + E(2.0) : 0) - dx;
    char tags[700] = "";
    if (has) {
        day_range(p->start, p->stop, tags, sizeof tags);
        size_t tl = strlen(tags);
        if (p->cat[0]) tl += (size_t)snprintf(tags + tl, sizeof tags - tl, " · %s", p->cat);
        if (p->sub[0] && tl < sizeof tags) tl += (size_t)snprintf(tags + tl, sizeof tags - tl, " · %s", p->sub);
        if (tm && tm->year[0] && tl < sizeof tags) snprintf(tags + tl, sizeof tags - tl, " · %s", tm->year);
    } else snprintf(tags, sizeof tags, "%s", tr(S_NO_PROGRAMME));
    d_text(g, dx, y, 1.2, F_R, WA(0.65), tags, dw);
    if (has && tm && tm->vote > 0) {
        /* ★ в шрифтах нет — значок I_STAR, число рядом */
        int tw = d_text_w(1.2, F_R, tags), sz = E(1.0), sx = dx + tw + E(0.7);
        if (sx + sz + E(2.5) < dx + dw) {
            d_icon(g, I_STAR, sx + sz / 2, y + d_line_h(1.2, F_R) / 2, sz, WA(0.65));
            char v[16];
            snprintf(v, sizeof v, "%.1f", tm->vote);
            int rx = sx + sz + E(0.35);
            d_text(g, rx, y, 1.2, F_R, WA(0.65), v, dw - (rx - dx));
        }
    }
    if (has) {
        const char *desc = tm && tm->overview[0] && (!p->desc[0] || tm->unique) ? tm->overview : p->desc[0] ? p->desc : tst == TMDB_LOADING ? "…" : "";
        double gap = 1.3 * 0.12;
        int ty = y + d_line_h(1.2, F_R) + E(0.6), lh = d_line_h(1.3, F_R) + E(gap);
        int lines = (y + bh - ty) / lh;
        if (lines < 2) lines = 2;
        if (*desc) d_lines(g, dx, ty, dw, 1.3, F_R, WA(0.85), desc, lines, gap);
    }
    if (nup) {
        int ux = x + cw - uw, uy = y;
        d_text(g, ux, uy, 1.15, F_R, WA(0.50), tr(S_NEXT_ON_CHANNEL), uw);
        uy += d_line_h(1.15, F_R) + E(0.3);
        for (int k = 0; k < nup; k++) {
            uy += E(0.5);
            int rh = d_line_h(1.3, F_R), cy = uy + rh / 2;
            char hm[8];
            fmt_hm(up[k].start, hm);
            d_text_mid(g, ux, cy, 1.25, F_R, WA(0.55), hm, E(4.4));
            d_text_mid(g, ux + E(4.4), cy, 1.3, F_R, WA(0.90), up[k].title, uw - E(4.4));
            uy += rh;
        }
    }
}

void info_draw(gfx_t *g) {
    channel *c = T.cur;
    if (!c) return;
    int64_t now = clock_now(), position = tv_position(), ref = T.seeking ? T.seek_to : position;
    int arch = T.arc || (T.paused && tv_behind() > 3);
    int x = E(4), right = SW - E(4), cw = right - x;
    int pi = repo_prog_at(c, ref);
    prog p = repo_prog(c, pi);
    int has = pi >= 0;
    /* высоты: основная часть 10,3 em; описание — полоса и картинка 16:9 в четверть ширины; подсказки 3 em */
    int main_h = E(10.3), banner_h = cw * 25 / 100 * 9 / 16;
    int h = main_h;
    if (T.info_expanded) h += E(2.4) + 1 + banner_h;
    int hints = T.info_pinned && P.hints;
    if (hints) h += E(3.0);
    int ct = SH - E(2.4) - h;   /* верх содержимого */
    int fade = E(5.6), top = ct - fade;
    panel_top = top;
    /* затемнение: к началу содержимого — уже 94 % плотности */
    uint32_t sc = C_SCRIM & 0x00ffffffu, sa = C_SCRIM >> 24;
#define SA(k) (sc | ((uint32_t)(sa * (k) + 0.5) << 24))
    d_vgrad(g, 0, top, SW, fade / 2, SA(0.0), SA(0.55));
    d_vgrad(g, 0, top + fade / 2, SW, fade - fade / 2, SA(0.55), SA(0.94));
    d_vgrad(g, 0, ct, SW, SH - ct, SA(0.94), SA(1.0));
#undef SA
    /* логотип; часы справа сверху */
    d_logo(g, c, x, ct + E(0.2), E(9.6), E(6.6), E(0.6), 1, 0);
    char clock[8];
    fmt_hm(now, clock);
    int clock_w = d_text_w(2.4, F_R, clock);
    d_text_r(g, right, ct, 2.4, F_R, WA(0.95), clock);
    int tx = x + E(11.0), tr_ = right - clock_w - E(1.6), tw = tr_ - tx;
    /* канал и бейджи */
    char name[300];
    if (P.number_mode == NUM_NEAR_NAME) snprintf(name, sizeof name, "%d. %s", c->num, c->name);
    else snprintf(name, sizeof name, "%s", c->name);
    int ny = ct + E(1.1);
    int nw = d_text_mid(g, tx, ny, 1.4, F_R, WA(0.80), name, E(34) < tw ? E(34) : tw);
    draw_badges(g, tx + nw + E(1.0), ny, arch);
    /* передача (без программы: архив — «Архив 21:40», эфир — «Нет программы») */
    char title[300];
    if (has) snprintf(title, sizeof title, "%s", p.title);
    else if (arch || T.seeking) { char hm[8]; fmt_hm(ref, hm); snprintf(title, sizeof title, "%s %s", tr(S_ARCHIVE), hm); }
    else snprintf(title, sizeof title, "%s", tr(S_NO_PROGRAMME));
    d_text_mid(g, tx, ct + E(2.4) + E(1.45), 2.2, F_B, C_WHITE, title, tw);
    /* шкала: передача, а без неё — получасовой слот вокруг позиции */
    int64_t from = has ? p.start : ref - ((ref % SLOT) + SLOT) % SLOT, to = has ? p.stop : from + SLOT;
    char a1[8], a2[8], remain[64] = "";
    fmt_hm(from, a1);
    fmt_hm(to, a2);
    if (T.seeking) remain[0] = 0;
    else if (arch) hms(position, remain, sizeof remain);
    else if (has && p.stop > position) {
        long min = (long)((p.stop - position + 59) / 60);
        if (min < 60) snprintf(remain, sizeof remain, "%s", trp(S_MIN_LEFT, min));
        else snprintf(remain, sizeof remain, "%s", trf(S_H_MIN_LEFT, (int)(min / 60), (int)(min % 60)));
    }
    int by = ct + E(5.8) + E(0.9);
    int sw = d_text_mid(g, tx, by, 1.25, F_R, WA(0.75), a1, 0);
    int rw = remain[0] ? d_text_w(1.3, F_B, remain) : 0;
    if (rw) d_text_mid(g, tr_ - rw, by, 1.3, F_B, C_WHITE, remain, 0);
    int ew = d_text_w(1.25, F_R, a2);
    int ex = tr_ - rw - E(1.2) - ew;
    d_text_mid(g, ex, by, 1.25, F_R, WA(0.75), a2, 0);
    int bar_x = tx + sw + E(0.9), bar_w = ex - E(0.9) - bar_x;
    double live = arch && to > from && now >= from && now <= to ? frac_of(from, to, now) : -1;
    double seek = T.seeking ? frac_of(from, to, T.seek_to) : -1;
    draw_bar(g, bar_x, by, bar_w, frac_of(from, to, position), live, seek, arch);
    /* «Далее: …» или пузырь перемотки (и «Эфир» под красной чертой) */
    int nyc = ct + E(8.1) + E(1.1);
    if (T.seeking) {
        char hm[16], bh[32], txt[64];
        hms(T.seek_to, hm, sizeof hm);
        int64_t diff = T.seek_to - now;
        fmt_dur(diff < 0 ? -diff : diff, bh, sizeof bh);
        snprintf(txt, sizeof txt, "%s   %s%s", hm, diff < 0 ? "−" : "+", bh);
        if (live >= 0) {
            const char *lt = tr(S_LIVE);
            bubble(g, bubble_x(bar_x, bar_w, live, bubble_w(lt), tx, tr_), nyc, C_LIVE, C_WHITE, lt);
        }
        bubble(g, bubble_x(bar_x, bar_w, seek, bubble_w(txt), tx, tr_), nyc, C_ARCHIVE, C_ON_ARCHIVE, txt);
    } else {
        int nx = tx + d_text_mid(g, tx, nyc, 1.3, F_R, WA(0.5), tr(S_NEXT_COLON), 0);
        int k0 = has ? pi + 1 : repo_prog_after(c, ref);
        prog n1 = k0 >= 0 ? repo_prog(c, k0) : (prog){0};
        char t[700];
        if (!n1.start) snprintf(t, sizeof t, "%s", tr(S_NO_PROGRAMME_LC));
        else {
            char h1[8];
            fmt_hm(n1.start, h1);
            int len = snprintf(t, sizeof t, "%s %s", h1, n1.title);
            prog n2 = repo_prog(c, k0 + 1);
            if (n2.start && len > 0 && (size_t)len < sizeof t) {
                char h2[8];
                fmt_hm(n2.start, h2);
                snprintf(t + len, sizeof t - (size_t)len, "   ·   %s %s", h2, n2.title);
            }
        }
        d_text_mid(g, nx, nyc, 1.3, F_R, WA(0.85), t, tr_ - nx);
    }
    int y = ct + main_h;
    /* описание */
    if (T.info_expanded) {
        y += E(1.2);
        d_fill(g, x, y, cw, 1, C_LINE_STRONG);
        y += 1 + E(1.2);
        draw_more(g, x, y, cw, banner_h, c, has, &p, pi);
        y += banner_h;
    }
    /* подсказки закреплённой панели */
    if (hints) {
        hint hs[6];
        int n = 0;
        hs[n++] = (hint){-1, tr(S_K_OK), T.paused ? tr(S_H_PLAY) : tr(S_H_PAUSE)};
        if (T.info_expanded) hs[n++] = (hint){-1, "↑", tr(S_H_COLLAPSE)};
        else {
            hs[n++] = (hint){-1, "↓", tr(S_H_ABOUT)};
            hs[n++] = (hint){-1, "↑", tr(S_H_CLOSE)};
        }
        hs[n++] = (hint){-1, "← →", arch ? tr(S_H_SEEK) : tr(S_H_CHANNEL)};
        hs[n++] = (hint){-1, tr(S_K_HOLD_OK), tr(S_H_MENU)};
        d_hints(g, x, y + E(1.2) + E(0.9), hs, n);
    }
}
