/* Формы с экранной клавиатурой: ввод строки (адрес плейлиста, программа) и форма плейлиста.
 * Клавиатура — сетка 11 × 5: три ряда букв (ЙЦУКЕН / QWERTY / символы с кусками адресов), ряд цифр и служебный
 * ряд. Цифры пульта вводятся сразу. */
#include "screens.h"
#include "layer.h"
#include "keys.h"
#include "tv.h"
#include "u.h"
#include "app.h"
#include "core/lang.h"
#include "core/prefs.h"
#include "data/loaders.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLS 11
static const char *const RU[3][COLS] = {{"й", "ц", "у", "к", "е", "н", "г", "ш", "щ", "з", "х"},
                                        {"ф", "ы", "в", "а", "п", "р", "о", "л", "д", "ж", "э"},
                                        {"я", "ч", "с", "м", "и", "т", "ь", "б", "ю", "ъ", "ё"}};
static const char *const EN[3][COLS] = {{"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "-"},
                                        {"a", "s", "d", "f", "g", "h", "j", "k", "l", ":", "_"},
                                        {"z", "x", "c", "v", "b", "n", "m", ".", "/", "?", "@"}};
static const char *const SY[3][COLS] = {{"http://", "https://", "www.", ".m3u", ".m3u8", ".xml", ".gz", ".com", ".ru", ".tv", "/"},
                                        {"!", "#", "$", "%", "&", "*", "(", ")", "=", "+", ";"},
                                        {"'", "\"", ",", "~", "[", "]", "{", "}", "|", "\\", "^"}};
static const char *const DG[COLS] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "."};
/* служебный ряд: ⇧ RU/EN #& пробел(4) ⌫(2) готово(2) */
enum { K_SHIFT, K_LANG, K_SYM, K_SPACE, K_BKSP, K_DONE };
static const int SVC_SPAN[] = {1, 1, 1, 4, 2, 2};

typedef struct {
    char text[2048];
    int row, col, lang, shift, sym;   /* lang 0 en, 1 ru */
} kb;

static void kb_type(kb *k, const char *s) {
    char up[16];
    if (k->shift && !k->sym) { text_upper(up, sizeof up, s); s = up; k->shift = 0; }
    size_t n = strlen(k->text), m = strlen(s);
    if (n + m < sizeof k->text) memcpy(k->text + n, s, m + 1);
}
static void kb_back(kb *k) {
    size_t n = strlen(k->text);
    if (!n) return;
    n--;
    while (n > 0 && ((unsigned char)k->text[n] & 0xc0) == 0x80) n--;
    k->text[n] = 0;
}
static int svc_at(int col) {   /* служебная клавиша под колонкой */
    int c = 0;
    for (int i = 0; i < 6; i++) { if (col < c + SVC_SPAN[i]) return i; c += SVC_SPAN[i]; }
    return 5;
}
static int svc_col(int i) { int c = 0; for (int k = 0; k < i; k++) c += SVC_SPAN[k]; return c; }
static const char *kb_label(const kb *k, int r, int c) {
    if (r == 3) return DG[c];
    if (k->sym) return SY[r][c];
    return k->lang ? RU[r][c] : EN[r][c];
}
/* 1 — «Готово» */
static int kb_key(kb *k, int key, int ev) {
    if (IS_DIGIT(key)) { char d[2] = {(char)('0' + key - KEY_D0), 0}; kb_type(k, d); return 0; }
    if (key == KEY_LEFT) { if (k->row == 4) { int s = svc_at(k->col); s = (s + 5) % 6; k->col = svc_col(s); } else k->col = (k->col + COLS - 1) % COLS; }
    else if (key == KEY_RIGHT) { if (k->row == 4) { int s = svc_at(k->col); s = (s + 1) % 6; k->col = svc_col(s); } else k->col = (k->col + 1) % COLS; }
    else if (key == KEY_UP) k->row = (k->row + 4) % 5;
    else if (key == KEY_DOWN) k->row = (k->row + 1) % 5;
    else if (key == KEY_CH_UP) kb_back(k);
    else if (key == KEY_OK) {
        if (k->row < 4) { kb_type(k, kb_label(k, k->row, k->col)); return 0; }
        switch (svc_at(k->col)) {
        case K_SHIFT: k->shift = !k->shift; break;
        case K_LANG: k->lang = !k->lang; k->sym = 0; break;
        case K_SYM: k->sym = !k->sym; break;
        case K_SPACE: kb_type(k, " "); break;
        case K_BKSP: if (ev == EV_LONG) k->text[0] = 0; else kb_back(k); break;
        case K_DONE: return 1;
        }
    }
    return 0;
}
static void kb_draw(gfx_t *g, const kb *k, int x, int y, int w) {
    int gap = E(0.4), kw = (w - (COLS - 1) * gap) / COLS, kh = E(3.2);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < COLS; c++) {
            int bx = x + c * (kw + gap), by = y + r * (kh + gap), foc = k->row == r && k->col == c;
            d_round(g, bx, by, kw, kh, E(0.4), foc ? C_WHITE : C_FILL_BTN);
            const char *lab = kb_label(k, r, c);
            char up[16];
            if (k->shift && !k->sym && r < 3) { text_upper(up, sizeof up, lab); lab = up; }
            double em = strlen(lab) > 4 ? 0.95 : 1.4;
            d_text_c(g, bx, kw, by + (kh - d_line_h(em, F_S)) / 2, em, F_S, foc ? 0xff101010u : C_WHITE, lab);
        }
    int by = y + 4 * (kh + gap);
    for (int i = 0; i < 6; i++) {
        int c0 = svc_col(i), span = SVC_SPAN[i];
        int bx = x + c0 * (kw + gap), bw = span * kw + (span - 1) * gap, foc = k->row == 4 && svc_at(k->col) == i;
        int on = (i == K_SHIFT && k->shift) || (i == K_SYM && k->sym);
        d_round(g, bx, by, bw, kh, E(0.4), foc ? C_WHITE : on ? C_FILL_ACTIVE : C_FILL_BTN);
        uint32_t fg = foc ? 0xff101010u : C_WHITE;
        const char *lab = i == K_SHIFT ? "⇧" : i == K_LANG ? (k->lang ? "RU" : "EN") : i == K_SYM ? "#&" : i == K_SPACE ? tr(S_KB_SPACE) : i == K_DONE ? tr(S_KB_DONE) : NULL;
        if (i == K_SHIFT) d_icon(g, I_UP, bx + bw / 2, by + kh / 2, E(1.3), fg);
        else if (lab) d_text_c(g, bx, bw, by + (kh - d_line_h(1.2, F_S)) / 2, 1.2, F_S, fg, lab);
        else d_icon(g, I_BACKSPACE, bx + bw / 2, by + kh / 2, E(1.6), fg);
    }
}
static int kb_height(void) { return 5 * E(3.2) + 4 * E(0.4); }

/* ================= ввод строки ================= */
static struct { char title[200]; kb k; text_fn done; void *ctx; int multiline; } TX;
static layer TL;
static int text_has_long(layer *l, int key) { (void)l; return key == KEY_OK && TX.k.row == 4 && svc_at(TX.k.col) == K_BKSP; }
static int text_key(layer *l, int key, int ev) {
    (void)l;
    if (key == KEY_BACK && ev == EV_SHORT) { layer_pop(&TL); return 1; }
    if (kb_key(&TX.k, key, ev)) {
        char t[2048];
        snprintf(t, sizeof t, "%s", TX.k.text);
        trim(t);
        text_fn f = TX.done;
        void *ctx = TX.ctx;
        layer_pop(&TL);
        if (f) f(t, ctx);
    }
    return 1;
}
static void text_layer_draw(layer *l, gfx_t *g) {
    (void)l;
    int w = E(62), h = E(12) + kb_height(), x = (SW - w) / 2, y = (SH - h) / 2;
    d_fill(g, 0, 0, SW, SH, C_OVERLAY);
    d_round(g, x, y, w, h, E(1), C_POPUP);
    d_text(g, x + E(2), y + E(1.5), 1.4, F_B, C_WHITE, TX.title, w - E(4));
    int fy = y + E(4.4), fw = w - E(4), fh = E(3.6);
    d_round(g, x + E(2), fy, fw, fh, E(0.4), C_WHITE);
    /* хвост текста, если не помещается; курсор в конце */
    const char *t = TX.k.text;
    while (*t && d_text_w(1.3, F_R, t) > fw - E(2.4)) { t++; while ((*t & 0xc0) == 0x80) t++; }
    int tw = d_text_mid(g, x + E(3), fy + fh / 2, 1.3, F_R, 0xff101010u, t, 0);
    if ((int)(app_now() * 2) % 2 == 0) d_fill(g, x + E(3) + tw + E(0.15), fy + E(0.7), E(0.2), fh - E(1.4), 0xff101010u);
    kb_draw(g, &TX.k, x + E(2), fy + fh + E(1.4), w - E(4));
    app_dirty();
}
void form_text(const char *title, const char *initial, int multiline, text_fn done, void *ctx) {
    TL.name = "form";
    TL.key = text_key;
    TL.has_long = text_has_long;
    TL.draw = text_layer_draw;
    memset(&TX, 0, sizeof TX);
    snprintf(TX.title, sizeof TX.title, "%s", title);
    snprintf(TX.k.text, sizeof TX.k.text, "%s", initial ? initial : "");
    TX.k.lang = 0;
    TX.k.sym = initial && *initial ? 0 : !strcmp(title, tr(S_PLAYLIST_URL)) || !strcmp(title, tr(S_EPG_URLS));
    TX.multiline = multiline;
    TX.done = done;
    TX.ctx = ctx;
    layer_push(&TL);
}

/* ================= форма плейлиста ================= */
enum { F_NAME, F_URL, F_EPG_MODE, F_EPG_FIRST };
static struct {
    int index;                 /* -1 — новый */
    char name[160], url[2048];
    int epg_mode;
    strlist epg;
    int focus, btn, top;
} PF;
static layer PL;

static int rows(void) { return F_EPG_FIRST + PF.epg.n + 1 + 1; }   /* поля, свои источники, «добавить», кнопки */
static int buttons_row(void) { return rows() - 1; }

static void set_name(const char *t, void *c) { (void)c; snprintf(PF.name, sizeof PF.name, "%s", t); }
static void set_url(const char *t, void *c) {
    (void)c;
    snprintf(PF.url, sizeof PF.url, "%s", t);
    if (!PF.name[0] && t[0]) {   /* имя по умолчанию — из адреса */
        const char *s = strrchr(t, '/');
        snprintf(PF.name, sizeof PF.name, "%s", s && s[1] ? s + 1 : t);
        char *q = strchr(PF.name, '?');
        if (q) *q = 0;
    }
}
static void set_epg(const char *t, void *c) {
    int i = (int)(intptr_t)c;
    if (i < PF.epg.n) { if (*t) { free(PF.epg.v[i]); PF.epg.v[i] = strdup(t); } else sl_remove(&PF.epg, i); }
    else if (*t) sl_add(&PF.epg, t);
}

static void save(void) {
    if (!PF.url[0]) { tv_toast(tr(S_ERR_NO_URL)); return; }
    playlist *p;
    int idx = PF.index;
    int url_changed = 1;
    if (idx < 0) {
        p = repo_add_playlist(PF.name, PF.url);
        if (!p) return;
        idx = R.npl - 1;
    } else {
        p = &R.pl[idx];
        url_changed = strcmp(p->url, PF.url) != 0;
        snprintf(p->name, sizeof p->name, "%s", PF.name[0] ? PF.name : p->name);
        snprintf(p->url, sizeof p->url, "%s", PF.url);
        if (url_changed) { p->etag[0] = p->last_modified[0] = 0; p->loaded_at = 0; }
    }
    char *ue = sl_join(&PF.epg, "\n");
    int epg_changed = !p->user_epg || strcmp(p->user_epg, ue) || p->epg_mode != PF.epg_mode;
    free(p->user_epg);
    p->user_epg = ue;
    p->epg_mode = PF.epg_mode;
    repo_save_playlists();
    layer_pop(&PL);
    /* первый плейлист (мастер) — закрыть всё и загрузить */
    layer *t = layer_count() ? layer_at(0) : NULL;
    if (t && t->name && !strcmp(t->name, "welcome")) layer_pop_all();
    if (idx == R.cur || R.cur < 0 || R.nch == 0) {
        if (R.cur != idx) { loaders_cancel(); repo_select(idx); }
        if (url_changed || !R.nch) load_playlist(idx, 1);
        else if (epg_changed) { repo_open_epg(); load_epg(0); }
    }
}
static void del_yes(void *c) {
    (void)c;
    int idx = PF.index;
    layer_pop(&PL);
    int was_cur = idx == R.cur;
    repo_delete_playlist(idx);
    if (was_cur) {
        loaders_cancel();
        if (R.npl) { R.cur = 0; tv_playlist_changed(0); }
        else { repo_select(-1); T.cur = NULL; layer_pop_all(); welcome_open(); }
    }
}

static int nbuttons(void) { return PF.index >= 0 ? 3 : 2; }
static int pf_key(layer *l, int k, int ev) {
    (void)l;
    int n = rows();
    if (k == KEY_BACK && ev == EV_SHORT) { layer_pop(&PL); return 1; }
    if (k == KEY_UP || k == KEY_DOWN) { PF.focus = CLAMP(PF.focus + (k == KEY_DOWN ? 1 : -1), 0, n - 1); return 1; }
    int f = PF.focus;
    if (f == buttons_row()) {
        if (k == KEY_LEFT || k == KEY_RIGHT) PF.btn = CLAMP(PF.btn + (k == KEY_RIGHT ? 1 : -1), 0, nbuttons() - 1);
        else if (k == KEY_OK && ev == EV_SHORT) {
            if (PF.btn == 0) save();
            else if (PF.index >= 0 && PF.btn == 1) confirm_open(trf(S_CONFIRM_DELETE, PF.name), tr(S_DELETE), del_yes, NULL);
            else layer_pop(&PL);
        }
        return 1;
    }
    if (f == F_EPG_MODE && (k == KEY_LEFT || k == KEY_RIGHT || k == KEY_OK)) { PF.epg_mode = (PF.epg_mode + (k == KEY_LEFT ? 2 : 1)) % 3; return 1; }
    if (k != KEY_OK || ev != EV_SHORT) return 1;
    if (f == F_NAME) form_text(tr(S_PLAYLIST_NAME), PF.name, 0, set_name, NULL);
    else if (f == F_URL) form_text(tr(S_PLAYLIST_URL), PF.url, 0, set_url, NULL);
    else if (f >= F_EPG_FIRST) {
        int i = f - F_EPG_FIRST;
        form_text(tr(S_EPG_URLS), i < PF.epg.n ? PF.epg.v[i] : "", 0, set_epg, (void *)(intptr_t)i);
    }
    return 1;
}

static void pf_draw(layer *l, gfx_t *g) {
    (void)l;
    int w = E(64), h = SH - E(8), x = (SW - w) / 2, y = E(4);
    d_fill(g, 0, 0, SW, SH, C_OVERLAY);
    d_round(g, x, y, w, h, E(1), C_POPUP);
    d_text(g, x + E(2), y + E(1.5), 1.5, F_B, C_WHITE, PF.index < 0 ? tr(S_NEW_PLAYLIST) : PF.name, w - E(4));
    int ly = y + E(5), row = E(4.6), fx = x + E(2), fw = w - E(4);
    int vis = (h - E(10)) / row;
    if (PF.top > PF.focus) PF.top = PF.focus;
    if (PF.focus >= PF.top + vis && PF.focus != buttons_row()) PF.top = PF.focus - vis + 1;
    int r = 0;
    for (int i = PF.top; i < buttons_row() && r < vis; i++, r++) {
        int ry = ly + r * row, foc = PF.focus == i;
        d_round(g, fx, ry, fw, row - E(0.5), E(0.5), foc ? C_WHITE : C_FILL);
        uint32_t fg = foc ? 0xff101010u : C_WHITE, fg2 = foc ? 0xff404040u : C_TEXT3;
        const char *label = "", *value = "";
        char buf[2100];
        static const int modes[] = {S_EPG_FROM_PLAYLIST, S_EPG_BOTH, S_EPG_MINE};
        if (i == F_NAME) { label = tr(S_PLAYLIST_NAME); value = PF.name; }
        else if (i == F_URL) { label = tr(S_PLAYLIST_URL); value = PF.url; }
        else if (i == F_EPG_MODE) { label = tr(S_EPG_MODE); snprintf(buf, sizeof buf, "‹  %s  ›", tr(modes[PF.epg_mode])); value = buf; }
        else if (i - F_EPG_FIRST < PF.epg.n) { label = tr(S_EPG_URLS); value = PF.epg.v[i - F_EPG_FIRST]; }
        else { label = tr(S_EPG_URLS); snprintf(buf, sizeof buf, "+  %s", tr(S_ADD_SOURCE)); value = buf; }
        d_text(g, fx + E(1.2), ry + E(0.4), 1.05, F_R, fg2, label, fw - E(2.4));
        d_text(g, fx + E(1.2), ry + E(2.0), 1.3, F_S, fg, value, fw - E(2.4));
    }
    /* адреса программы из плейлиста — только чтение */
    if (PF.index >= 0 && R.pl[PF.index].header_epg && R.pl[PF.index].header_epg[0] && r < vis) {
        char ro[600];
        snprintf(ro, sizeof ro, "%s: %s", tr(S_FROM_PLAYLIST_RO), R.pl[PF.index].header_epg);
        for (char *q = ro; *q; q++) if (*q == '\n') *q = ' ';
        d_text(g, fx + E(1.2), ly + r * row + E(0.6), 1.05, F_R, C_TEXT3, ro, fw - E(2.4));
    }
    int by = y + h - E(4.6), bx = fx;
    const char *b[3] = {tr(S_SAVE), PF.index >= 0 ? tr(S_DELETE) : tr(S_CANCEL), tr(S_CANCEL)};
    for (int i = 0; i < nbuttons(); i++) {
        int foc = PF.focus == buttons_row() && PF.btn == i;
        int bw = d_text_w(1.25, F_S, b[i]) + E(3);
        d_round(g, bx, by, bw, E(3), E(0.4), foc ? C_WHITE : C_FILL_BTN);
        d_text_c(g, bx, bw, by + (E(3) - d_line_h(1.25, F_S)) / 2, 1.25, F_S, foc ? 0xff101010u : C_WHITE, b[i]);
        bx += bw + E(1);
    }
}

static void pf_closed(layer *l) { (void)l; sl_clear(&PF.epg); }

void form_playlist(int index) {
    PL.name = "form";
    PL.key = pf_key;
    PL.draw = pf_draw;
    PL.closed = pf_closed;
    sl_clear(&PF.epg);
    memset(&PF, 0, sizeof PF);
    PF.index = index >= 0 && index < R.npl ? index : -1;
    if (PF.index >= 0) {
        playlist *p = &R.pl[PF.index];
        snprintf(PF.name, sizeof PF.name, "%s", p->name);
        snprintf(PF.url, sizeof PF.url, "%s", p->url);
        PF.epg_mode = p->epg_mode;
        sl_from_lines(&PF.epg, p->user_epg);
    }
    PF.focus = PF.index < 0 ? F_URL : F_NAME;
    layer_push(&PL);
}
