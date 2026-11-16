/* Окно PIN (docs/controls.md, «Окно PIN»): кольцо (↑→↓← = 1–4, OK = 0, «Меню» или удерж. OK — 5–9),
 * ряды (←/OK/→ — цифры ряда, ↑/↓ — ряд, на 7·8·9 ↓ = 0) или клавиатура; цифры пульта — всегда.
 * Видны только точки, длина PIN не видна; верный PIN принимается сразу, 6 цифр без совпадения — «Неверный PIN».
 * Новый PIN — клавиатурой с «Готово», дважды. */
#include "screens.h"
#include "layer.h"
#include "keys.h"
#include "tv.h"
#include "u.h"
#include "app.h"
#include "core/lang.h"
#include "core/prefs.h"
#include <stdio.h>
#include <string.h>

static struct {
    int create, step;         /* create: 0 — первый ввод, 1 — повтор */
    char in[8], first[8];
    int mode, row, alt, kx, ky;
    char msg[120];
    double msg_until;
    pin_fn ok, cancel;
    void *ctx;
} N;
static layer L;

static void finish(int success) {
    pin_fn f = success ? N.ok : N.cancel;
    void *ctx = N.ctx;
    layer_pop(&L);
    if (f) f(ctx);
}
static void flash(int id) { snprintf(N.msg, sizeof N.msg, "%s", tr(id)); N.msg_until = app_now() + 2; }

static void submit(void) {   /* «Готово» при создании */
    size_t n = strlen(N.in);
    if (!N.create) return;
    if (n < 4) return;
    if (N.step == 0) { snprintf(N.first, sizeof N.first, "%s", N.in); N.in[0] = 0; N.step = 1; return; }
    if (strcmp(N.first, N.in)) { flash(S_PIN_MISMATCH); N.in[0] = 0; N.step = 0; return; }
    snprintf(P.pin, sizeof P.pin, "%s", N.in);
    prefs_save();
    tv_toast(tr(S_PIN_SET));
    finish(1);
}

static void put(int d) {
    size_t n = strlen(N.in);
    if (n >= 6) return;
    N.in[n] = (char)('0' + d);
    N.in[n + 1] = 0;
    if (N.create) return;
    if (!strcmp(N.in, P.pin)) { finish(1); return; }
    if (n + 1 >= 6) { flash(S_PIN_WRONG); N.in[0] = 0; }
}
static void erase(void) {
    size_t n = strlen(N.in);
    if (!n) { finish(0); return; }
    N.in[n - 1] = 0;
}

/* клавиатура 3×4: 1-9, ⌫ 0 ✓ (✓ — только при создании) */
static void keypad_press(void) {
    int idx = N.ky * 3 + N.kx;
    if (idx < 9) put(idx + 1);
    else if (idx == 9) erase();
    else if (idx == 10) put(0);
    else submit();
}

static int has_long(layer *l, int k) { (void)l; return k == KEY_OK && N.mode == PIN_IN_RING; }

static int key(layer *l, int k, int ev) {
    (void)l;
    if (IS_DIGIT(k)) { if (ev == EV_SHORT) put(k - KEY_D0); return 1; }
    if (k == KEY_BACK) { if (ev == EV_SHORT) erase(); return 1; }
    if (ev == EV_REPEAT) return 1;
    if (N.mode == PIN_IN_KEYS) {
        if (k == KEY_LEFT) N.kx = (N.kx + 2) % 3;
        else if (k == KEY_RIGHT) N.kx = (N.kx + 1) % 3;
        else if (k == KEY_UP) N.ky = (N.ky + 3) % 4;
        else if (k == KEY_DOWN) N.ky = (N.ky + 1) % 4;
        else if (k == KEY_OK) keypad_press();
        if (!N.create && N.ky == 3 && N.kx == 2) N.kx = 1;
        return 1;
    }
    if (N.mode == PIN_IN_RING) {
        int base = N.alt ? 5 : 1;
        if (k == KEY_MENU || (k == KEY_OK && ev == EV_LONG)) { N.alt = !N.alt; return 1; }
        if (k == KEY_UP) put(base);
        else if (k == KEY_RIGHT) put(base + 1);
        else if (k == KEY_DOWN) put(base + 2);
        else if (k == KEY_LEFT) put(base + 3);
        else if (k == KEY_OK) put(N.alt ? 9 : 0);
        return 1;
    }
    /* ряды */
    if (k == KEY_UP) N.row = (N.row + 2) % 3;
    else if (k == KEY_DOWN) { if (N.row == 2) put(0); else N.row++; }
    else if (k == KEY_LEFT) put(N.row * 3 + 1);
    else if (k == KEY_OK) put(N.row * 3 + 2);
    else if (k == KEY_RIGHT) put(N.row * 3 + 3);
    return 1;
}

static void draw(layer *l, gfx_t *g) {
    (void)l;
    int w = E(40), h = E(30), x = (SW - w) / 2, y = (SH - h) / 2;
    d_fill(g, 0, 0, SW, SH, C_OVERLAY);
    d_round(g, x, y, w, h, E(1), C_POPUP);
    const char *title = N.create ? tr(N.step ? S_PIN_REPEAT : S_PIN_NEW) : tr(S_PIN_ENTER);
    d_text_c(g, x, w, y + E(1.6), 1.5, F_B, C_WHITE, title);
    /* точки */
    int n = (int)strlen(N.in), dr = E(0.55), cy = y + E(5.6);
    int total = n ? n : 1, dw = total * E(2) - E(0.9), dx = x + (w - dw) / 2;
    if (!n) d_circle(g, (float)(x + w / 2), (float)cy, (float)dr, WA(0.2));
    for (int i = 0; i < n; i++) d_circle(g, (float)(dx + dr + i * E(2)), (float)cy, (float)dr, C_WHITE);
    if (N.msg[0] && app_now() < N.msg_until) d_text_c(g, x, w, y + E(7.2), 1.15, F_S, C_LIVE, N.msg);
    int ky0 = y + E(10);
    if (N.mode == PIN_IN_KEYS) {
        int bw = E(6), bh = E(3.4), gap = E(0.6), gx = x + (w - 3 * bw - 2 * gap) / 2;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 3; c++) {
                int idx = r * 3 + c;
                if (idx == 11 && !N.create) continue;
                int bx = gx + c * (bw + gap), by = ky0 + r * (bh + gap), foc = N.kx == c && N.ky == r;
                d_round(g, bx, by, bw, bh, E(0.4), foc ? C_WHITE : C_FILL_BTN);
                uint32_t fg = foc ? 0xff101010u : C_WHITE;
                if (idx < 9 || idx == 10) { char t[4]; snprintf(t, sizeof t, "%d", idx < 9 ? idx + 1 : 0); d_text_c(g, bx, bw, by + (bh - d_line_h(1.5, F_S)) / 2, 1.5, F_S, fg, t); }
                else d_icon(g, idx == 9 ? I_BACKSPACE : I_CHECK, bx + bw / 2, by + bh / 2, E(1.6), fg);
            }
        return;
    }
    if (N.mode == PIN_IN_RING) {
        int cx = x + w / 2, ccy = ky0 + E(7), rr = E(6);
        d_circle(g, (float)cx, (float)ccy, (float)rr, C_FILL_BTN);
        d_circle(g, (float)cx, (float)ccy, (float)E(2.4), C_FILL_MARKED);
        int base = N.alt ? 5 : 1;
        char t[4];
        int pos[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
        for (int i = 0; i < 4; i++) {
            snprintf(t, sizeof t, "%d", base + i);
            d_text_c(g, cx + pos[i][0] * E(4.2) - E(2), E(4), ccy + pos[i][1] * E(4.2) - d_line_h(1.5, F_S) / 2, 1.5, F_S, C_WHITE, t);
        }
        snprintf(t, sizeof t, "%d", N.alt ? 9 : 0);
        d_text_c(g, cx - E(2), E(4), ccy - d_line_h(1.5, F_S) / 2, 1.5, F_S, C_WHITE, t);
        d_text_c(g, x, w, y + h - E(2.8), 1.05, F_R, C_TEXT3, tr(S_PIN_HINT_RING));
        return;
    }
    /* ряды: справа все цифры, ряд выделен; 0 — под рядом 7·8·9 («Т») */
    int bw = E(5), bh = E(3.2), gap = E(0.5), gx = x + (w - 3 * bw - 2 * gap) / 2;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 3; c++) {
            if (r == 3 && c != 1) continue;
            int d = r == 3 ? 0 : r * 3 + c + 1;
            int bx = gx + c * (bw + gap), by = ky0 + r * (bh + gap);
            int sel = r == N.row || (r == 3 && N.row == 2);
            d_round(g, bx, by, bw, bh, E(0.4), sel ? C_FILL_ACTIVE : C_FILL);
            char t[4];
            snprintf(t, sizeof t, "%d", d);
            d_text_c(g, bx, bw, by + (bh - d_line_h(1.4, F_S)) / 2, 1.4, F_S, sel ? C_WHITE : C_TEXT3, t);
        }
    int fy = ky0 + N.row * (bh + gap);
    d_stroke(g, gx - E(0.3), fy - E(0.3), 3 * bw + 2 * gap + E(0.6), bh + E(0.6), E(0.6), E(0.2), C_WHITE);
    if (N.row == 2) d_stroke(g, gx + bw + gap - E(0.3), fy + bh + gap - E(0.3), bw + E(0.6), bh + E(0.6), E(0.6), E(0.2), C_WHITE);
    d_text_c(g, x, w, y + h - E(2.8), 1.05, F_R, C_TEXT3, tr(S_PIN_HINT_ROWS));
}

static void open(int create, pin_fn ok, pin_fn cancel, void *ctx) {
    L.name = "pin";
    L.key = key;
    L.has_long = has_long;
    L.draw = draw;
    L.no_hold = 1;
    memset(&N, 0, sizeof N);
    N.create = create;
    N.mode = create ? PIN_IN_KEYS : P.pin_input;
    N.ok = ok;
    N.cancel = cancel;
    N.ctx = ctx;
    layer_push(&L);
}
void pin_ask(pin_fn ok, pin_fn cancel, void *ctx) {
    if (!P.pin[0]) { if (ok) ok(ctx); return; }
    open(0, ok, cancel, ctx);
}
void pin_create(pin_fn done, void *ctx) { open(1, done, NULL, ctx); }
