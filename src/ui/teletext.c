/* Окно телетекста во весь экран (docs/controls.md, «Телетекст»): сетка 40 × 25 по странице из core/ttx.c,
 * которую наполняет ретранслятор потока. Цифры — номер страницы, ↑/↓ и CH± — соседние принятые страницы,
 * ←/→ — подстраницы, цветные — ссылки Fastext, OK — показать скрытое, «Назад» — закрыть. */
#include "screens.h"
#include "layer.h"
#include "keys.h"
#include "tv.h"
#include "u.h"
#include "app.h"
#include "core/lang.h"
#include "core/relay.h"
#include "core/ttx.h"
#include <stdio.h>
#include <string.h>

static const uint32_t COLORS[8] = {0xff000000u, 0xffff0000u, 0xff00ff00u, 0xffffff00u, 0xff0000ffu, 0xffff00ffu, 0xff00ffffu, 0xffffffffu};

static struct {
    int number, sub, nsub, reveal, version;
    char typed[4];
    double opened, typed_at;
    ttx_page page;
    ttx_grid grid;
    int has;
} TT;
static layer TL;

static void load(void) {
    ttx *t = relay_ttx();
    TT.has = !ttx_get(t, TT.number, TT.sub, &TT.page, &TT.nsub);
    uint8_t row0[TTX_COLS];
    ttx_header_row(TT.number, TT.has ? &TT.page : NULL, TT.typed, row0);
    if (TT.has) ttx_layout(&TT.page, row0, &TT.grid);
    TT.version = ttx_version(t);
}

static void go(int number) {
    if (number < 0x100 || number > 0x8FF) return;
    TT.number = number;
    TT.sub = -1;
    TT.typed[0] = 0;
    load();
}

static int tt_key(layer *l, int k, int ev) {
    (void)l;
    if (ev == EV_LONG) return 1;
    ttx *t = relay_ttx();
    if (IS_DIGIT(k)) {
        int d = k - KEY_D0;
        size_t n = strlen(TT.typed);
        if (n == 0 && (d < 1 || d > 8)) return 1;   /* страницы 100…899 */
        TT.typed[n] = (char)('0' + d);
        TT.typed[n + 1] = 0;
        TT.typed_at = app_now();
        if (n + 1 == 3) go((TT.typed[0] - '0') << 8 | (TT.typed[1] - '0') << 4 | d);
        else load();
        return 1;
    }
    if (k == KEY_BACK || k == KEY_TEXT) {
        if (TT.typed[0]) { TT.typed[0] = 0; load(); }
        else layer_pop(&TL);
        return 1;
    }
    if (k == KEY_UP || k == KEY_CH_UP || k == KEY_DOWN || k == KEY_CH_DOWN) {
        int n = ttx_neighbour(t, TT.number, k == KEY_UP || k == KEY_CH_UP ? 1 : -1);
        if (n) go(n);
    } else if ((k == KEY_LEFT || k == KEY_RIGHT) && TT.nsub > 1) {
        int cur = TT.sub < 0 ? TT.nsub - 1 : TT.sub;
        TT.sub = (cur + (k == KEY_RIGHT ? 1 : TT.nsub - 1)) % TT.nsub;
        load();
    } else if (k >= KEY_RED && k <= KEY_BLUE && TT.has) {
        int link = TT.page.links[k - KEY_RED];
        if (link) go(link);
    } else if (k == KEY_OK) TT.reveal = !TT.reveal;
    return 1;
}

static void tt_tick(layer *l, double now) {
    (void)l;
    if (TT.typed[0] && now - TT.typed_at > 4) { TT.typed[0] = 0; load(); app_dirty(); }
    if (ttx_version(relay_ttx()) != TT.version) { load(); app_dirty(); }
    static double next;
    if (!TT.has && now >= next) { next = now + 1; app_dirty(); }   /* состояние «ждём» */
}

static void mosaic(gfx_t *g, int x, int y, int w, int h, uint32_t code, int sep, uint32_t c) {
    static const int bit[6] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x40};
    int hw = w / 2, th = h / 3;
    for (int i = 0; i < 6; i++) {
        if (!(code & (uint32_t)bit[i])) continue;
        int bx = x + (i & 1) * hw, by = y + (i / 2) * th, bw = (i & 1) ? w - hw : hw, bh = i / 2 == 2 ? h - 2 * th : th;
        if (sep) { bx += 2; by += 2; bw -= 2; bh -= 2; }
        d_fill(g, bx, by, bw, bh, c);
    }
}

static void tt_draw(layer *l, gfx_t *g) {
    (void)l;
    d_clear(g, 0xff000000u);
    int ch = (SH - E(4)) / TTX_ROWS, cw = ch * 4 / 5, x0 = (SW - cw * TTX_COLS) / 2, y0 = E(1);
    double em = ch * 0.8 / E(1);
    ttx *t = relay_ttx();
    if (!TT.has) {
        /* строка заголовка и состояние */
        uint8_t row0[TTX_COLS];
        ttx_header_row(TT.number, NULL, TT.typed, row0);
        char hdr[TTX_COLS + 1];
        memcpy(hdr, row0, TTX_COLS);
        hdr[TTX_COLS] = 0;
        d_text(g, x0, y0, em, F_S, C_WHITE, hdr, 0);
        const char *msg;
        char num[8];
        snprintf(num, sizeof num, "%03X", TT.number);
        int waited = app_now() - TT.opened > 8;
        if (relay_has_teletext()) msg = ttx_count(t) ? trf(S_TTX_NO_PAGE, num) : tr(S_TTX_WAIT);
        else if (waited && relay_ts_bytes() > 0) msg = relay_has_dvbsub() ? tr(S_TTX_DVB_ONLY) : tr(S_TTX_NONE);
        else msg = tr(S_TTX_WAIT);
        d_text_c(g, 0, SW, SH / 2 - E(1), 1.5, F_S, C_TEXT2, msg);
    } else {
        for (int r = 0; r < TTX_ROWS; r++)
            for (int c = 0; c < TTX_COLS; c++) {
                int i = r * TTX_COLS + c, a = TT.grid.attr[i], x = x0 + c * cw, y = y0 + r * ch;
                uint32_t fg = COLORS[a & 7], bg = COLORS[(a >> 3) & 7];
                if (a & TA_DH_BOTTOM) continue;   /* нижняя половина: фон и знак уже нарисованы ячейкой выше */
                d_fill(g, x, y, cw, (a & TA_DH) ? ch * 2 : ch, bg);
                if ((a & TA_CONCEAL) && !TT.reveal) continue;
                uint32_t code = TT.grid.ch[i];
                if (a & TA_MOSAIC) { mosaic(g, x, y, cw, (a & TA_DH) ? ch * 2 : ch, code, a & TA_SEPARATED, fg); continue; }
                if (code == 0x20) continue;
                char s[8];
                s[u8_put(s, code)] = 0;
                if (a & TA_DH) d_text_c(g, x - cw / 2, cw * 2, y + ch / 10, em * 1.9, F_R, fg, s);
                else d_text_c(g, x - cw / 2, cw * 2, y + ch / 10, em, F_R, fg, s);
            }
    }
    /* подсказка и подстраницы */
    char info[200];
    if (TT.has && TT.nsub > 1) {
        int cur = TT.sub < 0 ? TT.nsub : TT.sub + 1;
        snprintf(info, sizeof info, "%d / %d   ·   %s", cur, TT.nsub, tr(S_TTX_HINT));
    } else snprintf(info, sizeof info, "%s", tr(S_TTX_HINT));
    d_text_c(g, 0, SW, SH - E(2.4), 1.05, F_R, C_HINT, info);
}

void teletext_open(void) {
    TL.name = "teletext";
    TL.key = tt_key;
    TL.tick = tt_tick;
    TL.draw = tt_draw;
    TL.covers = 1;
    memset(&TT, 0, sizeof TT);
    TT.number = 0x100;
    TT.sub = -1;
    TT.opened = app_now();
    load();
    layer_push(&TL);
}
