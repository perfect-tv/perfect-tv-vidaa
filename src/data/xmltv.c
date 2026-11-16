#define _GNU_SOURCE
#include "xmltv.h"
#include "core/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { ENC_UTF8, ENC_1251, ENC_KOI8, ENC_LATIN1 };

typedef struct { char *title, *sub, *desc, *cat, *icon; int64_t start; int used; } pend;

struct xmltv {
    xmltv_cb cb;
    char *buf; size_t len, cap;
    long resume;                 /* начатый элемент: искать конец с этого места */
    int enc, programmes_started, channels_dirty;
    long channels, programmes, skipped;
    /* канал предыдущей передачи (сырые байты) */
    char last[256]; int last_len, last_slot;
    pend *pending; int npending;  /* передачи без stop по слотам */
    sbuf conv, t1, t2, t3, t4;
};

xmltv *xmltv_new(const xmltv_cb *cb) {
    xmltv *x = calloc(1, sizeof *x);
    if (!x) return NULL;
    x->cb = *cb;
    x->last_len = -1;
    x->resume = -1;
    return x;
}

static int is_space(int c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }
static int name_end(int c) { return c == '>' || c == '/' || is_space(c); }

static long find(const char *b, long from, long to, const char *p) {
    size_t n = strlen(p);
    if (to - from < (long)n) return -1;
    const char *r = memmem(b + from, (size_t)(to - from), p, n);
    return r ? (long)(r - b) : -1;
}

/* ---------- кодировки ---------- */
static const unsigned short CP1251[128] = {
    0x402,0x403,0x201a,0x453,0x201e,0x2026,0x2020,0x2021,0x20ac,0x2030,0x409,0x2039,0x40a,0x40c,0x40b,0x40f,
    0x452,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,0x3f,0x2122,0x459,0x203a,0x45a,0x45c,0x45b,0x45f,
    0xa0,0x40e,0x45e,0x408,0xa4,0x490,0xa6,0xa7,0x401,0xa9,0x404,0xab,0xac,0xad,0xae,0x407,
    0xb0,0xb1,0x406,0x456,0x491,0xb5,0xb6,0xb7,0x451,0x2116,0x454,0xbb,0x458,0x405,0x455,0x457};
static const unsigned short KOI8[64] = {   /* 0xC0..0xFF */
    0x44e,0x430,0x431,0x446,0x434,0x435,0x444,0x433,0x445,0x438,0x439,0x43a,0x43b,0x43c,0x43d,0x43e,
    0x43f,0x44f,0x440,0x441,0x442,0x443,0x436,0x432,0x44c,0x44b,0x437,0x448,0x44d,0x449,0x447,0x44a,
    0x42e,0x410,0x411,0x426,0x414,0x415,0x424,0x413,0x425,0x418,0x419,0x41a,0x41b,0x41c,0x41d,0x41e,
    0x41f,0x42f,0x420,0x421,0x422,0x423,0x416,0x412,0x42c,0x42b,0x417,0x428,0x42d,0x429,0x427,0x42a};

/* байты [from,to) в UTF-8: для UTF-8 — указатель на сам буфер */
static const char *decoded(xmltv *x, long from, long to, size_t *n) {
    if (x->enc == ENC_UTF8) { *n = (size_t)(to - from); return x->buf + from; }
    sb_clear(&x->conv);
    for (long i = from; i < to; i++) {
        unsigned char c = (unsigned char)x->buf[i];
        uint32_t cp = c;
        if (c >= 0x80) {
            if (x->enc == ENC_1251) cp = c >= 0xc0 ? 0x410u + (c - 0xc0u) : CP1251[c - 0x80];
            else if (x->enc == ENC_KOI8) cp = c >= 0xc0 ? KOI8[c - 0xc0] : c == 0xa3 ? 0x451 : c == 0xb3 ? 0x401 : 0x3f;
        }
        char t[4];
        int k = u8_put(t, cp);
        sb_addn(&x->conv, t, (size_t)k);
    }
    *n = x->conv.n;
    return x->conv.s ? x->conv.s : "";
}

/* ---------- текст ---------- */
static void put_cp(sbuf *o, uint32_t c) { char t[4]; sb_addn(o, t, (size_t)u8_put(t, c)); }

static void unescape_into(sbuf *o, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') { sb_addc(o, s[i]); continue; }
        size_t e = i + 1;
        while (e < n && e - i < 12 && s[e] != ';' && s[e] != '&') e++;
        if (e >= n || s[e] != ';') { sb_addc(o, '&'); continue; }
        const char *ent = s + i + 1;
        size_t el = e - i - 1;
        uint32_t cp = 0;
        if (el > 1 && ent[0] == '#') {
            char num[16];
            size_t k = el - 1 < sizeof num - 1 ? el - 1 : sizeof num - 1;
            memcpy(num, ent + 1, k);
            num[k] = 0;
            cp = (num[0] == 'x' || num[0] == 'X') ? (uint32_t)strtoul(num + 1, NULL, 16) : (uint32_t)strtoul(num, NULL, 10);
        } else if (el == 3 && !memcmp(ent, "amp", 3)) cp = '&';
        else if (el == 2 && !memcmp(ent, "lt", 2)) cp = '<';
        else if (el == 2 && !memcmp(ent, "gt", 2)) cp = '>';
        else if (el == 4 && !memcmp(ent, "quot", 4)) cp = '"';
        else if (el == 4 && !memcmp(ent, "apos", 4)) cp = '\'';
        else if (el == 4 && !memcmp(ent, "nbsp", 4)) cp = 0xa0;
        if (!cp || cp > 0x10ffff) { sb_addn(o, s + i, e - i + 1); i = e; continue; }
        put_cp(o, cp);
        i = e;
    }
}

/* CDATA как есть, вложенные теги убраны, сущности раскрыты */
static void text_into(sbuf *o, const char *s, size_t n) {
    sb_clear(o);
    size_t i = 0;
    while (i < n) {
        const char *c = n - i >= 9 ? memmem(s + i, n - i, "<![CDATA[", 9) : NULL;
        size_t part_end = c ? (size_t)(c - s) : n;
        /* часть без CDATA: убрать <...> */
        size_t a = i;
        while (a < part_end) {
            const char *lt = memchr(s + a, '<', part_end - a);
            size_t stop = lt ? (size_t)(lt - s) : part_end;
            unescape_into(o, s + a, stop - a);
            if (!lt) break;
            const char *gt = memchr(s + stop, '>', part_end - stop);
            if (!gt) { a = part_end; break; }
            a = (size_t)(gt - s) + 1;
        }
        if (!c) break;
        size_t cs = (size_t)(c - s) + 9;
        const char *e = n - cs >= 3 ? memmem(s + cs, n - cs, "]]>", 3) : NULL;
        if (!e) { sb_addn(o, s + cs, n - cs); break; }
        sb_addn(o, s + cs, (size_t)(e - s) - cs);
        i = (size_t)(e - s) + 3;
    }
    if (!o->s) sb_add(o, "");
    /* обрезка пробелов */
    size_t b = 0, e = o->n;
    while (b < e && is_space((unsigned char)o->s[b])) b++;
    while (e > b && is_space((unsigned char)o->s[e - 1])) e--;
    if (b) memmove(o->s, o->s + b, e - b);
    o->n = e - b;
    o->s[o->n] = 0;
}

void xmltv_text(const char *s, size_t n, char *out, size_t cap) {
    sbuf o = {0};
    text_into(&o, s, n);
    u8_copy(out, cap, o.s);
    sb_free(&o);
}

/* значение атрибута name в открывающем теге [s, s+n); 0 — нет */
static int attr_range(const char *s, size_t n, const char *name, size_t *vs, size_t *vl) {
    size_t nl = strlen(name), i = 0;
    while (i < n) {
        while (i < n && is_space((unsigned char)s[i])) i++;
        size_t ns = i;
        while (i < n && s[i] != '=' && !is_space((unsigned char)s[i]) && s[i] != '>' && s[i] != '/') i++;
        size_t ne = i;
        while (i < n && is_space((unsigned char)s[i])) i++;
        if (i >= n || s[i] != '=') { if (i == ns) i++; continue; }   /* без значения: дальше со следующего имени */
        i++;
        while (i < n && is_space((unsigned char)s[i])) i++;
        if (i >= n) return 0;
        char q = s[i];
        if (q != '"' && q != '\'') { i++; continue; }
        size_t v = i + 1, e = v;
        while (e < n && s[e] != q) e++;
        if (ne - ns == nl && !memcmp(s + ns, name, nl)) { *vs = v; *vl = e - v; return 1; }
        i = e + 1;
    }
    return 0;
}
static void attr_into(sbuf *o, const char *tag, size_t n, const char *name) {
    sb_clear(o);
    size_t vs, vl;
    if (attr_range(tag, n, name, &vs, &vl)) unescape_into(o, tag + vs, vl);
    if (!o->s) sb_add(o, "");
}

/* ---------- время ---------- */
static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
int64_t xmltv_time(const char *s, size_t n) {
    size_t i = 0;
    while (i < n && s[i] == ' ') i++;
#define NUM(len, out) do { int v_ = 0; size_t k_; if (i + (len) > n) { out = -1; break; } for (k_ = 0; k_ < (size_t)(len); k_++) { int c_ = s[i + k_] - '0'; if (c_ < 0 || c_ > 9) break; v_ = v_ * 10 + c_; } if (k_ < (size_t)(len)) { out = -1; break; } i += (len); out = v_; } while (0)
    int y, mo, d, h, mi, se;
    NUM(4, y); NUM(2, mo); NUM(2, d);
    if (y < 0 || mo < 1 || d < 1) return 0;
    NUM(2, h); NUM(2, mi); NUM(2, se);
    if (h < 0) h = 0;
    if (mi < 0) mi = 0;
    if (se < 0) se = 0;
    while (i < n && s[i] == ' ') i++;
    int64_t off = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) {
        int sign = s[i] == '-' ? -1 : 1, oh, om;
        i++;
        NUM(2, oh);
        if (i < n && s[i] == ':') i++;
        NUM(2, om);
        if (oh >= 0) off = sign * (oh * 3600 + (om > 0 ? om : 0) * 60);
    }
#undef NUM
    return days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se - off;
}

/* ---------- выбор по языку ---------- */
typedef struct { sbuf *v; int rank; } pick;
static void offer(xmltv *x, pick *p, const char *lang, size_t ll, const char *text, size_t tn) {
    int r = x->cb.nlangs;
    for (int i = 0; i < x->cb.nlangs && ll; i++) if (strlen(x->cb.langs[i]) == ll && !strncasecmp(x->cb.langs[i], lang, ll)) { r = i; break; }
    if (r < p->rank) { p->rank = r; text_into(p->v, text, tn); }
}

/* Обход дочерних элементов: title, sub-title, desc, category, icon, display-name. */
typedef void (*child_fn)(xmltv *x, void *u, const char *name, size_t nl, const char *tag, size_t tl, const char *inner, size_t il);
static void children(xmltv *x, const char *s, size_t n, child_fn fn, void *u) {
    size_t i = 0;
    while (i < n) {
        const char *lt = memchr(s + i, '<', n - i);
        if (!lt) return;
        size_t a = (size_t)(lt - s) + 1;
        if (a >= n) return;
        if (s[a] == '/' || s[a] == '!' || s[a] == '?') {
            if (s[a] == '!' && n - a >= 8 && !memcmp(s + a, "![CDATA[", 8)) {
                const char *e = memmem(s + a, n - a, "]]>", 3);
                i = e ? (size_t)(e - s) + 3 : n;
                continue;
            }
            i = a;
            continue;
        }
        size_t ne = a;
        while (ne < n && !name_end((unsigned char)s[ne])) ne++;
        const char *gt = memchr(s + ne, '>', n - ne);
        if (!gt) return;
        size_t te = (size_t)(gt - s);
        size_t nl = ne - a;
        int self = te > 0 && s[te - 1] == '/';
        size_t inner_s = te + 1, inner_e = te + 1, next = te + 1;
        if (!self) {
            /* </name> */
            char close[40];
            if (nl + 3 < sizeof close) {
                close[0] = '<'; close[1] = '/';
                memcpy(close + 2, s + a, nl);
                close[nl + 2] = 0;
                const char *c = memmem(s + inner_s, n - inner_s, close, nl + 2);
                while (c && (size_t)(c - s) + nl + 2 < n && !name_end((unsigned char)c[nl + 2]))
                    c = memmem(c + 1, n - (size_t)(c + 1 - s), close, nl + 2);
                if (c) {
                    inner_e = (size_t)(c - s);
                    const char *g = memchr(c, '>', n - inner_e);
                    next = g ? (size_t)(g - s) + 1 : n;
                } else { inner_e = inner_s; next = te + 1; }
            }
        }
        fn(x, u, s + a, nl, s + ne, te - ne, s + inner_s, inner_e - inner_s);
        i = next;
    }
}

static int name_is(const char *s, size_t n, const char *w) { return strlen(w) == n && !memcmp(s, w, n); }

typedef struct { pick title, sub, desc, cat; sbuf *icon; int have_icon; } prog_ctx;
static void prog_child(xmltv *x, void *u, const char *nm, size_t nl, const char *tag, size_t tl, const char *in, size_t il) {
    prog_ctx *p = u;
    if (name_is(nm, nl, "icon")) {
        if (!p->have_icon) { attr_into(p->icon, tag, tl, "src"); p->have_icon = p->icon->n > 0; }
        return;
    }
    pick *k = name_is(nm, nl, "title") ? &p->title : name_is(nm, nl, "sub-title") ? &p->sub : name_is(nm, nl, "desc") ? &p->desc :
              name_is(nm, nl, "category") ? &p->cat : NULL;
    if (!k) return;
    size_t vs = 0, vl = 0;
    const char *lang = "";
    if (attr_range(tag, tl, "lang", &vs, &vl)) lang = tag + vs;
    offer(x, k, lang, vl, in, il);
}

#define MAX_NAMES 16
typedef struct { sbuf names[MAX_NAMES]; int n; sbuf icon; int have_icon; } chan_ctx;
static void chan_child(xmltv *x, void *u, const char *nm, size_t nl, const char *tag, size_t tl, const char *in, size_t il) {
    (void)x;
    chan_ctx *c = u;
    if (name_is(nm, nl, "display-name") && c->n < MAX_NAMES) {
        text_into(&c->names[c->n], in, il);
        if (!c->names[c->n].n) return;
        for (int i = 0; i < c->n; i++) if (!strcmp(c->names[i].s, c->names[c->n].s)) return;
        c->n++;
    } else if (name_is(nm, nl, "icon") && !c->have_icon) {
        attr_into(&c->icon, tag, tl, "src");
        c->have_icon = c->icon.n > 0;
    }
}

static void prolog(xmltv *x, const char *s, size_t n) {
    size_t vs, vl;
    if (!attr_range(s, n, "encoding", &vs, &vl)) return;
    char e[32];
    size_t k = vl < sizeof e - 1 ? vl : sizeof e - 1;
    memcpy(e, s + vs, k);
    e[k] = 0;
    if (!strcasecmp(e, "windows-1251") || !strcasecmp(e, "cp1251")) x->enc = ENC_1251;
    else if (!strcasecmp(e, "koi8-r")) x->enc = ENC_KOI8;
    else if (!strcasecmp(e, "iso-8859-1") || !strcasecmp(e, "latin1")) x->enc = ENC_LATIN1;
    else x->enc = ENC_UTF8;
}

static void free_pend(pend *p) {
    free(p->title); free(p->sub); free(p->desc); free(p->cat); free(p->icon);
    memset(p, 0, sizeof *p);
}

static void emit(xmltv *x, int slot, int64_t start, int64_t stop, const char *t, const char *s, const char *d, const char *c, const char *i) {
    x->programmes++;
    x->cb.on_programme(x->cb.ctx, slot, start, stop, t, s, d, c, i);
}

static pend *pend_of(xmltv *x, int slot) {
    if (slot >= x->npending) {
        int n = slot + 64;
        pend *p = realloc(x->pending, (size_t)n * sizeof *p);
        if (!p) return NULL;
        memset(p + x->npending, 0, (size_t)(n - x->npending) * sizeof *p);
        x->pending = p;
        x->npending = n;
    }
    return &x->pending[slot];
}

static void programme(xmltv *x, int slot, const char *open, size_t ol, const char *inner, size_t il) {
    size_t vs, vl;
    int64_t start = attr_range(open, ol, "start", &vs, &vl) ? xmltv_time(open + vs, vl) : 0;
    if (start <= 0) { x->skipped++; return; }
    int64_t stop = attr_range(open, ol, "stop", &vs, &vl) ? xmltv_time(open + vs, vl) : 0;
    pend *pp = pend_of(x, slot);
    if (pp && pp->used) {   /* у предыдущей не было stop: до этой */
        int64_t pstop = start > pp->start + 60 ? start : pp->start + 60;
        emit(x, slot, pp->start, pstop, pp->title, pp->sub, pp->desc, pp->cat, pp->icon);
        free_pend(pp);
    }
    if (x->cb.accept && stop > start && !x->cb.accept(x->cb.ctx, slot, start, stop)) { x->skipped++; return; }
    prog_ctx p = {{&x->t1, 1 << 30}, {&x->t2, 1 << 30}, {&x->t3, 1 << 30}, {&x->t4, 1 << 30}, NULL, 0};
    sb_clear(&x->t1); sb_clear(&x->t2); sb_clear(&x->t3); sb_clear(&x->t4);
    sbuf icon = {0};
    p.icon = &icon;
    if (il) children(x, inner, il, prog_child, &p);
    const char *t = x->t1.s ? x->t1.s : "", *s = x->t2.s ? x->t2.s : "", *d = x->t3.s ? x->t3.s : "", *c = x->t4.s ? x->t4.s : "";
    const char *ic = icon.s ? icon.s : "";
    if (stop <= start) {
        if (pp) { pp->used = 1; pp->start = start; pp->title = strdup(t); pp->sub = strdup(s); pp->desc = strdup(d); pp->cat = strdup(c); pp->icon = strdup(ic); }
    } else emit(x, slot, start, stop, t, s, d, c, ic);
    sb_free(&icon);
}

static long tag_end(const char *b, long from, long n) {
    char q = 0;
    for (long i = from; i < n; i++) {
        char c = b[i];
        if (q) { if (c == q) q = 0; }
        else if (c == '"' || c == '\'') q = c;
        else if (c == '>') return i;
    }
    return -1;
}

/* разобрать, что возможно; вернуть, сколько байтов израсходовано */
static long run(xmltv *x, int final) {
    char *b = x->buf;
    long n = (long)x->len, pos = 0;
    while (pos < n) {
        const char *ltp = memchr(b + pos, '<', (size_t)(n - pos));
        if (!ltp) return n;
        long lt = (long)(ltp - b);
        pos = lt;
        if (lt + 1 >= n) return final ? n : lt;
        char c = b[lt + 1];
        if (c == '?') {
            long e = find(b, lt + 2, n, "?>");
            if (e < 0) return final ? n : lt;
            prolog(x, b + lt, (size_t)(e - lt));
            pos = e + 2;
            continue;
        }
        if (c == '!') {
            if (lt + 4 > n) return final ? n : lt;
            if (!memcmp(b + lt, "<!--", 4)) {
                long e = find(b, lt + 4, n, "-->");
                if (e < 0) return final ? n : lt;
                pos = e + 3;
            } else {
                const char *g = memchr(b + lt + 2, '>', (size_t)(n - lt - 2));
                if (!g) return final ? n : lt;
                pos = (long)(g - b) + 1;
            }
            continue;
        }
        long te = tag_end(b, lt + 1, n);
        if (te < 0) return final ? n : lt;
        long ne = lt + 1;
        while (ne < te && !name_end((unsigned char)b[ne])) ne++;
        int self = b[te - 1] == '/';
        long nl = ne - lt - 1;
        if (nl == 7 && !memcmp(b + lt + 1, "channel", 7)) {
            long end = te + 1;
            if (!self) {
                long e = find(b, x->resume > te ? x->resume : te + 1, n, "</channel>");
                if (e < 0) { x->resume = MAX(te + 1, n - 10); return final ? n : lt; }
                end = e + 10;
            }
            x->resume = -1;
            size_t dl;
            const char *d = decoded(x, lt, end, &dl);
            /* копия: decoded() может вернуть x->conv, а text_into ниже тоже пишет в буферы x */
            char *el = xstrndup(d, dl);
            if (el) {
                /* открывающий тег — до первого '>' вне кавычек */
                long ote = tag_end(el, 1, (long)dl);
                sbuf id = {0};
                attr_into(&id, el, ote > 0 ? (size_t)ote : dl, "id");
                if (id.n) {
                    chan_ctx cc;
                    memset(&cc, 0, sizeof cc);
                    if (ote > 0 && !self) children(x, el + ote + 1, dl - (size_t)ote - 1, chan_child, &cc);
                    const char *names[MAX_NAMES];
                    for (int i = 0; i < cc.n; i++) names[i] = cc.names[i].s;
                    x->channels++;
                    if (x->cb.on_channel) x->cb.on_channel(x->cb.ctx, id.s, names, cc.n, cc.icon.s ? cc.icon.s : "");
                    if (x->programmes_started) x->channels_dirty = 1;
                    for (int i = 0; i < MAX_NAMES; i++) sb_free(&cc.names[i]);
                    sb_free(&cc.icon);
                }
                sb_free(&id);
                free(el);
            }
            pos = end;
            continue;
        }
        if (nl == 9 && !memcmp(b + lt + 1, "programme", 9)) {
            long end = te + 1;
            if (!self) {
                long e = find(b, x->resume > te ? x->resume : te + 1, n, "</programme>");
                if (e < 0) { x->resume = MAX(te + 1, n - 12); return final ? n : lt; }
                end = e + 12;
            }
            x->resume = -1;
            if (!x->programmes_started || x->channels_dirty) {
                x->programmes_started = 1;
                x->channels_dirty = 0;
                x->last_len = -1;
                if (x->cb.before_programmes) x->cb.before_programmes(x->cb.ctx);
            }
            size_t vs, vl;
            if (!attr_range(b + ne, (size_t)(te - ne), "channel", &vs, &vl)) { x->skipped++; pos = end; continue; }
            const char *v = b + ne + vs;
            if ((int)vl != x->last_len || memcmp(v, x->last, vl)) {
                if (vl < sizeof x->last) { memcpy(x->last, v, vl); x->last_len = (int)vl; }
                else x->last_len = -1;
                size_t dl;
                const char *d = decoded(x, (long)(v - b), (long)(v - b + (long)vl), &dl);
                sbuf id = {0};
                unescape_into(&id, d, dl);
                x->last_slot = id.n && x->cb.wanted ? x->cb.wanted(x->cb.ctx, id.s) : -1;
                sb_free(&id);
            }
            if (x->last_slot < 0) { x->skipped++; pos = end; continue; }
            size_t dl;
            const char *d = decoded(x, lt, end, &dl);
            char *el = x->enc == ENC_UTF8 ? NULL : xstrndup(d, dl);
            const char *e = el ? el : d;
            long ote = tag_end(e, 1, (long)dl);
            if (ote > 0) {
                size_t ilen = self ? 0 : dl - (size_t)ote - 1;
                programme(x, x->last_slot, e, (size_t)ote, e + ote + 1, ilen);
            }
            free(el);
            pos = end;
            continue;
        }
        pos = te + 1;   /* <tv>, </tv> и прочее */
    }
    return n;
}

void xmltv_push(xmltv *x, const char *data, size_t n) {
    if (x->len + n + 1 > x->cap) {
        size_t cap = x->cap ? x->cap : 1 << 16;
        while (cap < x->len + n + 1) cap *= 2;
        char *nb = realloc(x->buf, cap);
        if (!nb) return;
        x->buf = nb;
        x->cap = cap;
    }
    memcpy(x->buf + x->len, data, n);
    x->len += n;
    long used = run(x, 0);
    if (used >= (long)x->len) x->len = 0;
    else if (used > 0) {
        memmove(x->buf, x->buf + used, x->len - (size_t)used);
        x->len -= (size_t)used;
        if (x->resume >= 0) x->resume -= used;
    }
    /* один элемент длиннее 64 МБ — испорченный файл: не копить бесконечно */
    if (x->len > (64u << 20)) { x->len = 0; x->resume = -1; }
}

void xmltv_end(xmltv *x) {
    if (x->len) run(x, 1);
    x->len = 0;
    for (int i = 0; i < x->npending; i++) {   /* хвосты без stop: до следующей не дошли — час */
        pend *p = &x->pending[i];
        if (p->used) emit(x, i, p->start, p->start + 3600, p->title, p->sub, p->desc, p->cat, p->icon);
        free_pend(p);
    }
}

long xmltv_channels(const xmltv *x) { return x->channels; }
long xmltv_programmes(const xmltv *x) { return x->programmes; }
long xmltv_skipped(const xmltv *x) { return x->skipped; }
void xmltv_free(xmltv *x) {
    if (!x) return;
    for (int i = 0; i < x->npending; i++) free_pend(&x->pending[i]);
    free(x->pending);
    free(x->buf);
    sb_free(&x->conv); sb_free(&x->t1); sb_free(&x->t2); sb_free(&x->t3); sb_free(&x->t4);
    free(x);
}
