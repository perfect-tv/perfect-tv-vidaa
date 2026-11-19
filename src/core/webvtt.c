#define _GNU_SOURCE
#include "webvtt.h"
#include "http.h"
#include "util.h"
#include "substyle.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define MAX_CUES 3000
#define MAX_SEEN 2000

typedef struct { int64_t s, e; char *t; } cue;
typedef struct job { char url[4096], hdr[2048]; volatile int cancel; int gen; } job;

static struct {
    pthread_mutex_t mu;
    job *j;                 /* текущая задача (NULL — нет) */
    int gen;
    cue *c;
    int n, cap;
    int64_t frame_pts;
    double pos;
} W = {.mu = PTHREAD_MUTEX_INITIALIZER, .frame_pts = -1};

/* ---------- разбор .vtt ---------- */
/* «[чч:]мм:сс.ттт» -> секунды; -1 — не время */
static double vtime(const char *s, const char **end) {
    int part[3] = {0, 0, 0}, np = 0;
    const char *p = s;
    while (np < 3) {
        if (*p < '0' || *p > '9') return -1;
        int v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        part[np++] = v;
        if (*p == ':') { p++; continue; }
        break;
    }
    if (*p != '.' && *p != ',') return -1;
    p++;
    int ms = 0, k = 0;
    while (*p >= '0' && *p <= '9' && k < 3) { ms = ms * 10 + (*p++ - '0'); k++; }
    while (k++ < 3) ms *= 10;
    while (*p >= '0' && *p <= '9') p++;
    if (end) *end = p;
    double h = np == 3 ? part[0] : 0, m = np == 3 ? part[1] : part[0], sec = np == 3 ? part[2] : part[1];
    return h * 3600 + m * 60 + sec + ms / 1000.0;
}

/* знак c при оформлении w — в out (с метками перед ним, core/substyle.h) */
static void ss_put(sbuf *out, ss_out *w, char c) {
    char m[9];
    int k = ss_out_marks(w, c, m);
    if (k) sb_addn(out, m, (size_t)k);
    sb_addc(out, c);
}

/* цвет класса WebVTT «c.yellow» (классы фона «bg_…» и чужие — нет) */
static uint32_t vtt_class_color(const char *t, const char *e) {
    static const struct { const char *n; uint32_t c; } C[] = {{"white", 0xffffffffu}, {"lime", 0xff00ff00u}, {"cyan", 0xff00ffffu}, {"red", 0xffff0000u},
                                                              {"yellow", 0xffffff00u}, {"magenta", 0xffff00ffu}, {"blue", 0xff0000ffu}, {"black", 0xff000000u}};
    uint32_t r = SS_NOCOLOR;
    for (const char *p = t; p < e; p++) {
        if (*p != '.') continue;
        size_t l = strcspn(p + 1, ". \t>");
        if (p + 1 + l > e) l = (size_t)(e - p - 1);
        for (int i = 0; i < COUNT(C); i++) if (strlen(C[i].n) == l && !strncmp(p + 1, C[i].n, l)) r = C[i].c;
    }
    return r;
}

/* Текст реплики: <i> <b> <u> и цвет <c.класс> — метками оформления, прочие теги (<v>, <lang>, <ruby>, метки времени)
 * прочь, сущности — знаками. */
static void vtt_styled(sbuf *out, const char *s) {
    ss_out w;
    ss_out_init(&w);
    struct { int f; uint32_t c; } st[16];
    int d = 0;
    for (const char *p = s; *p;) {
        if (*p == '<') {
            const char *e = strchr(p, '>');
            if (e) {
                const char *t = p + 1;
                p = e + 1;
                if (*t == '/') { if (d > 0) { d--; w.flags = st[d].f; w.color = st[d].c; } continue; }
                if (*t >= '0' && *t <= '9') continue;   /* метка времени */
                if (d < 16) { st[d].f = w.flags; st[d].c = w.color; d++; }
                size_t nl = strcspn(t, ". \t>");
                if (nl == 1 && *t == 'i') w.flags |= SS_I;
                else if (nl == 1 && *t == 'b') w.flags |= SS_B;
                else if (nl == 1 && *t == 'u') w.flags |= SS_U;
                else if (nl == 1 && *t == 'c') { uint32_t c = vtt_class_color(t, e); if (c != SS_NOCOLOR) w.color = c; }
                continue;
            }
        }
        if (*p == '&') {
            static const struct { const char *e, *v; } ent[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&nbsp;", " "}, {"&quot;", "\""}, {"&apos;", "'"}, {"&lrm;", ""}, {"&rlm;", ""}};
            int done = 0;
            for (size_t i = 0; i < sizeof ent / sizeof ent[0] && !done; i++) {
                size_t l = strlen(ent[i].e);
                if (!strncmp(p, ent[i].e, l)) { for (const char *v = ent[i].v; *v; v++) ss_put(out, &w, *v); p += l; done = 1; }
            }
            if (done) continue;
        }
        if (ss_is_mark((unsigned char)*p)) { p++; continue; }
        ss_put(out, &w, *p++);
    }
}

/* Блоки разделены пустыми строками. Первый — заголовок (WEBVTT, X-TIMESTAMP-MAP). В блоке реплики строка с «-->»
 * — время, после неё до пустой строки — текст; строка до неё — необязательный идентификатор. NOTE / STYLE — без
 * времени, пропускаются. */
int webvtt_parse(const char *text, size_t n, webvtt_cue_fn cb, void *ctx) {
    int64_t mpegts = 0;
    double local = 0, a = -1, b = -1;
    int count = 0, header = 1, in_text = 0;
    sbuf body = {0};
    const char *p = text, *end = text + n;
    while (p <= end) {
        const char *e = memchr(p, '\n', (size_t)(end - p));
        if (!e) e = end;
        size_t l = (size_t)(e - p);
        if (l && p[l - 1] == '\r') l--;
        char line[1024];
        if (l >= sizeof line) l = sizeof line - 1;
        memcpy(line, p, l);
        line[l] = 0;
        int blank = !l;
        if (header) {
            char *m = strstr(line, "X-TIMESTAMP-MAP=");
            if (m) {
                char *mp = strstr(m, "MPEGTS:"), *lc = strstr(m, "LOCAL:");
                if (mp) mpegts = strtoll(mp + 7, NULL, 10);
                if (lc) { double v = vtime(lc + 6, NULL); if (v >= 0) local = v; }
            }
            if (blank || strstr(line, "-->")) header = 0;
            if (blank) { p = e + 1; continue; }
        }
        if (blank || e == end) {   /* конец блока: реплика готова */
            if (!blank && in_text) { if (body.n) sb_addc(&body, '\n'); sb_add(&body, line); }
            if (in_text && a >= 0 && b > a && body.s) {
                sbuf st = {0};
                vtt_styled(&st, body.s);
                if (st.s) trim(st.s);
                if (st.s && ss_visible(st.s)) {
                    cb(ctx, mpegts + (int64_t)((a - local) * 90000.0), mpegts + (int64_t)((b - local) * 90000.0), st.s);
                    count++;
                }
                sb_free(&st);
            }
            sb_clear(&body);
            in_text = 0;
            a = b = -1;
        } else if (!in_text && strstr(line, "-->")) {
            char *q = strstr(line, "-->") + 3;
            while (*q == ' ') q++;
            a = vtime(line, NULL);
            b = vtime(q, NULL);
            in_text = 1;
        } else if (in_text) {
            if (body.n) sb_addc(&body, '\n');
            sb_add(&body, line);
        }
        if (e == end) break;
        p = e + 1;
    }
    sb_free(&body);
    return count;
}

/* ---------- TTML / IMSC (текстовый профиль) ----------
 * Свой маленький разбор XML: элементы body / div / p / span с begin / end / dur (время ребёнка — от начала родителя,
 * timeContainer «par»), текст абзаца — его текст и span, <br/> — перевод строки, пробелы сжимаются. Время — часы
 * «чч:мм:сс(.доли)» и «чч:мм:сс:кадры», смещения «12.5s», «300ms», «40f», «1200t» (ttp:tickRate, ttp:frameRate,
 * ttp:frameRateMultiplier). Оформление (курсив, жирный, подчёркивание, цвет — tts: на элементе и через
 * <style>) — метками core/substyle.h; области не берутся — текст внизу, как у остальных субтитров. Картинки
 * (профиль image) — нет. */
typedef struct { double tick, fps; } ttp;

static double ttml_time(const char *s, const ttp *tp) {
    while (*s == ' ') s++;
    if (!*s) return -1;
    int colons = 0;
    for (const char *p = s; *p; p++) colons += *p == ':';
    if (colons >= 2) {
        double h = atof(s), m, sec, fr = 0;
        const char *p = strchr(s, ':') + 1;
        m = atof(p);
        p = strchr(p, ':') + 1;
        sec = atof(p);
        const char *q = strchr(p, ':');
        if (q) fr = atof(q + 1);
        return h * 3600 + m * 60 + sec + fr / tp->fps;
    }
    char *e;
    double v = strtod(s, &e);
    if (e == s) return -1;
    if (!strncmp(e, "ms", 2)) return v / 1000;
    if (*e == 'h') return v * 3600;
    if (*e == 'm') return v * 60;
    if (*e == 'f') return v / tp->fps;
    if (*e == 't') return v / tp->tick;
    return v;   /* «s» или без единицы */
}

/* значение атрибута (с префиксом пространства имён или без): name="…" */
static int xml_attr(const char *tag, size_t tl, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    for (const char *p = tag; p + nl < tag + tl; p++) {
        if (strncmp(p, name, nl) || (p[-1] != ' ' && p[-1] != ':' && p[-1] != '\t' && p[-1] != '\n' && p[-1] != '\r')) continue;
        const char *q = p + nl;
        while (*q == ' ') q++;
        if (*q != '=') continue;
        q++;
        while (*q == ' ') q++;
        char quote = *q;
        if (quote != '"' && quote != '\'') continue;
        const char *v = q + 1, *e = memchr(v, quote, (size_t)(tag + tl - v));
        if (!e) return 0;
        size_t l = (size_t)(e - v) < cap - 1 ? (size_t)(e - v) : cap - 1;
        memcpy(out, v, l);
        out[l] = 0;
        return 1;
    }
    return 0;
}

/* знак текста TTML: с метками оформления (w), \n сбрасывает их */
static void tput(sbuf *b, ss_out *w, const char *s, size_t n) {
    char m[9];
    int k = ss_out_marks(w, s[0], m);
    if (k) sb_addn(b, m, (size_t)k);
    sb_addn(b, s, n);
}

/* цвет tts:color: «#rrggbb[aa]», «rgb(…)» / «rgba(…)», имена TTML; SS_NOCOLOR — не понят или прозрачный */
static uint32_t tts_color(const char *v) {
    static const struct { const char *n; uint32_t c; } N[] = {
        {"white", 0xffffff}, {"black", 0x000000}, {"red", 0xff0000}, {"lime", 0x00ff00}, {"green", 0x008000}, {"blue", 0x0000ff},
        {"yellow", 0xffff00}, {"cyan", 0x00ffff}, {"aqua", 0x00ffff}, {"magenta", 0xff00ff}, {"fuchsia", 0xff00ff}, {"gray", 0x808080},
        {"silver", 0xc0c0c0}, {"maroon", 0x800000}, {"olive", 0x808000}, {"navy", 0x000080}, {"purple", 0x800080}, {"teal", 0x008080}};
    while (*v == ' ') v++;
    unsigned r, g, b, a = 255;
    if (*v == '#') {
        size_t l = strspn(v + 1, "0123456789abcdefABCDEF");
        if (l != 6 && l != 8) return SS_NOCOLOR;
        unsigned long x = strtoul(v + 1, NULL, 16);
        if (l == 8) { a = (unsigned)(x & 0xff); x >>= 8; }
        return a ? 0xff000000u | (uint32_t)x : SS_NOCOLOR;
    }
    if (sscanf(v, "rgba(%u,%u,%u,%u)", &r, &g, &b, &a) == 4 || sscanf(v, "rgb(%u,%u,%u)", &r, &g, &b) == 3)
        return a && r < 256 && g < 256 && b < 256 ? 0xff000000u | r << 16 | g << 8 | b : SS_NOCOLOR;
    for (int i = 0; i < COUNT(N); i++) if (!strcasecmp(v, N[i].n)) return 0xff000000u | N[i].c;
    return SS_NOCOLOR;
}

/* оформление из атрибутов tts: (элемент или <style>): mask — какие флаги заданы, val — их значения */
static void tts_attrs(const char *tag, size_t tl, int *mask, int *val, uint32_t *color) {
    char v[64];
    if (xml_attr(tag, tl, "fontStyle", v, sizeof v)) { *mask |= SS_I; if (!strcmp(v, "italic") || !strcmp(v, "oblique")) *val |= SS_I; else *val &= ~SS_I; }
    if (xml_attr(tag, tl, "fontWeight", v, sizeof v)) { *mask |= SS_B; if (!strcmp(v, "bold")) *val |= SS_B; else *val &= ~SS_B; }
    if (xml_attr(tag, tl, "textDecoration", v, sizeof v)) {
        if (strstr(v, "noUnderline")) { *mask |= SS_U; *val &= ~SS_U; }
        else if (strstr(v, "underline")) { *mask |= SS_U; *val |= SS_U; }
    }
    if (xml_attr(tag, tl, "color", v, sizeof v)) { uint32_t c = tts_color(v); if (c != SS_NOCOLOR) *color = c; }
}

/* текст XML: сущности — знаками, пробелы — одним */
static void xml_text(sbuf *b, ss_out *w, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '&') {
            const char *e = memchr(s + i, ';', n - i);
            if (e && e - (s + i) < 12) {
                size_t l = (size_t)(e - (s + i)) + 1;
                unsigned cp = 0;
                if (!strncmp(s + i, "&amp;", l)) cp = '&';
                else if (!strncmp(s + i, "&lt;", l)) cp = '<';
                else if (!strncmp(s + i, "&gt;", l)) cp = '>';
                else if (!strncmp(s + i, "&quot;", l)) cp = '"';
                else if (!strncmp(s + i, "&apos;", l)) cp = '\'';
                else if (s[i + 1] == '#') cp = (unsigned)(s[i + 2] == 'x' || s[i + 2] == 'X' ? strtoul(s + i + 3, NULL, 16) : strtoul(s + i + 2, NULL, 10));
                if (cp) {
                    char u[4];
                    int k = cp < 0x80 ? (u[0] = (char)cp, 1)
                          : cp < 0x800 ? (u[0] = (char)(0xC0 | cp >> 6), u[1] = (char)(0x80 | (cp & 63)), 2)
                          : cp < 0x10000 ? (u[0] = (char)(0xE0 | cp >> 12), u[1] = (char)(0x80 | ((cp >> 6) & 63)), u[2] = (char)(0x80 | (cp & 63)), 3)
                          : (u[0] = (char)(0xF0 | cp >> 18), u[1] = (char)(0x80 | ((cp >> 12) & 63)), u[2] = (char)(0x80 | ((cp >> 6) & 63)), u[3] = (char)(0x80 | (cp & 63)), 4);
                    if (!ss_is_mark((unsigned char)u[0])) tput(b, w, u, (size_t)k);
                    i += l - 1;
                    continue;
                }
            }
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (b->n && b->s[b->n - 1] != ' ' && b->s[b->n - 1] != '\n') tput(b, w, " ", 1);
            continue;
        }
        if (ss_is_mark((unsigned char)c)) continue;
        tput(b, w, &c, 1);
    }
}

/* пробелы у переводов строк и по краям */
static void tidy(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == ' ' && (o == s || o[-1] == '\n' || p[1] == '\n' || !p[1])) continue;
        *o++ = *p;
    }
    *o = 0;
    while (o > s && (o[-1] == '\n' || o[-1] == ' ')) *--o = 0;
}

int ttml_parse(const char *xml, size_t n, double base_sec, webvtt_cue_fn cb, void *ctx) {
    ttp tp = {1, 30};
    struct { double b, e; int f; uint32_t c; } st[48];
    struct { char id[32]; int mask, val; uint32_t c; } sty[32];   /* <style xml:id> из <styling> */
    int nsty = 0;
    int depth = 0, count = 0, p_depth = -1;
    double pb = 0, pe = 0;
    sbuf text = {0};
    ss_out w;
    ss_out_init(&w);
    st[0].b = base_sec;
    st[0].e = 1e12;
    st[0].f = 0;
    st[0].c = SS_NOCOLOR;
    const char *p = xml, *end = xml + n;
    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt) lt = end;
        if (p_depth >= 0 && lt > p) { w.flags = st[depth].f; w.color = st[depth].c; xml_text(&text, &w, p, (size_t)(lt - p)); }
        if (lt >= end) break;
        if (lt + 4 <= end && !strncmp(lt, "<!--", 4)) { const char *e = memmem(lt, (size_t)(end - lt), "-->", 3); p = e ? e + 3 : end; continue; }
        const char *gt = memchr(lt, '>', (size_t)(end - lt));
        if (!gt) break;
        const char *tag = lt + 1;
        size_t tl = (size_t)(gt - tag);
        p = gt + 1;
        if (*tag == '?' || *tag == '!') continue;
        int closing = *tag == '/', selfc = tl && tag[tl - 1] == '/';
        if (closing) { tag++; tl--; }
        /* имя без префикса пространства имён */
        size_t nl = strcspn(tag, " \t\r\n/>");
        if (nl > tl) nl = tl;
        const char *name = tag;
        const char *colon = memchr(tag, ':', nl);
        if (colon) { nl -= (size_t)(colon + 1 - tag); name = colon + 1; }
#define IS(s) (nl == strlen(s) && !strncmp(name, s, nl))
        if (closing) {
            if (depth > 0) depth--;
            if (IS("p") && p_depth >= 0 && depth <= p_depth) {
                if (text.s) {
                    tidy(text.s);
                    if (ss_visible(text.s) && pe > pb) {
                        int64_t s0 = (int64_t)(pb * 90000.0 + 0.5), e0 = (int64_t)(pe * 90000.0 + 0.5);
                        cb(ctx, s0, e0, text.s);
                        count++;
                    }
                }
                sb_clear(&text);
                p_depth = -1;
            }
            continue;
        }
        if (IS("br")) { if (p_depth >= 0) tput(&text, &w, "\n", 1); if (!selfc && depth < 47) { depth++; st[depth] = st[depth - 1]; } continue; }
        if (IS("style") && nsty < COUNT(sty)) {   /* именованный стиль: свои tts: и стили, на которые ссылается */
            char id[32], refs[256];
            if (xml_attr(tag, tl, "id", id, sizeof id)) {
                int mask = 0, val = 0;
                uint32_t c = SS_NOCOLOR;
                if (xml_attr(tag, tl, "style", refs, sizeof refs))
                    for (char *sv, *r = strtok_r(refs, " ", &sv); r; r = strtok_r(NULL, " ", &sv))
                        for (int k = 0; k < nsty; k++) if (!strcmp(sty[k].id, r)) { mask |= sty[k].mask; val = (val & ~sty[k].mask) | sty[k].val; if (sty[k].c != SS_NOCOLOR) c = sty[k].c; }
                tts_attrs(tag, tl, &mask, &val, &c);
                snprintf(sty[nsty].id, sizeof sty[nsty].id, "%s", id);
                sty[nsty].mask = mask;
                sty[nsty].val = val;
                sty[nsty].c = c;
                nsty++;
            }
        }
        if (IS("tt")) {
            char v[64];
            if (xml_attr(tag, tl, "frameRate", v, sizeof v)) tp.fps = atof(v) > 0 ? atof(v) : 30;
            if (xml_attr(tag, tl, "frameRateMultiplier", v, sizeof v)) { double a = atof(v), b = atof(strchr(v, ' ') ? strchr(v, ' ') : "1"); if (a > 0 && b > 0) tp.fps *= a / b; }
            tp.tick = tp.fps;
            if (xml_attr(tag, tl, "tickRate", v, sizeof v) && atof(v) > 0) tp.tick = atof(v);
        }
        double pbeg = depth >= 0 ? st[depth].b : base_sec, pend = st[depth].e;
        char v[64];
        double b = pbeg, e = pend;
        if (xml_attr(tag, tl, "begin", v, sizeof v)) { double t = ttml_time(v, &tp); if (t >= 0) b = pbeg + t; }
        if (xml_attr(tag, tl, "end", v, sizeof v)) { double t = ttml_time(v, &tp); if (t >= 0) e = pbeg + t; }
        else if (xml_attr(tag, tl, "dur", v, sizeof v)) { double t = ttml_time(v, &tp); if (t >= 0) e = b + t; }
        if (e > pend) e = pend;
        /* оформление: от родителя, затем style="…", затем свои tts: */
        int mask = 0, val = st[depth].f;
        uint32_t col = st[depth].c;
        char refs[256];
        if (xml_attr(tag, tl, "style", refs, sizeof refs))
            for (char *sv, *r = strtok_r(refs, " ", &sv); r; r = strtok_r(NULL, " ", &sv))
                for (int k = 0; k < nsty; k++) if (!strcmp(sty[k].id, r)) { val = (val & ~sty[k].mask) | sty[k].val; if (sty[k].c != SS_NOCOLOR) col = sty[k].c; }
        tts_attrs(tag, tl, &mask, &val, &col);
        if (IS("p") && p_depth < 0) { p_depth = depth; pb = b; pe = e; sb_clear(&text); ss_out_init(&w); }
        if (!selfc && depth < 47) { depth++; st[depth].b = b; st[depth].e = e; st[depth].f = val; st[depth].c = col; }
#undef IS
    }
    sb_free(&text);
    return count;
}

/* ---------- fMP4: субтитры stpp (TTML) и wvtt (WebVTT в MP4, ISO/IEC 14496-30) ---------- */
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

/* следующая коробка в [p, end): тип, начало содержимого и конец; 0 — конец или сломано */
static int box_next(const uint8_t **p, const uint8_t *end, char type[5], const uint8_t **body, const uint8_t **bend) {
    if (end - *p < 8) return 0;
    uint64_t sz = be32(*p);
    size_t hl = 8;
    memcpy(type, *p + 4, 4);
    type[4] = 0;
    if (sz == 1) { if (end - *p < 16) return 0; sz = be64(*p + 8); hl = 16; }
    else if (sz == 0) sz = (uint64_t)(end - *p);
    if (sz < hl || sz > (uint64_t)(end - *p)) return 0;
    *body = *p + hl;
    *bend = *p + sz;
    *p += sz;
    return 1;
}

static void init_walk(const uint8_t *p, const uint8_t *end, mp4sub_init *in, uint32_t *ts) {
    char t[5];
    const uint8_t *b, *be;
    while (box_next(&p, end, t, &b, &be)) {
        if (!strcmp(t, "moov") || !strcmp(t, "trak") || !strcmp(t, "mdia") || !strcmp(t, "minf") || !strcmp(t, "stbl")) init_walk(b, be, in, ts);
        else if (!strcmp(t, "mdhd") && be - b >= 24) *ts = b[0] == 1 ? (be - b >= 24 ? be32(b + 20) : 0) : be32(b + 12);
        else if (!strcmp(t, "stsd") && be - b >= 8 && !in->kind) {
            const uint8_t *q = b + 8;
            char et[5];
            const uint8_t *eb, *ee;
            if (box_next(&q, be, et, &eb, &ee)) {
                if (!strcmp(et, "stpp")) in->kind = MP4SUB_STPP;
                else if (!strcmp(et, "wvtt")) in->kind = MP4SUB_WVTT;
                if (in->kind) in->timescale = *ts;
            }
        }
    }
}

int mp4sub_init_parse(const uint8_t *d, size_t n, mp4sub_init *in) {
    memset(in, 0, sizeof *in);
    uint32_t ts = 0;
    init_walk(d, d + n, in, &ts);
    return in->kind ? 0 : -1;
}

typedef struct { webvtt_cue_fn cb; void *ctx; int n; int64_t s[64], e[64]; char *t[64]; } cue_tmp;
static void tmp_cb(void *ctx, int64_t s, int64_t e, const char *text) {
    cue_tmp *c = ctx;
    if (c->n < 64) { c->s[c->n] = s; c->e[c->n] = e; c->t[c->n] = strdup(text); c->n++; }
}

/* один образец: TTML-документ или коробки vttc */
static int sample(const uint8_t *d, size_t n, int kind, double t0, double dur, webvtt_cue_fn cb, void *ctx) {
    int count = 0;
    if (!kind) kind = n && d[0] == '<' ? MP4SUB_STPP : MP4SUB_WVTT;
    if (kind == MP4SUB_STPP) {
        /* время в документе — по шкале дорожки; бывает и от начала образца: тогда все реплики внутри его длины */
        cue_tmp c = {0};
        ttml_parse((const char *)d, n, 0, tmp_cb, &c);
        int relative = t0 > 1 && c.n;
        for (int i = 0; i < c.n; i++) relative &= c.e[i] <= (int64_t)((dur + 1) * 90000.0);
        int64_t off = relative ? (int64_t)(t0 * 90000.0 + 0.5) : 0;
        for (int i = 0; i < c.n; i++) { cb(ctx, c.s[i] + off, c.e[i] + off, c.t[i]); free(c.t[i]); count++; }
        return count;
    }
    const uint8_t *p = d, *end = d + n, *b, *be;
    char t[5];
    while (box_next(&p, end, t, &b, &be)) {
        if (strcmp(t, "vttc")) continue;
        const uint8_t *q = b, *pb, *pe;
        char it[5];
        while (box_next(&q, be, it, &pb, &pe)) {
            if (strcmp(it, "payl")) continue;
            char *s = xstrndup((const char *)pb, (size_t)(pe - pb));
            if (!s) continue;
            sbuf st = {0};
            vtt_styled(&st, s);
            if (st.s) trim(st.s);
            if (st.s && ss_visible(st.s)) { cb(ctx, (int64_t)(t0 * 90000.0 + 0.5), (int64_t)((t0 + dur) * 90000.0 + 0.5), st.s); count++; }
            sb_free(&st);
            free(s);
        }
    }
    return count;
}

int mp4sub_parse(const uint8_t *d, size_t n, const mp4sub_init *in, webvtt_cue_fn cb, void *ctx) {
    double ts = in && in->timescale ? in->timescale : 1000;
    int kind = in ? in->kind : 0, count = 0;
    const uint8_t *p = d, *end = d + n;
    char t[5];
    const uint8_t *b, *be;
    while (box_next(&p, end, t, &b, &be)) {
        if (strcmp(t, "moof")) continue;
        const uint8_t *moof = b - 8;
        const uint8_t *q = b, *tb, *te;
        char tt[5];
        while (box_next(&q, be, tt, &tb, &te)) {
            if (strcmp(tt, "traf")) continue;
            uint64_t base = 0, data_base = (uint64_t)(moof - d);
            uint32_t def_dur = 0, def_size = 0;
            const uint8_t *r = tb, *xb, *xe;
            char xt[5];
            /* сначала tfhd и tfdt, потом trun */
            while (box_next(&r, te, xt, &xb, &xe)) {
                if (!strcmp(xt, "tfhd") && xe - xb >= 8) {
                    uint32_t fl = be32(xb) & 0xFFFFFF;
                    const uint8_t *f = xb + 8;
                    if ((fl & 1) && xe - f >= 8) { data_base = be64(f); f += 8; }
                    if (fl & 2) f += 4;
                    if ((fl & 8) && xe - f >= 4) { def_dur = be32(f); f += 4; }
                    if ((fl & 0x10) && xe - f >= 4) def_size = be32(f);
                } else if (!strcmp(xt, "tfdt") && xe - xb >= 8) base = xb[0] == 1 && xe - xb >= 12 ? be64(xb + 4) : be32(xb + 4);
            }
            r = tb;
            while (box_next(&r, te, xt, &xb, &xe)) {
                if (strcmp(xt, "trun") || xe - xb < 8) continue;
                uint32_t fl = be32(xb) & 0xFFFFFF, cnt = be32(xb + 4);
                const uint8_t *f = xb + 8;
                int32_t doff = 0;
                if ((fl & 1) && xe - f >= 4) { doff = (int32_t)be32(f); f += 4; }
                if (fl & 4) { if (xe - f < 4) continue; f += 4; }
                uint64_t off;
                if (doff < 0) {
                    uint64_t back = (uint64_t)(-(int64_t)doff);
                    off = data_base >= back ? data_base - back : UINT64_MAX;
                } else {
                    off = data_base > UINT64_MAX - (uint64_t)doff ? UINT64_MAX : data_base + (uint64_t)doff;
                }
                uint64_t tm = base;
                /* trun без полей и без размера по умолчанию не двигает ни f, ни off — цикл был бы до 4·10⁹ */
                if (!(fl & (0x100 | 0x200 | 0x400 | 0x800)) && !def_size) continue;
                if (cnt > (1u << 20)) continue;
                for (uint32_t i = 0; i < cnt && f <= xe; i++) {
                    uint32_t dur = def_dur, size = def_size;
                    size_t left = (size_t)(xe - f);
                    if (fl & 0x100) { if (left < 4) break; dur = be32(f); f += 4; left -= 4; }
                    if (fl & 0x200) { if (left < 4) break; size = be32(f); f += 4; left -= 4; }
                    if (fl & 0x400) { if (left < 4) break; f += 4; left -= 4; }
                    if (fl & 0x800) { if (left < 4) break; f += 4; }
                    if (size && off <= n && (uint64_t)size <= (uint64_t)n - off)
                        count += sample(d + off, size, kind, tm / ts, dur / ts, cb, ctx);
                    if (off > UINT64_MAX - size) break;
                    off += size;
                    tm += dur;
                }
            }
        }
    }
    return count;
}

/* сегмент любого вида: WebVTT, TTML или fMP4 */
int webvtt_segment(const char *d, size_t n, const mp4sub_init *in, webvtt_cue_fn cb, void *ctx) {
    size_t i = 0;
    if (n >= 3 && (unsigned char)d[0] == 0xEF && (unsigned char)d[1] == 0xBB && (unsigned char)d[2] == 0xBF) i = 3;
    while (i < n && (d[i] == ' ' || d[i] == '\r' || d[i] == '\n' || d[i] == '\t')) i++;
    if (n - i >= 6 && !strncmp(d + i, "WEBVTT", 6)) return webvtt_parse(d, n, cb, ctx);
    if (i < n && d[i] == '<') return ttml_parse(d + i, n - i, 0, cb, ctx);
    if (n >= 8) {
        char t[5];
        memcpy(t, d + 4, 4);
        t[4] = 0;
        if (!strcmp(t, "ftyp") || !strcmp(t, "styp") || !strcmp(t, "moof") || !strcmp(t, "sidx") || !strcmp(t, "emsg") || !strcmp(t, "prft") ||
            !strcmp(t, "moov"))
            return mp4sub_parse((const uint8_t *)d, n, in, cb, ctx);
    }
    return webvtt_parse(d, n, cb, ctx);
}

/* ---------- реплики ---------- */
static void add_cue(void *ctx, int64_t s, int64_t e, const char *text) {
    job *j = ctx;
    pthread_mutex_lock(&W.mu);
    if (W.j == j) {
        int dup = 0;
        for (int i = W.n - 1; i >= 0 && i >= W.n - 200 && !dup; i--) dup = W.c[i].s == s && !strcmp(W.c[i].t, text);
        if (!dup) {
            if (W.n == MAX_CUES) {   /* самые старые — прочь */
                free(W.c[0].t);
                memmove(W.c, W.c + 1, (size_t)(W.n - 1) * sizeof *W.c);
                W.n--;
            }
            if (W.n == W.cap) {
                int cap = W.cap ? W.cap * 2 : 256;
                cue *n2 = realloc(W.c, (size_t)cap * sizeof *n2);
                if (n2) { W.c = n2; W.cap = cap; }
            }
            char *t = W.n < W.cap ? strdup(text) : NULL;
            if (t) W.c[W.n++] = (cue){s, e, t};
        }
    }
    pthread_mutex_unlock(&W.mu);
}

static char *get(job *j, const char *url, size_t *len) {
    char err[120] = "";
    hs_req rq = {.url = url, .headers = j->hdr[0] ? j->hdr : NULL, .timeout_ms = 10000, .cancel = &j->cancel, .keepalive = 1, .cookies = 1};
    hs *h = hs_open(&rq, err, sizeof err);
    if (!h) return NULL;
    sbuf b = {0};
    char buf[16384];
    long r;
    while ((r = hs_read(h, buf, sizeof buf)) > 0 && b.n < (4u << 20)) sb_addn(&b, buf, (size_t)r);
    hs_close(h);
    *len = b.n;
    return r < 0 ? (sb_free(&b), NULL) : sb_take(&b);
}

static void nap(job *j, double s) {
    for (double t = 0; t < s && !j->cancel; t += 0.1) usleep(100000);
}

static void *worker(void *arg) {
    job *j = arg;
    pthread_setname_np(pthread_self(), "ptv-vtt");
    strlist seen = {0};
    char init_url[4096] = "";
    mp4sub_init init = {0};
    while (!j->cancel) {
        size_t n = 0;
        char *pl = get(j, j->url, &n);
        if (!pl) { nap(j, 3); continue; }
        /* плейлист: сегменты с длительностью и номером */
        double target = 6, t0 = 0, pos;
        int endlist = strstr(pl, "#EXT-X-ENDLIST") != NULL;
        pthread_mutex_lock(&W.mu);
        pos = W.pos;
        pthread_mutex_unlock(&W.mu);
        char *tg = strstr(pl, "#EXT-X-TARGETDURATION:");
        if (tg) target = atof(tg + 22);
        struct { char url[4096]; double t, d; } seg[64];
        int ns = 0, total = 0;
        double dur = 0;
        char *save = NULL;
        for (char *line = strtok_r(pl, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char *cr = strchr(line, '\r');
            if (cr) *cr = 0;
            if (!strncmp(line, "#EXTINF:", 8)) { dur = atof(line + 8); continue; }
            /* fMP4 (stpp / wvtt): инициализация — шкала времени и вид дорожки, один раз на адрес */
            if (!strncmp(line, "#EXT-X-MAP:", 11)) {
                char *u = strstr(line, "URI=\"");
                char *q = u ? strchr(u + 5, '"') : NULL;
                if (q) {
                    *q = 0;
                    char abs[4096];
                    http_resolve(j->url, u + 5, abs, sizeof abs);
                    if (strcmp(abs, init_url)) {
                        size_t in_n = 0;
                        char *im = get(j, abs, &in_n);
                        if (im) {
                            mp4sub_init_parse((const uint8_t *)im, in_n, &init);
                            snprintf(init_url, sizeof init_url, "%s", abs);
                            fprintf(stderr, "webvtt_init kind=%d timescale=%u\n", init.kind, init.timescale);
                            free(im);
                        }
                    }
                }
                continue;
            }
            if (!*line || *line == '#') continue;
            total++;
            /* запись — сегменты около позиции; эфир — последние */
            int want = endlist ? t0 + dur > pos - 10 && t0 < pos + 60 : 1;
            if (want) {
                if (!endlist && ns == 64) { memmove(seg, seg + 1, sizeof seg[0] * 63); ns--; }
                if (ns < 64) { http_resolve(j->url, line, seg[ns].url, sizeof seg[ns].url); seg[ns].t = t0; seg[ns].d = dur; ns++; }
            }
            t0 += dur;
            dur = 0;
        }
        free(pl);
        int from = endlist ? 0 : (ns > 4 ? ns - 4 : 0), fetched = 0;
        for (int i = from; i < ns && !j->cancel && fetched < 6; i++) {
            if (sl_find(&seen, seg[i].url) >= 0) continue;
            size_t vn = 0;
            char *v = get(j, seg[i].url, &vn);
            if (!v) continue;
            webvtt_segment(v, vn, init_url[0] ? &init : NULL, add_cue, j);
            free(v);
            sl_add(&seen, seg[i].url);
            if (seen.n > MAX_SEEN) sl_remove(&seen, 0);
            fetched++;
        }
        (void)total;
        nap(j, endlist ? 1 : target / 2 > 1 ? target / 2 : 1);
    }
    sl_clear(&seen);
    pthread_mutex_lock(&W.mu);
    if (W.j == j) W.j = NULL;
    pthread_mutex_unlock(&W.mu);
    free(j);
    return NULL;
}

static void clear_cues(void) {
    for (int i = 0; i < W.n; i++) free(W.c[i].t);
    W.n = 0;
}

void webvtt_stop(void) {
    pthread_mutex_lock(&W.mu);
    if (W.j) { W.j->cancel = 1; W.j = NULL; }   /* поток сам выйдет и освободит задачу */
    clear_cues();
    pthread_mutex_unlock(&W.mu);
}

void webvtt_start(const char *url, const char *headers) {
    webvtt_stop();
    job *j = calloc(1, sizeof *j);
    if (!j) return;
    snprintf(j->url, sizeof j->url, "%s", url);
    snprintf(j->hdr, sizeof j->hdr, "%s", headers ? headers : "");
    pthread_mutex_lock(&W.mu);
    W.j = j;
    j->gen = ++W.gen;
    pthread_mutex_unlock(&W.mu);
    pthread_t t;
    if (pthread_create(&t, NULL, worker, j)) {
        pthread_mutex_lock(&W.mu);
        W.j = NULL;
        pthread_mutex_unlock(&W.mu);
        free(j);
        return;
    }
    pthread_detach(t);
    fprintf(stderr, "webvtt_start\n");
}

int webvtt_running(void) { return W.j != NULL; }

void webvtt_clock(int64_t frame_pts, double position) {
    pthread_mutex_lock(&W.mu);
    W.frame_pts = frame_pts;
    W.pos = position;
    pthread_mutex_unlock(&W.mu);
}

size_t webvtt_text_at(int64_t pts, char *out, size_t cap) {
    size_t o = 0;
    if (cap) out[0] = 0;
    pthread_mutex_lock(&W.mu);
    for (int i = 0; i < W.n; i++) {
        cue *c = &W.c[i];
        if (pts < c->s || pts >= c->e) continue;
        size_t l = strlen(c->t);
        if (o + l + 2 >= cap) break;
        if (o) out[o++] = '\n';
        memcpy(out + o, c->t, l);
        o += l;
        out[o] = 0;
    }
    pthread_mutex_unlock(&W.mu);
    return o;
}

int webvtt_cues(void) { return W.n; }
