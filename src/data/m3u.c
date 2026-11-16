#include "m3u.h"
#include "names.h"
#include "core/util.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_ATTRS 48
typedef struct { char *k, *v; } kv;
typedef struct { kv a[MAX_ATTRS]; int n; } attrs;

struct m3u {
    m3u_cb cb; void *ctx;
    sbuf rest;
    int first_line, rejected, is_m3u, count;
    attrs header;
    char *group;
    /* ожидающий #EXTINF */
    int pending;
    char *p_name;
    attrs p_attrs, p_opts;
    char *p_headers;
    sbuf epg;
};

static void attrs_clear(attrs *a) {
    for (int i = 0; i < a->n; i++) { free(a->a[i].k); free(a->a[i].v); }
    a->n = 0;
}
static const char *attrs_get(const attrs *a, const char *k) {
    for (int i = 0; i < a->n; i++) if (!strcmp(a->a[i].k, k)) return a->a[i].v;
    return NULL;
}
static void attrs_set(attrs *a, const char *k, size_t kn, const char *v, size_t vn) {
    char key[64];
    if (kn >= sizeof key) kn = sizeof key - 1;
    for (size_t i = 0; i < kn; i++) key[i] = (char)tolower((unsigned char)k[i]);
    key[kn] = 0;
    for (int i = 0; i < a->n; i++)
        if (!strcmp(a->a[i].k, key)) { free(a->a[i].v); a->a[i].v = xstrndup(v, vn); return; }
    if (a->n >= MAX_ATTRS) return;
    a->a[a->n].k = strdup(key);
    a->a[a->n].v = xstrndup(v, vn);
    a->n++;
}

/* ([^\s=]+)=(("|')(.*?)\3|\S+) по всей строке */
static void parse_attrs(const char *s, size_t len, attrs *out) {
    const char *e = s + len;
    while (s < e) {
        while (s < e && isspace((unsigned char)*s)) s++;
        const char *ks = s;
        while (s < e && !isspace((unsigned char)*s) && *s != '=') s++;
        if (s >= e || *s != '=' || s == ks) { while (s < e && !isspace((unsigned char)*s)) s++; continue; }
        const char *ke = s++;
        if (s < e && (*s == '"' || *s == '\'')) {
            char q = *s++;
            const char *vs = s;
            while (s < e && *s != q) s++;
            if (s < e) { attrs_set(out, ks, (size_t)(ke - ks), vs, (size_t)(s - vs)); s++; continue; }
            s = vs - 1;   /* нет закрывающей кавычки: \S+ */
        }
        const char *vs = s;
        while (s < e && !isspace((unsigned char)*s)) s++;
        if (s > vs) attrs_set(out, ks, (size_t)(ke - ks), vs, (size_t)(s - vs));
    }
}

const char *m3u_attr(const char *s, const char *key, char *out, size_t cap) {
    attrs a = {0};
    parse_attrs(s, strlen(s), &a);
    const char *v = attrs_get(&a, key);
    if (v) snprintf(out, cap, "%s", v);
    attrs_clear(&a);
    return v ? out : NULL;
}

m3u *m3u_new(m3u_cb cb, void *ctx) {
    m3u *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->cb = cb;
    p->ctx = ctx;
    p->first_line = 1;
    p->group = strdup("");
    return p;
}

static void pending_clear(m3u *p) {
    p->pending = 0;
    free(p->p_name); p->p_name = NULL;
    free(p->p_headers); p->p_headers = NULL;
    attrs_clear(&p->p_attrs);
    attrs_clear(&p->p_opts);
}

/* «-1 attr="..." ...,Название»: название — после первой запятой вне кавычек значений */
static void parse_extinf(m3u *p, const char *s) {
    pending_clear(p);
    while (*s == ' ') s++;
    const char *d = s;
    if (*d == '-') d++;
    if (!isdigit((unsigned char)*d)) return;
    while (isdigit((unsigned char)*d) || *d == '.') d++;
    char quote = 0;
    for (const char *c = d; *c; c++) {
        if (quote) { if (*c == quote) quote = 0; }
        else if ((*c == '"' || *c == '\'') && c > d && c[-1] == '=') quote = *c;
        else if (*c == ',') {
            p->pending = 1;
            p->p_name = strdup(c + 1);
            trim(p->p_name);
            parse_attrs(d, (size_t)(c - d), &p->p_attrs);
            return;
        }
    }
}

static int is_url(const char *s) {
    static const char *sch[] = {"http://", "https://", "rtmp://", "rtmpe://", "rtsp://", "udp://", "rtp://", "mms://"};
    for (int i = 0; i < 8; i++) if (starts_with_ci(s, sch[i]) && s[strlen(sch[i])]) return 1;
    return 0;
}
static int is_youtube(const char *s) { return starts_with_ci(s, "http://youtube.com") || starts_with_ci(s, "https://youtube.com") || starts_with_ci(s, "http://www.youtube.com") || starts_with_ci(s, "https://www.youtube.com"); }

static const char *first2(const char *a, const char *b) { return a ? a : b; }

static void add_epg(m3u *p, const char *v) {
    if (!v) return;
    const char *s = v;
    while (*s) {
        while (*s == ',' || *s == ' ') s++;
        const char *e = s;
        while (*e && *e != ',' && *e != ' ') e++;
        if (e > s) {
            char u[2048];
            size_t n = (size_t)(e - s) < sizeof u - 1 ? (size_t)(e - s) : sizeof u - 1;
            memcpy(u, s, n);
            u[n] = 0;
            if (starts_with_ci(u, "http://") || starts_with_ci(u, "https://") || starts_with_ci(u, "file://")) {
                /* без повторов */
                int dup = 0;
                for (const char *q = p->epg.s; q && *q;) {
                    const char *nl = strchr(q, '\n');
                    size_t l = nl ? (size_t)(nl - q) : strlen(q);
                    if (l == n && !memcmp(q, u, n)) { dup = 1; break; }
                    q = nl ? nl + 1 : q + l;
                }
                if (!dup) { if (p->epg.n) sb_addc(&p->epg, '\n'); sb_add(&p->epg, u); }
            }
        }
        s = e;
    }
}

static int is_int(const char *s) {
    if (*s == '-') s++;
    if (!*s) return 0;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

static void emit(m3u *p, const char *url) {
    const attrs *a = &p->p_attrs, *h = &p->header, *o = &p->p_opts;
    m3u_channel c;
    memset(&c, 0, sizeof c);
    c.num = ++p->count;
    char name[512], defname[32];
    snprintf(name, sizeof name, "%s", p->p_name ? p->p_name : "");
    names_clean(name);
    if (!name[0]) { snprintf(defname, sizeof defname, "Ch %d", c.num); c.name = defname; }
    else c.name = name;
    c.url = url;
    c.group = attrs_get(a, "group-title") && *attrs_get(a, "group-title") ? attrs_get(a, "group-title") : p->group;
    c.logo = first2(attrs_get(a, "tvg-logo"), attrs_get(a, "logo"));
    if (!c.logo) c.logo = "";
    c.tvg_id = attrs_get(a, "tvg-id") ? attrs_get(a, "tvg-id") : "";
    c.tvg_name = attrs_get(a, "tvg-name") ? attrs_get(a, "tvg-name") : "";
    const char *sh = attrs_get(a, "tvg-shift");
    if (!sh) sh = attrs_get(h, "tvg-shift");
    c.tvg_shift = sh ? atof(sh) : 0;
    /* архив */
    const char *d = first2(first2(attrs_get(a, "catchup-days"), attrs_get(a, "tvg-rec")), first2(attrs_get(a, "timeshift"),
                     first2(attrs_get(h, "catchup-days"), first2(attrs_get(h, "tvg-rec"), attrs_get(h, "timeshift")))));
    char dd[32];
    snprintf(dd, sizeof dd, "%s", d ? d : "0");
    trim(dd);
    int days = is_int(dd) ? atoi(dd) : 0;
    int enabled;
    const char *ce = attrs_get(a, "catchup-enable");
    if (ce) enabled = atoi(ce) > 0;
    else enabled = days > 0 || attrs_get(a, "catchup") || attrs_get(a, "catchup-type") || attrs_get(a, "catchup-source");
    const char *type = first2(first2(attrs_get(a, "catchup"), attrs_get(a, "catchup-type")), first2(attrs_get(h, "catchup"), attrs_get(h, "catchup-type")));
    if (!type) type = "";
    if (enabled && strcasecmp(type, "disabled")) {
        c.has_catchup = 1;
        c.cu_type = type;
        c.cu_source = first2(attrs_get(a, "catchup-source"), attrs_get(h, "catchup-source"));
        if (!c.cu_source) c.cu_source = "";
        c.cu_days = days > 0 ? days : 0;
    }
    c.user_agent = first2(attrs_get(o, "http-user-agent"), first2(attrs_get(a, "http-user-agent"), attrs_get(a, "user-agent")));
    if (!c.user_agent) c.user_agent = "";
    c.referrer = first2(first2(attrs_get(o, "http-referrer"), attrs_get(o, "http-referer")),
                        first2(first2(attrs_get(a, "http-referrer"), attrs_get(a, "http-referer")), attrs_get(a, "referer")));
    if (!c.referrer) c.referrer = "";
    c.headers_json = p->p_headers ? p->p_headers : "";
    p->cb(&c, p->ctx);
}

static void line(m3u *p, char *s) {
    size_t n = strlen(s);
    if (n && s[n - 1] == '\r') s[--n] = 0;
    if (p->first_line) {
        if ((unsigned char)s[0] == 0xef && (unsigned char)s[1] == 0xbb && (unsigned char)s[2] == 0xbf) s += 3;
        trim(s);
        if (!*s) return;
        p->first_line = 0;
        if (strncasecmp(s, "#EXTM3U", 7)) { p->rejected = 1; return; }
        p->is_m3u = 1;
        parse_attrs(s + 7, strlen(s + 7), &p->header);
        add_epg(p, attrs_get(&p->header, "url-tvg"));
        add_epg(p, attrs_get(&p->header, "x-tvg-url"));
        add_epg(p, attrs_get(&p->header, "tvg-url"));
        return;
    }
    trim(s);
    if (!*s) return;
    if (*s == '#') {
        if (!strncasecmp(s, "#EXTINF:", 8)) parse_extinf(p, s + 8);
        else if (!strncasecmp(s, "#EXTGRP:", 8)) { char *g = s + 8; trim(g); if (*g) { free(p->group); p->group = strdup(g); } }
        else if (!strncasecmp(s, "#EXTVLCOPT:", 11)) {
            char *opt = s + 11, *eq = strchr(opt, '=');
            if (eq && eq > opt && p->pending) {
                char *k = opt, *v = eq + 1;
                *eq = 0;
                trim(k);
                trim(v);
                attrs_set(&p->p_opts, k, strlen(k), v, strlen(v));
            }
        } else if (!strncasecmp(s, "#EXTHTTP:", 9)) {
            if (p->pending) { free(p->p_headers); p->p_headers = strdup(s + 9); trim(p->p_headers); }
        } else if (!strncasecmp(s, "#EXTM3U", 7) && p->is_m3u) {
            /* второй заголовок (склеенные плейлисты): дополнить адреса программы */
            attrs h2 = {0};
            parse_attrs(s + 7, strlen(s + 7), &h2);
            add_epg(p, attrs_get(&h2, "url-tvg"));
            add_epg(p, attrs_get(&h2, "x-tvg-url"));
            attrs_clear(&h2);
        }
        return;
    }
    if (!is_url(s)) return;
    if (is_youtube(s)) { pending_clear(p); return; }
    emit(p, s);
    pending_clear(p);
}

void m3u_push(m3u *p, const char *d, size_t n) {
    if (p->rejected) return;
    size_t from = 0;
    for (size_t i = 0; i < n; i++) {
        if (d[i] != '\n') continue;
        if (p->rest.n) {
            sb_addn(&p->rest, d + from, i - from);
            line(p, p->rest.s);
            sb_clear(&p->rest);
        } else {
            char stack[2048];
            size_t len = i - from;
            if (len < sizeof stack) { memcpy(stack, d + from, len); stack[len] = 0; line(p, stack); }
            else { char *h = xstrndup(d + from, len); if (h) { line(p, h); free(h); } }
        }
        from = i + 1;
        if (p->rejected) return;
    }
    if (from < n) sb_addn(&p->rest, d + from, n - from);
    if (p->rest.n > (8u << 20)) { p->rejected = 1; }   /* строка длиннее 8 МБ — не плейлист */
}

void m3u_end(m3u *p) {
    if (p->rest.n && !p->rejected) { line(p, p->rest.s); sb_clear(&p->rest); }
}
int m3u_count(const m3u *p) { return p->count; }
int m3u_rejected(const m3u *p) { return p->rejected; }
int m3u_is_m3u(const m3u *p) { return p->is_m3u; }
const char *m3u_epg_urls(const m3u *p) { return p->epg.s ? p->epg.s : ""; }
void m3u_free(m3u *p) {
    if (!p) return;
    pending_clear(p);
    attrs_clear(&p->header);
    sb_free(&p->rest);
    sb_free(&p->epg);
    free(p->group);
    free(p);
}
