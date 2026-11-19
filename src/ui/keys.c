#include "keys.h"
#include "layer.h"
#include "tv.h"
#include "app.h"
#include <stdio.h>

#define LONG_PRESS 0.5
#define OK_LONG_PRESS 0.8
#define HOLD_START 0.35

int keys_map(int c) {
    switch (c) {
    case 105: return KEY_LEFT;
    case 106: return KEY_RIGHT;
    case 103: return KEY_UP;
    case 108: return KEY_DOWN;
    case 28: case 352: case 353: case 96: return KEY_OK;
    case 158: case 1: case 14: return KEY_BACK;
    case 102: case 172: case 174: return KEY_HOME;
    case 139: case 127: return KEY_MENU;
    case 358: return KEY_INFO;
    case 365: return KEY_GUIDE;
    case 164: return KEY_PLAYPAUSE;
    case 207: return KEY_PLAY;
    case 119: case 201: return KEY_PAUSE;
    case 128: case 166: return KEY_STOP;
    case 168: return KEY_REW;
    case 208: return KEY_FF;
    case 402: case 104: return KEY_CH_UP;
    case 403: case 109: return KEY_CH_DOWN;
    case 398: return KEY_RED;
    case 399: return KEY_GREEN;
    case 400: return KEY_YELLOW;
    case 401: return KEY_BLUE;
    case 388: return KEY_TEXT;
    case 395: case 405: return KEY_LIST;   /* LIST, LAST */
    case 364: return KEY_FAV;
    case 370: return KEY_SUBTITLE;
    case 392: return KEY_AUDIO;
    case 217: return KEY_SEARCH;
    case 11: return KEY_D0;
    default:
        if (c >= 2 && c <= 10) return KEY_D0 + (c - 1);
        if (c >= 512 && c <= 521) return KEY_D0 + (c - 512);   /* KEY_NUMERIC_0..9 */
        if (c >= 71 && c <= 82) {                               /* цифровой блок */
            static const int kp[] = {7, 8, 9, -1, 4, 5, 6, -1, 1, 2, 3, 0};
            int d = kp[c - 71];
            return d >= 0 ? KEY_D0 + d : KEY_NONE;
        }
        return KEY_NONE;
    }
}

static struct {
    int down;               /* нажатая клавиша */
    double at;              /* когда нажата */
    int pending;            /* ждём: отпускание — короткое, срок — долгое */
    int swallow;            /* после долгого — до отпускания ничего */
    int hold;               /* ↑/↓: свой повтор */
    double next;            /* следующий шаг повтора */
} K;

static int top_has_long(int k) { return layer_count() ? layer_top_has_long(k) : tv_has_long(k); }
static void deliver(int k, int ev) {
    if (layer_count()) layer_dispatch(k, ev);
    else tv_key(k, ev);
    app_dirty();
    app_activity();
}
static int no_hold(void) { layer *t = layer_top(); return t && t->no_hold; }

void keys_raw(int code, int value) {
    int k = keys_map(code);
    if (!k) return;
    if (k == KEY_HOME) { if (value == 1) { fprintf(stderr, "exit_key=home\n"); app_quit(); } return; }
    double now = app_now();
    if (value == 1) {
        if (K.down && K.pending && K.down != k) deliver(K.down, EV_SHORT);   /* вторая клавиша раньше отпускания первой */
        K.down = k;
        K.at = now;
        K.swallow = 0;
        K.hold = 0;
        if (top_has_long(k)) { K.pending = 1; return; }
        K.pending = 0;
        deliver(k, EV_SHORT);
        if ((k == KEY_UP || k == KEY_DOWN) && !no_hold()) { K.hold = 1; K.next = now + HOLD_START; }
        return;
    }
    if (value == 2) {
        if (k != K.down || K.swallow || K.pending || K.hold) return;
        /* повтор системы: стрелки ←/→ и CH± (перемотка, страницы); остальное — нет */
        if (k == KEY_LEFT || k == KEY_RIGHT || k == KEY_CH_UP || k == KEY_CH_DOWN) {
            if (no_hold() && (k == KEY_LEFT || k == KEY_RIGHT)) return;
            deliver(k, EV_REPEAT);
        }
        return;
    }
    if (value == 0 && k == K.down) {
        if (K.pending && !K.swallow) deliver(k, EV_SHORT);
        K.down = 0;
        K.pending = 0;
        K.hold = 0;
        K.swallow = 0;
    }
}

void keys_tick(double now) {
    if (!K.down) return;
    if (K.pending && !K.swallow && now - K.at >= (K.down == KEY_OK ? OK_LONG_PRESS : LONG_PRESS)) {
        K.pending = 0;
        K.swallow = 1;
        deliver(K.down, EV_LONG);
        return;
    }
    if (K.hold && now >= K.next) {
        /* шаг с ускорением: от 90 до 35 мс за 1,5 с удержания (интервал без времени шага) */
        double held = now - K.at - HOLD_START;
        double iv = 0.090 - (held > 1.5 ? 1.5 : held < 0 ? 0 : held) / 1.5 * 0.055;
        deliver(K.down, EV_REPEAT);
        K.next = app_now() + iv;
    }
}
