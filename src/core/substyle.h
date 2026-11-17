/* Оформление внутри текста субтитров (WebVTT, TTML, CEA-608 / 708): управляющие байты прямо в строке UTF-8 —
 * хранение и передача реплик остаются строками, рисует ui/subs.c.
 *   SS_SET + флаги (0…7) — начертание с этого места: SS_I курсив, SS_B жирный, SS_U подчёркнутый (байты 0x01…0x08);
 *   SS_COLOR и шесть шестнадцатеричных цифр «rrggbb» — цвет знаков, SS_COLOR_DEF — цвет по умолчанию.
 * Каждая строка (после \n) начинается обычным начертанием и цветом по умолчанию: кто пишет оформление, повторяет его
 * после перевода строки (ss_out). Байты 0x01…0x08, 0x0E, 0x0F в тексте субтитров иначе не встречаются — входной
 * текст от них чистится. */
#ifndef PTV_SUBSTYLE_H
#define PTV_SUBSTYLE_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { SS_I = 1, SS_B = 2, SS_U = 4 };
#define SS_SET 0x01
#define SS_COLOR 0x0E
#define SS_COLOR_DEF 0x0F
#define SS_NOCOLOR 0xFFFFFFFFu   /* «цвет по умолчанию» */

static inline int ss_is_mark(unsigned char c) { return (c >= SS_SET && c <= SS_SET + 7) || c == SS_COLOR || c == SS_COLOR_DEF; }

/* длина метки с начала s (0 — не метка) */
static inline int ss_mark_len(const char *s) {
    unsigned char c = (unsigned char)*s;
    if (c >= SS_SET && c <= SS_SET + 7) return 1;
    if (c == SS_COLOR_DEF) return 1;
    if (c == SS_COLOR) { int k = 1; while (k < 7 && s[k]) k++; return k; }
    return 0;
}

/* разобрать метку: флаги / цвет меняются по ней */
static inline void ss_apply(const char *s, int *flags, uint32_t *color) {
    unsigned char c = (unsigned char)*s;
    if (c >= SS_SET && c <= SS_SET + 7) *flags = c - SS_SET;
    else if (c == SS_COLOR_DEF) *color = SS_NOCOLOR;
    else if (c == SS_COLOR) {
        uint32_t v = 0;
        int k;
        for (k = 1; k < 7 && s[k]; k++) {
            char h = s[k];
            v = v << 4 | (uint32_t)(h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
        }
        *color = k == 7 ? 0xff000000u | v : SS_NOCOLOR;
    }
}

/* текст без меток (тесты, ширина без оформления) */
static inline void ss_plain(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < cap;) {
        int l = ss_mark_len(p);
        if (l) { p += l; continue; }
        dst[o++] = *p++;
    }
    if (cap) dst[o] = 0;
}

/* есть ли видимый текст (не только пробелы, переводы строк и метки) */
static inline int ss_visible(const char *s) {
    for (; *s; s++) if (!ss_is_mark((unsigned char)*s) && *s != ' ' && *s != '\n') return 1;
    return 0;
}

/* Писатель оформленного текста: flags / color — оформление следующего знака; ss_out_marks отдаёт метки, нужные перед
 * ним (только при смене; после \n — заново, пробел без подчёркивания меток не требует). */
typedef struct { int flags; uint32_t color; int out_flags; uint32_t out_color; } ss_out;
static inline void ss_out_init(ss_out *w) { w->flags = w->out_flags = 0; w->color = w->out_color = SS_NOCOLOR; }
/* buf — не меньше 9 байт; длина меток */
static inline int ss_out_marks(ss_out *w, char c, char *buf) {
    int n = 0;
    if (c == '\n') { w->out_flags = 0; w->out_color = SS_NOCOLOR; return 0; }
    if (c == ' ' && !(w->flags & SS_U) && !(w->out_flags & SS_U)) return 0;
    if (w->flags != w->out_flags) { buf[n++] = (char)(SS_SET + w->flags); w->out_flags = w->flags; }
    if (w->color != w->out_color) {
        if (w->color == SS_NOCOLOR) buf[n++] = SS_COLOR_DEF;
        else { buf[n++] = SS_COLOR; snprintf(buf + n, 7, "%06x", (unsigned)(w->color & 0xffffff)); n += 6; }
        w->out_color = w->color;
    }
    return n;
}
#endif
