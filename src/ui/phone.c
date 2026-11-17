/* Окно «Ввод с телефона»: QR-код адреса одноразовой страницы (core/phone.c), адрес текстом и состояние.
 * Пришёл ответ — окно закрывается и отдаёт его вызвавшему (форма плейлиста). Истёк срок — OK выдаёт новый код. */
#include "screens.h"
#include "layer.h"
#include "keys.h"
#include "tv.h"
#include "u.h"
#include "app.h"
#include "core/lang.h"
#include "core/phone.h"
#include "core/qr.h"
#include <stdio.h>
#include <string.h>

static struct {
    char url[200], err[120];
    uint8_t m[QR_SIZE][QR_SIZE];
    int ok;                  /* код показан */
    phone_fn done;
    void *ctx;
} PH;
static layer PHL;

static void start(void) {
    char e[100] = "";
    PH.ok = 0;
    PH.err[0] = 0;
    if (phone_start(PH.url, sizeof PH.url, e, sizeof e)) {
        snprintf(PH.err, sizeof PH.err, "%s", !strcmp(e, "no_network") ? tr(S_PHONE_NO_NET) : trf(S_PHONE_FAILED, e));
        return;
    }
    PH.ok = !qr_encode(PH.url, PH.m);
}

static int ph_key(layer *l, int k, int ev) {
    (void)l;
    if (ev != EV_SHORT) return 1;
    if (k == KEY_BACK) layer_pop(&PHL);
    else if (k == KEY_OK && !phone_running()) start();
    return 1;
}

static void ph_tick(layer *l, double now) {
    (void)l; (void)now;
    phone_result r;
    if (phone_poll(&r)) {
        phone_fn fn = PH.done;
        void *ctx = PH.ctx;
        layer_pop(&PHL);
        tv_toast(tr(S_PHONE_GOT));
        if (fn) fn(&r, ctx);
        app_dirty();
    } else if (PH.ok && !phone_running()) {
        PH.ok = 0;   /* срок истёк */
        app_dirty();
    }
}

static void ph_draw(layer *l, gfx_t *g) {
    (void)l;
    /* QR: белая подложка с полем в 4 модуля (без него телефоны читают хуже); модуль — целое число пикселей */
    int mod = E(24) / (QR_SIZE + 8), plate = mod * (QR_SIZE + 8);
    int w = E(66), h = E(5.2) + plate + E(4.6), x = (SW - w) / 2, y = (SH - h) / 2;
    d_fill(g, 0, 0, SW, SH, C_OVERLAY);
    d_round(g, x, y, w, h, E(1), C_POPUP);
    d_text(g, x + E(2), y + E(1.6), 1.5, F_B, C_WHITE, tr(S_PHONE_TITLE), w - E(4));
    int qx = x + E(2), qy = y + E(5.2);
    int tx = qx + plate + E(2.2), tw = x + w - E(2) - tx;
    if (PH.ok) {
        d_round(g, qx, qy, plate, plate, E(0.6), 0xffffffffu);
        for (int r = 0; r < QR_SIZE; r++)
            for (int c = 0; c < QR_SIZE; c++)
                if (PH.m[r][c]) d_fill(g, qx + (c + 4) * mod, qy + (r + 4) * mod, mod, mod, 0xff000000u);
    } else {
        d_round(g, qx, qy, plate, plate, E(0.6), C_FILL);
    }
    int ty = qy;
    ty += d_lines(g, tx, ty, tw, 1.3, F_R, C_TEXT2, tr(S_PHONE_HINT), 6, 0.25) * d_line_h(1.3, F_R) + E(1.6);
    if (PH.ok) {
        ty += d_lines(g, tx, ty, tw, 1.3, F_S, C_WHITE, PH.url, 3, 0) * d_line_h(1.3, F_S) + E(1.6);
        d_lines(g, tx, ty, tw, 1.2, F_R, C_TEXT3, tr(S_PHONE_WAIT), 3, 0.2);
    } else {
        d_lines(g, tx, ty, tw, 1.3, F_S, C_ARCHIVE, PH.err[0] ? PH.err : tr(S_PHONE_EXPIRED), 3, 0.2);
    }
    hint hs[2] = {{0, tr(S_K_BACK), tr(S_CLOSE)}, {0, "OK", tr(S_PHONE_NEW)}};
    d_hints(g, x + E(2), y + h - E(2.2), hs, PH.ok ? 1 : 2);
}

static void ph_closed(layer *l) { (void)l; phone_stop(); }

void phone_open(phone_fn done, void *ctx) {
    PHL.name = "phone";
    PHL.key = ph_key;
    PHL.tick = ph_tick;
    PHL.draw = ph_draw;
    PHL.closed = ph_closed;
    PH.done = done;
    PH.ctx = ctx;
    start();
    layer_push(&PHL);
}
