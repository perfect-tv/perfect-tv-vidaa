/* Окно X11 для запуска интерфейса на ПК (perfecttv-host --window): холст 1920×1080 масштабируется в окно
 * (по умолчанию 1280×720, F11 — во весь экран 1:1), «дыра» холста — тестовая картинка «видео», клавиатура —
 * коды пульта evdev (раскладка — host_keys_help). Повтор клавиш X11 — как повтор пульта (value 2). */
#define _GNU_SOURCE
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void keys_raw(int code, int value);
extern int host_video_on;

static Display *dpy;
static Window win;
static GC gc;
static XImage *img;
static Atom wm_delete;
static int ww = 1280, wh = 720, closed;
static uint32_t *src;   /* последний кадр холста */
static int sw, sh;
static int down[512];

static const struct { KeySym k; int code; } MAP[] = {
    {XK_Left, 105}, {XK_Right, 106}, {XK_Up, 103}, {XK_Down, 108},
    {XK_Return, 28}, {XK_KP_Enter, 28},
    {XK_Escape, 158}, {XK_BackSpace, 158},
    {XK_m, 139}, {XK_Menu, 139},
    {XK_i, 358}, {XK_g, 365}, {XK_l, 395}, {XK_v, 364},
    {XK_Page_Up, 402}, {XK_Page_Down, 403}, {XK_equal, 402}, {XK_minus, 403},
    {XK_F1, 398}, {XK_F2, 399}, {XK_F3, 400}, {XK_F4, 401},
    {XK_space, 164}, {XK_bracketleft, 168}, {XK_bracketright, 208}, {XK_x, 128},
    {XK_t, 388}, {XK_s, 370}, {XK_f, 217}, {XK_slash, 217},
    {XK_Home, 102},
    {XK_0, 11}, {XK_1, 2}, {XK_2, 3}, {XK_3, 4}, {XK_4, 5}, {XK_5, 6}, {XK_6, 7}, {XK_7, 8}, {XK_8, 9}, {XK_9, 10},
    {XK_KP_0, 11}, {XK_KP_1, 2}, {XK_KP_2, 3}, {XK_KP_3, 4}, {XK_KP_4, 5}, {XK_KP_5, 6}, {XK_KP_6, 7}, {XK_KP_7, 8}, {XK_KP_8, 9}, {XK_KP_9, 10},
};

void host_keys_help(void) {
    printf("Клавиши: стрелки — стрелки пульта, Enter — OK, Esc / Backspace — «Назад», M — «Меню», I — INFO, G — гид,\n"
           "PgUp / PgDn (или = / -) — CH+ / CH−, F1–F4 — красная, зелёная, жёлтая, синяя, 0–9 — цифры,\n"
           "пробел — пауза, [ / ] — перемотка, X — стоп, L — недавние, V — избранное, T — телетекст, S — субтитры,\n"
           "F или / — поиск, Home — выход, F11 — окно / весь экран. Долгое нажатие — держать клавишу.\n");
}

static int code_for(KeySym k) {
    if (k >= 'A' && k <= 'Z') k += 32;
    for (size_t i = 0; i < sizeof MAP / sizeof MAP[0]; i++) if (MAP[i].k == k) return MAP[i].code;
    return 0;
}

static void make_image(void) {
    if (img) { XDestroyImage(img); img = NULL; }
    char *buf = malloc((size_t)ww * wh * 4);
    if (!buf) return;
    img = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), (unsigned)DefaultDepth(dpy, DefaultScreen(dpy)), ZPixmap, 0, buf,
                       (unsigned)ww, (unsigned)wh, 32, 0);
}

/* холст -> окно: «видео» под прозрачными пикселями, усреднение 2×2 при уменьшении */
static void blit(void) {
    if (!dpy || !src || !img) return;
    uint32_t *dst = (uint32_t *)img->data;
    int stride = img->bytes_per_line / 4;
    for (int y = 0; y < wh; y++) {
        int sy = (int)((long)y * sh / wh), sy2 = sy + 1 < sh ? sy + 1 : sy;
        for (int x = 0; x < ww; x++) {
            int sx = (int)((long)x * sw / ww), sx2 = sx + 1 < sw ? sx + 1 : sx;
            uint32_t p[4] = {src[sy * sw + sx], src[sy * sw + sx2], src[sy2 * sw + sx], src[sy2 * sw + sx2]};
            int n = ww < sw ? 4 : 1;
            unsigned r = 0, g = 0, b = 0;
            for (int k = 0; k < n; k++) {
                uint32_t c = p[k];
                unsigned a = c >> 24, vr = 0, vg = 0, vb = 0;
                if (host_video_on == 1) {
                    int vx = k & 1 ? sx2 : sx, vy = k & 2 ? sy2 : sy;
                    vr = (unsigned)(60 + vx * 120 / sw);
                    vg = (unsigned)(90 + vy * 90 / sh);
                    vb = (unsigned)(140 + ((vx + vy) / 40 % 2) * 30);
                }
                r += (((c >> 16) & 255) * a + vr * (255 - a)) / 255;
                g += (((c >> 8) & 255) * a + vg * (255 - a)) / 255;
                b += ((c & 255) * a + vb * (255 - a)) / 255;
            }
            dst[y * stride + x] = (r / (unsigned)n) << 16 | (g / (unsigned)n) << 8 | (b / (unsigned)n);
        }
    }
    XPutImage(dpy, win, gc, img, 0, 0, 0, 0, (unsigned)ww, (unsigned)wh);
    XFlush(dpy);
}

int host_window_open(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "нет дисплея X11 (DISPLAY не задан?)\n"); return -1; }
    int s = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, s), 0, 0, (unsigned)ww, (unsigned)wh, 0, BlackPixel(dpy, s), BlackPixel(dpy, s));
    /* WM_NAME — Latin-1 (UTF-8 там превращается в «Ã…»): ASCII; заголовок в UTF-8 — _NET_WM_NAME */
    XStoreName(dpy, win, "Perfect TV (PC)");
    const char *title = "Perfect TV — ПК";
    XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_NAME", False), XInternAtom(dpy, "UTF8_STRING", False), 8, PropModeReplace,
                    (const unsigned char *)title, (int)strlen(title));
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | KeyReleaseMask | StructureNotifyMask | FocusChangeMask);
    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    XSizeHints *hints = XAllocSizeHints();
    hints->flags = PMinSize;
    hints->min_width = 480;
    hints->min_height = 270;
    XSetWMNormalHints(dpy, win, hints);
    XFree(hints);
    gc = XCreateGC(dpy, win, 0, NULL);
    XkbSetDetectableAutoRepeat(dpy, True, NULL);   /* удержание: только повторные нажатия, без отпусканий */
    XMapWindow(dpy, win);
    make_image();
    return 0;
}

void host_window_present(const uint32_t *px, int w, int h, int stride) {
    if (!dpy) return;
    if (!src || w != sw || h != sh) { free(src); src = malloc((size_t)w * h * 4); sw = w; sh = h; }
    if (!src) return;
    for (int y = 0; y < h; y++) memcpy(src + (size_t)y * w, px + (size_t)y * stride, (size_t)w * 4);
    blit();
}

static void toggle_full(void) {
    static int full;
    full = !full;
    Atom st = XInternAtom(dpy, "_NET_WM_STATE", False), fs = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    XEvent e;
    memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.window = win;
    e.xclient.message_type = st;
    e.xclient.format = 32;
    e.xclient.data.l[0] = full ? 1 : 0;
    e.xclient.data.l[1] = (long)fs;
    XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
}

/* события окна; 0 — окно закрыли */
int host_window_poll(void) {
    if (!dpy) return 0;
    while (XPending(dpy)) {
        XEvent e;
        XNextEvent(dpy, &e);
        if (e.type == ClientMessage && (Atom)e.xclient.data.l[0] == wm_delete) closed = 1;
        else if (e.type == Expose && img) XPutImage(dpy, win, gc, img, 0, 0, 0, 0, (unsigned)ww, (unsigned)wh);
        else if (e.type == ConfigureNotify && (e.xconfigure.width != ww || e.xconfigure.height != wh)) {
            ww = e.xconfigure.width;
            wh = e.xconfigure.height;
            make_image();
            blit();
        } else if (e.type == FocusOut) {   /* отпустить всё, что держали */
            for (int c = 0; c < 512; c++) if (down[c]) { down[c] = 0; keys_raw(c, 0); }
        } else if (e.type == KeyPress || e.type == KeyRelease) {
            KeySym k = XLookupKeysym(&e.xkey, 0);
            if (k == XK_F11) { if (e.type == KeyPress) toggle_full(); continue; }
            int code = code_for(k);
            if (!code) continue;
            if (e.type == KeyPress) { keys_raw(code, down[code] ? 2 : 1); down[code] = 1; }
            else { down[code] = 0; keys_raw(code, 0); }
        }
    }
    return !closed;
}
