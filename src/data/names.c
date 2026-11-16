#include "names.h"
#include "core/util.h"
#include <stdio.h>
#include <string.h>

static int ascii_sep(uint32_t c) {
    return c <= ' ' || (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}
static int badge(uint32_t c) {
    return c == 0x24e2 || c == 0x24d6 || c == 0x24e5 || c == 0x24de || c == 0x24c8 || c == 0x24bc || c == 0x24cb || c == 0x24c4;
}

void names_key(const char *s, char *out, size_t cap) {
    size_t n = 0;
    uint32_t c;
    while ((c = u8_next(&s)) != 0) {
        if (ascii_sep(c)) continue;
        char t[4];
        int k = u8_put(t, u8_lower(c));
        if (n + (size_t)k + 1 > cap) break;
        memcpy(out + n, t, (size_t)k);
        n += (size_t)k;
    }
    if (cap) out[n] = 0;
}

void names_clean(char *s) {
    char *o = s;
    const char *p = s;
    int space = 0;
    uint32_t c;
    while (*p) {
        const char *q = p;
        c = u8_next(&q);
        if (badge(c) || c == ' ' || c == '\t') { space = 1; p = q; continue; }
        if (space && o != s) *o++ = ' ';
        space = 0;
        while (p < q) *o++ = *p++;
    }
    *o = 0;
}

/* Работаем с массивом кодов: так проще повторить регулярные выражения веб-версии. */
typedef struct { uint32_t c[256]; int n; } cps;
static void to_cps(const char *s, cps *v) {
    v->n = 0;
    uint32_t c;
    while ((c = u8_next(&s)) != 0 && v->n < 256) v->c[v->n++] = u8_lower(c);
}
static void from_cps(const cps *v, char *out, size_t cap) {
    size_t n = 0;
    for (int i = 0; i < v->n; i++) {
        char t[4];
        int k = u8_put(t, v->c[i]);
        if (n + (size_t)k + 1 > cap) break;
        memcpy(out + n, t, (size_t)k);
        n += (size_t)k;
    }
    if (cap) out[n] = 0;
}
static int eq_at(const cps *v, int at, const char *word) {   /* word — UTF-8 в нижнем регистре */
    uint32_t c;
    int i = at;
    while ((c = u8_next(&word)) != 0) { if (i >= v->n || v->c[i] != c) return 0; i++; }
    return i - at;
}
static void cut(cps *v, int from, int to) {
    memmove(v->c + from, v->c + to, (size_t)(v->n - to) * sizeof v->c[0]);
    v->n -= to - from;
}
static int is_sp(uint32_t c) { return c == ' ' || c == '\t' || c == 0xa0; }
static int is_sign(uint32_t c) { return is_sp(c) || c == '!' || c == '.' || c == ',' || c == '(' || c == ')' || c == 0x2013 || c == '-' || badge(c); }

void names_short(const char *name, char *out, size_t cap) {
    cps v;
    to_cps(name, &v);
    /* \s+\(архив\)$ */
    {
        int k = 0;
        const char *a = "(архив)";
        int len = 7;
        if (v.n >= len && (k = eq_at(&v, v.n - len, a)) == len) {
            int s = v.n - len;
            int b = s;
            while (b > 0 && is_sp(v.c[b - 1])) b--;
            if (b < s) v.n = b;
        }
    }
    /* \s+\((\+\d+)\) -> " $1" */
    for (int i = 0; i < v.n; i++) {
        if (!is_sp(v.c[i])) continue;
        int j = i;
        while (j < v.n && is_sp(v.c[j])) j++;
        if (j + 2 < v.n && v.c[j] == '(' && v.c[j + 1] == '+' && v.c[j + 2] >= '0' && v.c[j + 2] <= '9') {
            int e = j + 2;
            while (e < v.n && v.c[e] >= '0' && v.c[e] <= '9') e++;
            if (e < v.n && v.c[e] == ')') {
                cut(&v, e, e + 1);            /* ')' */
                cut(&v, j, j + 1);            /* '(' */
                cut(&v, i + 1, j);            /* пробелы -> один */
            }
        }
    }
    /* ^телеканал\s+ */
    {
        int k = eq_at(&v, 0, "телеканал");
        if (k && k < v.n && is_sp(v.c[k])) { int e = k; while (e < v.n && is_sp(v.c[e])) e++; cut(&v, 0, e); }
    }
    /* знаки -> пробел (последовательность — один пробел), обрезка по краям */
    {
        cps w;
        w.n = 0;
        for (int i = 0; i < v.n;) {
            if (is_sign(v.c[i])) { while (i < v.n && is_sign(v.c[i])) i++; w.c[w.n++] = ' '; }
            else w.c[w.n++] = v.c[i++];
        }
        int a = 0, b = w.n;
        while (a < b && w.c[a] == ' ') a++;
        while (b > a && w.c[b - 1] == ' ') b--;
        v.n = 0;
        for (int i = a; i < b; i++) v.c[v.n++] = w.c[i];
    }
    /* \s(канал|тв)(\s.+|\s*)$ -> $2 : « канал» или « тв» в конце или перед « хвост» */
    for (int i = 0; i < v.n; i++) {
        if (v.c[i] != ' ') continue;
        int k = eq_at(&v, i + 1, "канал");
        if (!k) k = eq_at(&v, i + 1, "тв");
        if (!k) continue;
        int e = i + 1 + k;
        if (e == v.n) { v.n = i; break; }
        if (v.c[e] == ' ' && e + 1 < v.n) { cut(&v, i, e); break; }
    }
    /* \s(50|orig|original)$ */
    {
        const char *w[] = {"50", "orig", "original"};
        for (int t = 0; t < 3; t++) {
            int len = (int)strlen(w[t]);
            if (v.n > len && v.c[v.n - len - 1] == ' ' && eq_at(&v, v.n - len, w[t]) == len) { v.n -= len + 1; break; }
        }
    }
    /* \s(\d+) -> $1 */
    for (int i = 0; i + 1 < v.n; i++) if (v.c[i] == ' ' && v.c[i + 1] >= '0' && v.c[i + 1] <= '9') { cut(&v, i, i + 1); i--; }
    from_cps(&v, out, cap);
}

static const char *translit(uint32_t c) {
    switch (c) {
    case 0x451: return "e";  case 0x443: return "y";  case 0x43a: return "k";  case 0x435: return "e";
    case 0x43d: return "h";  case 0x448: return "w";  case 0x437: return "3";  case 0x445: return "x";
    case 0x44b: return "bl"; case 0x432: return "b";  case 0x430: return "a";  case 0x440: return "p";
    case 0x43e: return "o";  case 0x447: return "4";  case 0x441: return "c";  case 0x43c: return "m";
    case 0x442: return "t";  case 0x44c: return "b";  case 0x431: return "6";
    default: return NULL;
    }
}

void names_match_key(const char *name, char *out, size_t cap) {
    char s[512];
    names_short(name, s, sizeof s);
    size_t n = 0;
    const char *p = s;
    uint32_t c;
    while ((c = u8_next(&p)) != 0) {
        const char *t = translit(c);
        char b[4];
        int k;
        if (t) { k = (int)strlen(t); memcpy(b, t, (size_t)k); }
        else k = u8_put(b, c);
        if (n + (size_t)k + 1 > cap) break;
        memcpy(out + n, b, (size_t)k);
        n += (size_t)k;
    }
    if (cap) out[n] = 0;
}

void names_monogram(const char *name, char *out, size_t cap) {
    uint32_t sb[4];
    int sn = 0, words = 0;
    uint32_t first[8];
    int fn = 0;
    const char *p = name;
    while (*p) {
        const char *q = p;
        uint32_t c = u8_next(&q);
        if (!u8_is_letter(c)) { p = q; continue; }
        /* слово */
        int digit = c >= '0' && c <= '9';
        int took = 0;
        int wl = 0;
        while (*p) {
            const char *r = p;
            uint32_t d = u8_next(&r);
            if (!u8_is_letter(d)) break;
            if (words == 0 && fn < 8) first[fn++] = d;
            if (sn < 3 && (digit ? 1 : !took)) { sb[sn++] = d; took = 1; }
            wl++;
            p = r;
        }
        (void)wl;
        words++;
    }
    uint32_t res[4];
    int rn = 0;
    if (words == 0) { snprintf(out, cap, "?"); return; }
    if (words == 1) { int k = fn <= 3 ? fn : 2; for (int i = 0; i < k; i++) res[rn++] = first[i]; }
    else for (int i = 0; i < sn; i++) res[rn++] = sb[i];
    size_t n = 0;
    for (int i = 0; i < rn; i++) {
        uint32_t c = res[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        else if (c >= 0x430 && c <= 0x44f) c -= 32;
        else if (c >= 0x450 && c <= 0x45f) c -= 80;
        char t[4];
        int k = u8_put(t, c);
        if (n + (size_t)k + 1 > cap) break;
        memcpy(out + n, t, (size_t)k);
        n += (size_t)k;
    }
    if (cap) out[n] = 0;
}

uint32_t names_color(const char *key) {
    static const uint32_t pal[] = {0xff4a5d7e, 0xff6b4f7a, 0xff3f6b5e, 0xff7a5a3f, 0xff5a6b3f, 0xff3f5f7a, 0xff7a3f4f, 0xff4f4f7a,
                                   0xff3f6f6f, 0xff6f5f3f};
    return pal[hash_str(key) % (sizeof pal / sizeof pal[0])];
}
