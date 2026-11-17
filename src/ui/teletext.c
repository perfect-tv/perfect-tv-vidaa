/* Телетекст во весь экран — как у Android-версии (docs/controls.md, «Телетекст»): страница 40 × 25 в области 4:3,
 * по умолчанию полупрозрачно поверх видео (чёрное — лёгкая вуаль, цвета почти непрозрачные, знаки с тенью).
 * Без клавиатуры: ← / → — предыдущая / следующая из принятых страниц, ↑ / ↓ — подстраницы, OK — клавиатура.
 * С клавиатурой (справа, как PIN «ряды»): ↑ / ↓ — ряд из шести, ← / OK / → — кнопка ряда; над ней круг d-pad с
 * кнопками ряда, под ней — диапазоны принятых страниц. Всегда: цифры — номер, цветные — Fastext, CH± — подстраницы,
 * TEXT — закрыть; удерж. OK или «Меню» — меню телетекста. Страница субтитров — по времени кадра. Подстраницы
 * листаются сами раз в 6 с. */
#include "screens.h"
#include "layer.h"
#include "keys.h"
#include "tv.h"
#include "u.h"
#include "app.h"
#include "core/lang.h"
#include "core/prefs.h"
#include "core/relay.h"
#include "core/ttx.h"
#include <stdio.h>
#include <string.h>

#define SUBPAGE_S 6.0
static const uint32_t COLORS[8] = {0xff000000u, 0xffff0000u, 0xff00ff00u, 0xffffff00u, 0xff0000ffu, 0xffff00ffu, 0xff00ffffu, 0xffffffffu};
/* клавиатура: 6 рядов по 3 кнопки */
enum { P_DIGIT, P_RED, P_GREEN, P_YELLOW, P_BLUE, P_DEL, P_PREV, P_IDX, P_NEXT };
static const struct { int kind, digit; } PAD[6][3] = {
    {{P_DIGIT, 1}, {P_DIGIT, 2}, {P_DIGIT, 3}}, {{P_DIGIT, 4}, {P_DIGIT, 5}, {P_DIGIT, 6}}, {{P_DIGIT, 7}, {P_DIGIT, 8}, {P_DIGIT, 9}},
    {{P_DIGIT, 0}, {P_RED, 0}, {P_DEL, 0}},         {{P_GREEN, 0}, {P_YELLOW, 0}, {P_BLUE, 0}}, {{P_PREV, 0}, {P_IDX, 0}, {P_NEXT, 0}},
};

static struct {
    int number, sub, nsub, reveal, hold, pad, row, flash_on, version, has, syncing;
    char typed[4];
    double opened, last_switch, flash_at, typed_at;
    ttx_page page;
    ttx_grid grid;
} TT;
static layer TL;

static void blank(ttx_page *p) { memset(p, 0, sizeof *p); p->number = (uint16_t)TT.number; }

/* страница субтитров идёт по времени кадра (плеер читает поток вперёд — последняя принятая опережает картинку) */
static int syncing(ttx *t) { return TT.number == ttx_subtitle_number(t) && ttx_has_subtitles(t) && tv_frame_pts() >= 0; }

static void load(void) {
    ttx *t = relay_ttx();
    uint8_t row0[TTX_COLS];
    TT.syncing = syncing(t);
    if (TT.syncing) {
        if (ttx_subtitle_at(t, tv_frame_pts(), &TT.page)) blank(&TT.page);
        TT.has = 1;
        TT.nsub = 1;
    } else {
        TT.has = !ttx_get(t, TT.number, TT.sub, &TT.page, &TT.nsub);
        if (TT.has && TT.sub >= TT.nsub) { TT.sub = 0; TT.has = !ttx_get(t, TT.number, 0, &TT.page, &TT.nsub); }
    }
    ttx_header_row(TT.number, TT.has ? &TT.page : NULL, TT.typed, row0);
    if (!TT.has) blank(&TT.page);
    ttx_layout(&TT.page, row0, &TT.grid);
    TT.version = ttx_version(t);
}

/* номера BCD 100…899: следующий / предыдущий подряд (шестнадцатеричные «цифры» пропускаются) */
static int bcd_next(int n) {
    int v = n + 1;
    if ((v & 0xF) > 9) v = (v & 0xFF0) + 0x10;
    if ((v & 0xF0) > 0x90) v = (v & 0xF00) + 0x100;
    return v > 0x899 ? 0x100 : v;
}
static int bcd_prev(int n) {
    int v = n - 1;
    if ((v & 0xF) > 9) v = (v & 0xFF0) + 9;
    if ((v & 0xF0) > 0x90) v = (v & 0xF00) + 0x99;
    return v < 0x100 ? 0x899 : v;
}
/* соседняя принятая страница по кругу; ничего не принято — соседний номер */
static int known(int d) {
    int n = ttx_neighbour(relay_ttx(), TT.number, d);
    return n ? n : d > 0 ? bcd_next(TT.number) : bcd_prev(TT.number);
}

static void go(int number) {
    if (number < 0x100 || number > 0x8FF) return;
    TT.number = number;
    TT.sub = 0;
    TT.typed[0] = 0;
    TT.last_switch = app_now();
    load();
    app_dirty();
}
static void subpage(int d) {
    if (TT.nsub < 2) return;
    TT.sub = (TT.sub + d + TT.nsub) % TT.nsub;
    TT.last_switch = app_now() + SUBPAGE_S;   /* после ручного листания само листать не спешим */
    load();
}
static void digit(int d) {
    size_t n = strlen(TT.typed);
    if (!n && (d < 1 || d > 8)) return;   /* страницы 100…899 */
    TT.typed[n] = (char)('0' + d);
    TT.typed[n + 1] = 0;
    TT.typed_at = app_now();
    if (n + 1 == 3) go((TT.typed[0] - '0') << 8 | (TT.typed[1] - '0') << 4 | d);
    else load();
}
static void link(int i) {
    int l = TT.has && !TT.syncing ? TT.page.links[i] : 0;
    if (l) go(l);
}

static void press(int r, int c) {
    switch (PAD[r][c].kind) {
    case P_DIGIT: digit(PAD[r][c].digit); break;
    case P_RED: link(0); break;
    case P_GREEN: link(1); break;
    case P_YELLOW: link(2); break;
    case P_BLUE: link(3); break;
    case P_DEL: { size_t n = strlen(TT.typed); if (n) { TT.typed[n - 1] = 0; load(); } break; }
    case P_PREV: go(known(-1)); break;
    case P_IDX: go(0x100); break;
    case P_NEXT: go(known(1)); break;
    }
}

/* ---- меню телетекста ---- */
static void ttx_menu_chosen(int i, void *c) {
    (void)c;
    if (i == 0) { P.ttx_keypad = !P.ttx_keypad; TT.pad = P.ttx_keypad; prefs_save(); }
    else if (i == 1) { P.ttx_mix = !P.ttx_mix; TL.covers = !P.ttx_mix; prefs_save(); }
    else if (i == 2) TT.reveal = !TT.reveal;
    else if (i == 3) { TT.hold = !TT.hold; TT.last_switch = app_now(); }
    else if (i == 4) layer_pop(&TL);
    app_dirty();
}
static void ttx_menu(void) {
    static const char *items[5];
    items[0] = tr(P.ttx_keypad ? S_TTX_KEYPAD_ON : S_TTX_KEYPAD_OFF);
    items[1] = tr(P.ttx_mix ? S_TTX_BG_MIX : S_TTX_BG_BLACK);
    items[2] = tr(TT.reveal ? S_TTX_REVEAL_ON : S_TTX_REVEAL_OFF);
    items[3] = tr(TT.hold ? S_TTX_SUBPAGES_HOLD : S_TTX_SUBPAGES_AUTO);
    items[4] = tr(S_TTX_CLOSE);
    choose_open(tr(S_TELETEXT), items, 5, 0, ttx_menu_chosen, NULL);
}

static int tt_has_long(layer *l, int key) { (void)l; return key == KEY_OK; }

static int tt_key(layer *l, int k, int ev) {
    (void)l;
    if (ev == EV_LONG) { if (k == KEY_OK) ttx_menu(); return 1; }
    if (IS_DIGIT(k)) { digit(k - KEY_D0); return 1; }
    switch (k) {
    case KEY_RED: link(0); return 1;
    case KEY_GREEN: link(1); return 1;
    case KEY_YELLOW: link(2); return 1;
    case KEY_BLUE: link(3); return 1;
    case KEY_CH_UP: subpage(1); return 1;
    case KEY_CH_DOWN: subpage(-1); return 1;
    case KEY_TEXT: layer_pop(&TL); return 1;
    case KEY_MENU: ttx_menu(); return 1;
    case KEY_BACK:
        if (TT.typed[0]) { TT.typed[0] = 0; load(); }
        else if (TT.pad) TT.pad = 0;   /* только скрыть: настройка не меняется */
        else layer_pop(&TL);
        return 1;
    }
    if (TT.pad) {
        if (k == KEY_UP) TT.row = (TT.row + 5) % 6;
        else if (k == KEY_DOWN) TT.row = (TT.row + 1) % 6;
        else if (k == KEY_LEFT) press(TT.row, 0);
        else if (k == KEY_RIGHT) press(TT.row, 2);
        else if (k == KEY_OK) press(TT.row, 1);
    } else {
        if (k == KEY_LEFT) go(known(-1));
        else if (k == KEY_RIGHT) go(known(1));
        else if (k == KEY_UP) subpage(-1);
        else if (k == KEY_DOWN) subpage(1);
        else if (k == KEY_OK) TT.pad = 1;
    }
    return 1;
}

static void tt_tick(layer *l, double now) {
    (void)l;
    ttx *t = relay_ttx();
    if (TT.typed[0] && now - TT.typed_at > 4) { TT.typed[0] = 0; load(); app_dirty(); }
    /* подстраницы листаются сами, как на телевизоре */
    if (TT.nsub > 1 && !TT.hold && now - TT.last_switch > SUBPAGE_S) { TT.sub = (TT.sub + 1) % TT.nsub; TT.last_switch = now; load(); app_dirty(); }
    /* мигание — раз в 0,5 с; субтитры — за кадром раз в 0,25 с */
    static double beat;
    if (now >= beat) {
        beat = now + 0.25;
        if (now - TT.flash_at >= 0.5) { TT.flash_at = now; TT.flash_on = !TT.flash_on; app_dirty(); }
        if (syncing(t) || TT.syncing) { load(); app_dirty(); }
        else if (!TT.has || TT.pad) app_dirty();   /* «ждём», список страниц */
    }
    if (ttx_version(t) != TT.version) { load(); app_dirty(); }
}

/* ---- рисование ---- */
static void mosaic(gfx_t *g, int x, int y, int w, int h, uint32_t code, int sep, uint32_t c) {
    static const int bit[6] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x40};
    int hw = w / 2, th = h / 3, gx = sep ? w / 12 : 0, gy = sep ? h / 12 : 0;
    for (int i = 0; i < 6; i++) {
        if (!(code & (uint32_t)bit[i])) continue;
        int bx = x + (i & 1) * hw, by = y + (i / 2) * th, bw = (i & 1) ? w - hw : hw, bh = i / 2 == 2 ? h - 2 * th : th;
        d_fill(g, bx + gx, by + gy, bw - 2 * gx, bh - 2 * gy, c);
    }
}

static void glyph(gfx_t *g, uint32_t code, int x, int y, int w, double em, uint32_t fg, int shadow) {
    char s[8];
    s[u8_put(s, code)] = 0;
    int tw = d_text_w(em, F_R, s), tx = x + (w - tw) / 2;
    if (shadow) d_text(g, tx + 2, y + 2, em, F_R, 0xcc000000u, s, 0);
    d_text(g, tx, y, em, F_R, fg, s, 0);
}

static void draw_page(gfx_t *g, int x0, int ch, int cw) {
    int mix = P.ttx_mix;
    double em = ch * 0.78 / E(1);
    for (int r = 0; r < TTX_ROWS; r++)
        for (int c = 0; c < TTX_COLS; c++) {
            int i = r * TTX_COLS + c, a = TT.grid.attr[i], x = x0 + c * cw, y = r * ch;
            int bg = (a >> 3) & 7;
            if (a & TA_DH_BOTTOM) continue;   /* фон и знак — ячейкой выше */
            int hh = (a & TA_DH) ? ch * 2 : ch;
            if (mix) d_fill(g, x, y, cw, hh, bg ? ((COLORS[bg] & 0xffffff) | 0xe0000000u) : 0x66000000u);
            else if (bg) d_fill(g, x, y, cw, hh, COLORS[bg]);
            if ((a & TA_CONCEAL) && !TT.reveal) continue;
            if ((a & TA_FLASH) && !TT.flash_on) continue;
            if (a & TA_DW_RIGHT) continue;
            uint32_t code = TT.grid.ch[i], fg = COLORS[a & 7];
            int ww = (a & TA_DW) ? cw * 2 : cw;
            if (a & TA_MOSAIC) { mosaic(g, x, y, ww, hh, code, a & TA_SEPARATED, fg); continue; }
            if (code == 0x20) continue;
            double e2 = (a & TA_DH) ? em * 1.9 : (a & TA_DW) ? em * 1.4 : em;
            glyph(g, code, x, y + (hh - d_line_h(e2, F_R)) / 2, ww, e2, fg, mix);
        }
}

static void draw_wheel(gfx_t *g, int x, int y, int w, int h) {
    float cx = x + w / 2.0f, cy = y + h / 2.0f, r = h / 2.0f - E(0.2), d = r * 0.7f;
    d_circle(g, cx, cy, r, WA(0.85));
    d_circle(g, cx, cy, r * 0.38f, C_POPUP);
    for (int c = 0; c < 3; c++) {
        float px = cx + (c - 1) * d;
        int kind = PAD[TT.row][c].kind;
        if (kind >= P_RED && kind <= P_BLUE) { d_circle(g, px, cy, r * (c == 1 ? 0.2f : 0.22f), COLORS[kind - P_RED + 1]); continue; }
        uint32_t col = c == 1 ? C_WHITE : 0xff101010u;
        double em = c == 1 ? 1.8 : 1.6;
        char s[8] = "";
        if (kind == P_DIGIT) snprintf(s, sizeof s, "%d", PAD[TT.row][c].digit);
        else if (kind == P_IDX) snprintf(s, sizeof s, "100");
        if (kind == P_DEL) d_icon(g, I_BACKSPACE, (int)px, (int)cy, E(1.6), col);
        else if (kind == P_PREV) d_icon(g, I_LEFT, (int)px, (int)cy, E(1.3), col);
        else if (kind == P_NEXT) d_icon(g, I_RIGHT, (int)px, (int)cy, E(1.3), col);
        else d_text_c(g, (int)px - E(3), E(6), (int)cy - d_line_h(em, F_B) / 2, em, F_B, col, s);
    }
    d_icon(g, I_UP, (int)cx, (int)(cy - d), E(1.1), 0xff101010u);
    d_icon(g, I_DOWN, (int)cx, (int)(cy + d), E(1.1), 0xff101010u);
}

/* диапазоны принятых страниц: «100–109  116  888», текущий — белым жирным */
static void draw_ranges(gfx_t *g, int x, int y, int w, int maxy) {
    static uint16_t nums[1000];
    int n = ttx_numbers(relay_ttx(), nums, 1000);
    if (!n) { d_lines(g, x, y, w, 1.1, F_R, C_TEXT3, tr(S_TTX_NO_PAGES), 2, 0.2); return; }
    int cx = x, lh = d_line_h(1.1, F_R) + E(0.2);
    for (int i = 0; i < n;) {
        int j = i;
        while (j + 1 < n && nums[j + 1] == bcd_next(nums[j])) j++;
        char s[16];
        if (i == j) snprintf(s, sizeof s, "%03X", nums[i]);
        else snprintf(s, sizeof s, "%03X–%03X", nums[i], nums[j]);
        int cur = TT.number >= nums[i] && TT.number <= nums[j];
        int tw = d_text_w(1.1, cur ? F_B : F_R, s);
        if (cx > x && cx + tw > x + w) { cx = x; y += lh; }
        if (y + lh > maxy) break;
        d_text(g, cx, y, 1.1, cur ? F_B : F_R, cur ? C_WHITE : C_TEXT3, s, 0);
        cx += tw + E(1.0);
        i = j + 1;
    }
}

static void draw_pad(gfx_t *g, int x, int y) {
    int kw = E(5.6), kh = E(4.2), gap = E(0.35), pad = E(0.7);
    int w = 3 * kw + 2 * gap + 2 * pad, wh = E(9.6);
    int h = pad + wh + E(0.5) + 6 * kh + 5 * gap + E(0.8) + E(5.4) + pad;
    d_round(g, x, y, w, h, E(1), C_POPUP);
    draw_wheel(g, x + pad, y + pad, w - 2 * pad, wh);
    int ky = y + pad + wh + E(0.5);
    for (int r = 0; r < 6; r++)
        for (int c = 0; c < 3; c++) {
            int bx = x + pad + c * (kw + gap), by = ky + r * (kh + gap), sel = r == TT.row, kind = PAD[r][c].kind;
            if (kind >= P_RED && kind <= P_BLUE) {
                d_round(g, bx, by, kw, kh, E(0.6), (COLORS[kind - P_RED + 1] & 0xffffff) | 0xc8000000u);
                if (sel) d_stroke(g, bx, by, kw, kh, E(0.6), E(0.3), 0xffffffffu);
                continue;
            }
            d_round(g, bx, by, kw, kh, E(0.6), sel ? C_WHITE : C_FILL_BTN);
            uint32_t fg = sel ? 0xff101010u : C_WHITE;
            int cx = bx + kw / 2, cy = by + kh / 2;
            if (kind == P_DEL) d_icon(g, I_BACKSPACE, cx, cy, E(1.6), fg);
            else if (kind == P_PREV) d_icon(g, I_LEFT, cx, cy, E(1.2), fg);
            else if (kind == P_NEXT) d_icon(g, I_RIGHT, cx, cy, E(1.2), fg);
            else {
                char s[8];
                if (kind == P_IDX) snprintf(s, sizeof s, "100");
                else snprintf(s, sizeof s, "%d", PAD[r][c].digit);
                double em = kind == P_IDX ? 1.3 : 1.6;
                d_text_c(g, bx, kw, cy - d_line_h(em, F_B) / 2, em, F_B, fg, s);
            }
        }
    int ry = ky + 6 * kh + 5 * gap + E(0.8);
    draw_ranges(g, x + pad, ry, w - 2 * pad, y + h - pad);
}

static void tt_draw(layer *l, gfx_t *g) {
    (void)l;
    if (!P.ttx_mix) d_clear(g, 0xff000000u);
    int ch = SH / TTX_ROWS, cw = ch * 4 / 3 * TTX_ROWS / TTX_COLS;   /* область 4:3 на всю высоту */
    int pw = cw * TTX_COLS;
    int x0 = TT.pad ? E(1) : (SW - pw) / 2;
    draw_page(g, x0, ch, cw);
    if (!TT.has) {
        ttx *t = relay_ttx();
        char num[8];
        snprintf(num, sizeof num, "%03X", TT.number);
        const char *msg;
        if (relay_has_teletext()) msg = ttx_count(t) ? trf(S_TTX_NO_PAGE, num) : tr(S_TTX_WAIT);
        else if (app_now() - TT.opened > 8 && relay_ts_bytes() > 0) msg = tr(relay_has_dvbsub() ? S_TTX_DVB_ONLY : S_TTX_NONE);
        else msg = tr(S_TTX_WAIT);
        int mw = d_text_w(1.5, F_S, msg) + E(3);
        if (mw > pw - E(2)) mw = pw - E(2);
        d_round(g, x0 + (pw - mw) / 2, SH / 2 - E(2), mw, E(4), E(0.8), 0xd0000000u);
        d_text_c(g, x0 + (pw - mw) / 2, mw, SH / 2 - E(2) + (E(4) - d_line_h(1.5, F_S)) / 2, 1.5, F_S, C_WHITE, msg);
    }
    if (TT.pad) {
        int pw2 = 3 * E(5.6) + 2 * E(0.35) + 2 * E(0.7);
        int ph = E(0.7) + E(9.6) + E(0.5) + 6 * E(4.2) + 5 * E(0.35) + E(0.8) + E(5.4) + E(0.7);
        draw_pad(g, SW - pw2 - E(1.2), (SH - ph) / 2);
    }
}

void teletext_open(void) {
    TL.name = "teletext";
    TL.key = tt_key;
    TL.has_long = tt_has_long;
    TL.tick = tt_tick;
    TL.draw = tt_draw;
    TL.covers = !P.ttx_mix;   /* поверх видео — видео и «дыра» под страницей */
    memset(&TT, 0, sizeof TT);
    TT.number = 0x100;
    TT.pad = P.ttx_keypad;
    TT.row = 1;   /* «4 5 6» */
    TT.flash_on = 1;
    TT.opened = TT.last_switch = TT.flash_at = app_now();
    load();
    layer_push(&TL);
}
