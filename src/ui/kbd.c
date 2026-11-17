#include "kbd.h"
#include "keys.h"
#include "u.h"
#include "core/lang.h"
#include <stdio.h>
#include <string.h>

#define COLS KB_COLS
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

void kb_type(kb *k, const char *s) {
    char up[16];
    if (k->shift && !k->sym) { text_upper(up, sizeof up, s); s = up; k->shift = 0; }
    size_t n = strlen(k->text), m = strlen(s);
    if (n + m < sizeof k->text) memcpy(k->text + n, s, m + 1);
}
void kb_back(kb *k) {
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
int kb_long(const kb *k, int key) { return key == KEY_OK && k->row == 4 && svc_at(k->col) == K_BKSP; }

int kb_key(kb *k, int key, int ev) {
    if (IS_DIGIT(key)) { char d[2] = {(char)('0' + key - KEY_D0), 0}; kb_type(k, d); return KB_NONE; }
    int last = k->row == 4 ? svc_at(k->col) == 5 : k->col == COLS - 1;
    if (k->edges && key == KEY_RIGHT && last) return KB_OUT_RIGHT;
    if (k->edges && key == KEY_LEFT && (k->row == 4 ? svc_at(k->col) == 0 : k->col == 0)) return KB_OUT_LEFT;
    if (k->edges && key == KEY_DOWN && k->row == 4) return KB_OUT_DOWN;
    if (key == KEY_LEFT) { if (k->row == 4) { int s = svc_at(k->col); s = (s + 5) % 6; k->col = svc_col(s); } else k->col = (k->col + COLS - 1) % COLS; }
    else if (key == KEY_RIGHT) { if (k->row == 4) { int s = svc_at(k->col); s = (s + 1) % 6; k->col = svc_col(s); } else k->col = (k->col + 1) % COLS; }
    else if (key == KEY_UP) { if (!(k->edges && k->row == 0)) k->row = (k->row + 4) % 5; }
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
        case K_DONE: return KB_DONE;
        }
    }
    return KB_NONE;
}
void kb_draw(gfx_t *g, const kb *k, int x, int y, int w, int focused) {
    int gap = E(0.4), kw = (w - (COLS - 1) * gap) / COLS, kh = E(3.2);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < COLS; c++) {
            int bx = x + c * (kw + gap), by = y + r * (kh + gap), sel = k->row == r && k->col == c, foc = sel && focused;
            d_round(g, bx, by, kw, kh, E(0.4), foc ? C_WHITE : C_FILL_BTN);
            if (sel && !focused) d_stroke(g, bx, by, kw, kh, E(0.4), E(0.12), WA(0.6));
            const char *lab = kb_label(k, r, c);
            char up[16];
            if (k->shift && !k->sym && r < 3) { text_upper(up, sizeof up, lab); lab = up; }
            double em = strlen(lab) > 4 ? 0.95 : 1.4;
            d_text_c(g, bx, kw, by + (kh - d_line_h(em, F_S)) / 2, em, F_S, foc ? 0xff101010u : C_WHITE, lab);
        }
    int by = y + 4 * (kh + gap);
    for (int i = 0; i < 6; i++) {
        int c0 = svc_col(i), span = SVC_SPAN[i];
        int bx = x + c0 * (kw + gap), bw = span * kw + (span - 1) * gap, sel = k->row == 4 && svc_at(k->col) == i, foc = sel && focused;
        int on = (i == K_SHIFT && k->shift) || (i == K_SYM && k->sym);
        d_round(g, bx, by, bw, kh, E(0.4), foc ? C_WHITE : on ? C_FILL_ACTIVE : C_FILL_BTN);
        if (sel && !focused) d_stroke(g, bx, by, bw, kh, E(0.4), E(0.12), WA(0.6));
        uint32_t fg = foc ? 0xff101010u : C_WHITE;
        const char *lab = i == K_SHIFT ? "⇧" : i == K_LANG ? (k->lang ? "RU" : "EN") : i == K_SYM ? "#&" : i == K_SPACE ? tr(S_KB_SPACE) : i == K_DONE ? tr(S_KB_DONE) : NULL;
        if (i == K_SHIFT) d_icon(g, I_UP, bx + bw / 2, by + kh / 2, E(1.3), fg);
        else if (lab) d_text_c(g, bx, bw, by + (kh - d_line_h(1.2, F_S)) / 2, 1.2, F_S, fg, lab);
        else d_icon(g, I_BACKSPACE, bx + bw / 2, by + kh / 2, E(1.6), fg);
    }
}
int kb_height(void) { return 5 * E(3.2) + 4 * E(0.4); }

