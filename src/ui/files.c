/* Окно «Файл с флешки»: накопители (core/usb.c), папки и файлы плейлистов / программы. OK на папке — войти, на файле —
 * выбрать; «Назад» — папка выше, из корня накопителя — к списку накопителей (один накопитель — закрыть). */
#include "screens.h"
#include "layer.h"
#include "keys.h"
#include "tv.h"
#include "u.h"
#include "app.h"
#include "core/lang.h"
#include "core/usb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    usb_root roots[8];
    int nroots, root;          /* root -1 — список накопителей */
    char dir[USB_PATH];
    usb_entry *e;
    int n, focus, top, err;
    file_fn done;
    void *ctx;
} FB;
static layer FL;

static void load(void) {
    free(FB.e);
    FB.e = NULL;
    FB.n = 0;
    FB.err = 0;
    FB.focus = FB.top = 0;
    if (FB.root < 0) return;
    FB.n = usb_list(FB.dir, &FB.e);
    if (FB.n < 0) { FB.n = 0; FB.err = 1; }
}
static int count(void) { return FB.root < 0 ? FB.nroots : FB.n; }

static void enter_root(int i) {
    FB.root = i;
    snprintf(FB.dir, sizeof FB.dir, "%s", FB.roots[i].path);
    load();
}

static void up(void) {
    if (FB.root < 0) { layer_pop(&FL); return; }
    if (!strcmp(FB.dir, FB.roots[FB.root].path)) {
        if (FB.nroots <= 1) { layer_pop(&FL); return; }
        FB.root = -1;
        load();
        return;
    }
    char was[256];
    char *s = strrchr(FB.dir, '/');
    snprintf(was, sizeof was, "%s", s ? s + 1 : "");
    if (s) *s = 0;
    load();
    for (int i = 0; i < FB.n; i++) if (FB.e[i].dir && !strcmp(FB.e[i].name, was)) FB.focus = i;
}

static int fb_key(layer *l, int k, int ev) {
    (void)l;
    int n = count();
    if (k == KEY_BACK && ev == EV_SHORT) { up(); return 1; }
    if (k == KEY_LEFT) { up(); return 1; }
    if (!n) return 1;
    if (k == KEY_UP) FB.focus = (FB.focus + n - 1) % n;
    else if (k == KEY_DOWN) FB.focus = (FB.focus + 1) % n;
    else if (k == KEY_CH_UP) FB.focus = MAX(0, FB.focus - 8);
    else if (k == KEY_CH_DOWN) FB.focus = MIN(n - 1, FB.focus + 8);
    else if ((k == KEY_OK && ev == EV_SHORT) || k == KEY_RIGHT) {
        if (FB.root < 0) { enter_root(FB.focus); return 1; }
        usb_entry *e = &FB.e[FB.focus];
        if (e->dir) {
            size_t l0 = strlen(FB.dir);
            if (l0 + strlen(e->name) + 2 < sizeof FB.dir) { snprintf(FB.dir + l0, sizeof FB.dir - l0, "/%s", e->name); load(); }
        } else if (k == KEY_OK) {
            char path[USB_PATH + 260];
            snprintf(path, sizeof path, "%s/%s", FB.dir, e->name);
            file_fn fn = FB.done;
            void *ctx = FB.ctx;
            int epg = usb_is_epg(e->name);
            layer_pop(&FL);
            if (fn) fn(path, epg, ctx);
        }
    }
    return 1;
}

static void size_str(long long b, char *out, size_t cap) {
    if (b >= 1 << 20) snprintf(out, cap, "%.1f %s", b / 1048576.0, tr(S_MB));
    else snprintf(out, cap, "%lld %s", (b + 1023) / 1024, tr(S_KB));
}

static void fb_draw(layer *l, gfx_t *g) {
    (void)l;
    int w = E(64), h = SH - E(8), x = (SW - w) / 2, y = E(4);
    d_fill(g, 0, 0, SW, SH, C_OVERLAY);
    d_round(g, x, y, w, h, E(1), C_POPUP);
    d_text(g, x + E(2), y + E(1.5), 1.5, F_B, C_WHITE, tr(S_USB_TITLE), w - E(4));
    const char *sub = FB.root < 0 ? tr(S_USB_DRIVES) : FB.dir;
    /* хвост длинного пути */
    while (*sub && d_text_w(1.15, F_R, sub) > w - E(4)) { sub++; while ((*sub & 0xc0) == 0x80) sub++; }
    d_text(g, x + E(2), y + E(4.1), 1.15, F_R, C_TEXT3, sub, w - E(4));
    int ly = y + E(6.8), rh = E(3.4), vis = (h - E(10.4)) / rh, n = count();
    if (!n) {
        const char *msg = FB.root < 0 ? tr(S_USB_NONE) : FB.err ? tr(S_USB_CANT_OPEN) : tr(S_USB_EMPTY);
        d_lines(g, x + E(2), ly + E(0.4), w - E(4), 1.25, F_R, C_TEXT2, msg, 4, 0.2);
    }
    if (FB.focus < FB.top) FB.top = FB.focus;
    if (FB.focus >= FB.top + vis) FB.top = FB.focus - vis + 1;
    for (int r = 0; r < vis && FB.top + r < n; r++) {
        int i = FB.top + r, ry = ly + r * rh, foc = i == FB.focus;
        d_round(g, x + E(1), ry, w - E(2), rh - E(0.3), E(0.4), foc ? C_WHITE : C_FILL);
        uint32_t fg = foc ? 0xff101010u : C_WHITE, fg2 = foc ? 0xff404040u : C_TEXT3;
        const char *name = FB.root < 0 ? FB.roots[i].name : FB.e[i].name;
        int dir = FB.root < 0 || FB.e[i].dir;
        char right[40] = "";
        if (FB.root < 0) snprintf(right, sizeof right, "%s", FB.roots[i].path);
        else if (!dir) size_str(FB.e[i].size, right, sizeof right);
        int rw = right[0] ? d_text_w(1.1, F_R, right) + E(1.2) : 0;
        if (dir) { d_icon(g, I_CHEVRON, x + w - E(2.6), ry + (rh - E(0.3)) / 2, E(1.1), fg2); rw += E(1.8); }
        d_text_mid(g, x + E(2.2), ry + (rh - E(0.3)) / 2, 1.25, dir ? F_S : F_R, fg, name, w - E(4.4) - rw);
        if (right[0]) d_text_r(g, x + w - E(2.2) - (dir ? E(1.8) : 0), ry + (rh - E(0.3)) / 2 - d_line_h(1.1, F_R) / 2, 1.1, F_R, fg2, right);
    }
    hint hs[2] = {{0, "OK", tr(S_USB_OPEN)}, {0, tr(S_K_BACK), tr(S_USB_UP)}};
    d_hints(g, x + E(2), y + h - E(2.2), hs, 2);
}

static void fb_closed(layer *l) { (void)l; free(FB.e); FB.e = NULL; FB.n = 0; }

void files_open(file_fn done, void *ctx) {
    FL.name = "files";
    FL.key = fb_key;
    FL.draw = fb_draw;
    FL.closed = fb_closed;
    free(FB.e);
    memset(&FB, 0, sizeof FB);
    FB.done = done;
    FB.ctx = ctx;
    FB.nroots = usb_roots(FB.roots, 8);
    FB.root = -1;
    if (FB.nroots == 1) enter_root(0);
    layer_push(&FL);
}
