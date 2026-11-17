#define _GNU_SOURCE
#include "repo.h"
#include "app.h"
#include "json.h"
#include "names.h"
#include "catchup.h"
#include "core/clock.h"
#include "core/lang.h"
#include "core/prefs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

repo R;

/* ---------- плейлисты ---------- */
static char *jstr(const json *o, const char *k) { return strdup(json_str(json_get(o, k), "")); }

void repo_channels_path(int id, char *out, size_t cap) {
    char n[40];
    snprintf(n, sizeof n, "channels_%d.tsv", id);
    snprintf(out, cap, "%s", data_path(n));
}

static void load_playlists(void) {
    size_t n;
    char *t = file_read(data_path("playlists.json"), &n);
    if (!t) return;
    json_doc *d = json_parse(t, n);
    free(t);
    if (!d) return;
    json *a = json_root(d);
    int len = json_len(a);
    R.pl = calloc((size_t)(len > 0 ? len : 1), sizeof *R.pl);
    for (int i = 0; i < len && R.pl; i++) {
        json *o = json_at(a, i);
        playlist *p = &R.pl[R.npl++];
        p->id = (int)json_int(json_get(o, "id"), i + 1);
        snprintf(p->name, sizeof p->name, "%s", json_str(json_get(o, "name"), ""));
        snprintf(p->url, sizeof p->url, "%s", json_str(json_get(o, "url"), ""));
        p->epg_mode = (int)json_int(json_get(o, "epg_mode"), 0);
        p->user_epg = jstr(o, "user_epg");
        p->header_epg = jstr(o, "header_epg");
        p->loaded_at = json_int(json_get(o, "loaded_at"), 0);
        p->count = (int)json_int(json_get(o, "count"), 0);
        snprintf(p->error, sizeof p->error, "%s", json_str(json_get(o, "error"), ""));
        snprintf(p->etag, sizeof p->etag, "%s", json_str(json_get(o, "etag"), ""));
        snprintf(p->last_modified, sizeof p->last_modified, "%s", json_str(json_get(o, "last_modified"), ""));
    }
    json_free(d);
}

static void jput(sbuf *b, const char *k, const char *v, int comma) {
    size_t cap = strlen(v) * 6 + 8;
    char *e = malloc(cap);
    if (!e) return;
    json_escape(e, cap, v);
    sb_printf(b, "%s\"%s\":\"%s\"", comma ? "," : "", k, e);
    free(e);
}

void repo_save_playlists(void) {
    sbuf b = {0};
    sb_add(&b, "[");
    for (int i = 0; i < R.npl; i++) {
        playlist *p = &R.pl[i];
        sb_printf(&b, "%s\n{\"id\":%d,\"epg_mode\":%d,\"loaded_at\":%lld,\"count\":%d", i ? "," : "", p->id, p->epg_mode, (long long)p->loaded_at, p->count);
        jput(&b, "name", p->name, 1);
        jput(&b, "url", p->url, 1);
        jput(&b, "user_epg", p->user_epg ? p->user_epg : "", 1);
        jput(&b, "header_epg", p->header_epg ? p->header_epg : "", 1);
        jput(&b, "error", p->error, 1);
        jput(&b, "etag", p->etag, 1);
        jput(&b, "last_modified", p->last_modified, 1);
        sb_add(&b, "}");
    }
    sb_add(&b, "\n]\n");
    if (file_write_atomic(data_path("playlists.json"), b.s, b.n)) fprintf(stderr, "playlists_save=failed\n");
    sb_free(&b);
}

playlist *repo_cur(void) { return R.cur >= 0 && R.cur < R.npl ? &R.pl[R.cur] : NULL; }

playlist *repo_add_playlist(const char *name, const char *url) {
    playlist *n = realloc(R.pl, (size_t)(R.npl + 1) * sizeof *n);
    if (!n) return NULL;
    R.pl = n;
    int id = 1;
    for (int i = 0; i < R.npl; i++) if (R.pl[i].id >= id) id = R.pl[i].id + 1;
    playlist *p = &R.pl[R.npl++];
    memset(p, 0, sizeof *p);
    p->id = id;
    snprintf(p->name, sizeof p->name, "%s", name && *name ? name : trf(S_PLAYLIST_DEFAULT_NAME, id));
    snprintf(p->url, sizeof p->url, "%s", url);
    p->user_epg = strdup("");
    p->header_epg = strdup("");
    repo_save_playlists();
    return p;
}

void repo_delete_playlist(int i) {
    if (i < 0 || i >= R.npl) return;
    char path[700];
    repo_channels_path(R.pl[i].id, path, sizeof path);
    unlink(path);
    free(R.pl[i].user_epg);
    free(R.pl[i].header_epg);
    memmove(R.pl + i, R.pl + i + 1, (size_t)(R.npl - i - 1) * sizeof *R.pl);
    R.npl--;
    if (R.cur == i) R.cur = -1;
    else if (R.cur > i) R.cur--;
    repo_save_playlists();
    repo_gc_epg();
}

void repo_move_playlist(int i, int dir) {
    int j = i + dir;
    if (i < 0 || j < 0 || i >= R.npl || j >= R.npl) return;
    playlist t = R.pl[i];
    R.pl[i] = R.pl[j];
    R.pl[j] = t;
    if (R.cur == i) R.cur = j;
    else if (R.cur == j) R.cur = i;
    repo_save_playlists();
}

void repo_epg_urls(const playlist *p, strlist *out) {
    strlist u = {0}, h = {0};
    sl_from_lines(&u, p->user_epg);
    sl_from_lines(&h, p->header_epg);
    sl_clear(out);
    if (p->epg_mode == EPG_MINE) for (int i = 0; i < u.n; i++) sl_add(out, u.v[i]);
    else if (p->epg_mode == EPG_BOTH) {
        for (int i = 0; i < u.n; i++) sl_add(out, u.v[i]);
        for (int i = 0; i < h.n; i++) if (sl_find(out, h.v[i]) < 0) sl_add(out, h.v[i]);
    } else {
        strlist *s = h.n ? &h : &u;
        for (int i = 0; i < s->n; i++) sl_add(out, s->v[i]);
    }
    while (out->n > MAX_EPG) sl_remove(out, out->n - 1);
    sl_clear(&u);
    sl_clear(&h);
}

/* ---------- пользовательские данные ---------- */
static const char *ukey(const char *kind) {
    static char k[48];
    playlist *p = repo_cur();
    snprintf(k, sizeof k, "%s.%d", kind, p ? p->id : 0);
    return k;
}
static void load_list(strlist *l, const char *kind) { sl_from_lines(l, store_get(ukey(kind))); }
static void save_list(const strlist *l, const char *kind) {
    char *s = sl_join(l, "\n");
    store_set(ukey(kind), s && *s ? s : NULL);
    free(s);
}
static void load_set(smap *m, const char *kind) {
    smap_free(m);
    smap_init(m, 16);
    strlist l = {0};
    load_list(&l, kind);
    for (int i = 0; i < l.n; i++) smap_set(m, l.v[i], 1);
    sl_clear(&l);
}
static void save_set(const smap *m, const char *kind) {
    strlist l = {0};
    for (int i = 0; i < m->cap; i++) if (m->keys[i] && m->vals[i]) sl_add(&l, m->keys[i]);
    save_list(&l, kind);
    sl_clear(&l);
}
static void load_scores(void) {
    smap_free(&R.scores);
    smap_init(&R.scores, 64);
    strlist l = {0};
    load_list(&l, "score");
    for (int i = 0; i < l.n; i++) {
        char *t = strrchr(l.v[i], '\t');
        if (!t) continue;
        *t = 0;
        smap_set(&R.scores, l.v[i], atoi(t + 1));
    }
    sl_clear(&l);
}
static void save_scores(void) {
    sbuf b = {0};
    for (int i = 0; i < R.scores.cap; i++) if (R.scores.keys[i] && R.scores.vals[i] > 0) sb_printf(&b, "%s\t%d\n", R.scores.keys[i], R.scores.vals[i]);
    store_set(ukey("score"), b.n ? b.s : NULL);
    sb_free(&b);
}

/* ---------- каналы ---------- */
static void free_groups(group *g, int n) {
    for (int i = 0; i < n; i++) free(g[i].ch);
    free(g);
}

static void free_channels(void) {
    free(R.ch);
    R.ch = NULL;
    R.nch = 0;
    sp_free(&R.pool);
    free_groups(R.groups, R.ngroups);
    R.groups = NULL;
    R.ngroups = 0;
    free_groups(R.raw, R.nraw);
    R.raw = NULL;
    R.nraw = 0;
}

/* поля TSV: \t \n \\ экранированы */
static char *unesc(char *s) {
    char *o = s, *r = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) { s++; *o++ = *s == 't' ? '\t' : *s == 'n' ? '\n' : *s; }
        else *o++ = *s;
    }
    *o = 0;
    return r;
}

static void group_add(group *g, channel *c) {
    if (g->n >= g->cap) {
        int cap = g->cap ? g->cap * 2 : 16;
        channel **v = realloc(g->ch, (size_t)cap * sizeof *v);
        if (!v) return;
        g->ch = v;
        g->cap = cap;
    }
    g->ch[g->n++] = c;
}

/* Строка TSV каналов -> поля (на месте); число полей */
static int split_tsv(char *line, char **fld, int max) {
    int nf = 0;
    char *p = line;
    fld[nf++] = p;
    for (; *p && nf < max; p++) {
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == '\t') { *p = 0; fld[nf++] = p + 1; }
    }
    for (int i = 0; i < nf; i++) unesc(fld[i]);
    return nf;
}

/* Каналы плейлиста pl_id из его файла — для фильтра сборки программы. Только файлы: можно в рабочем потоке. */
int repo_read_wants(int pl_id, int default_days, strpool *pool, epg_want **w, int *nw, int *cap) {
    char path[700];
    repo_channels_path(pl_id, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char *line = NULL;
    size_t lcap = 0;
    ssize_t len;
    char k[400];
    while ((len = getline(&line, &lcap, f)) > 0) {
        if (line[len - 1] == '\n') line[--len] = 0;
        char *fld[16];
        if (split_tsv(line, fld, 16) < 15) continue;
        if (*nw >= *cap) {
            int nc = *cap ? *cap * 2 : 1024;
            epg_want *n = realloc(*w, (size_t)nc * sizeof *n);
            if (!n) break;
            *w = n;
            *cap = nc;
        }
        epg_want *e = &(*w)[(*nw)++];
        e->tvg_id = sp_dup(pool, fld[5]);
        names_match_key(fld[6], k, sizeof k);
        e->tvg_name_key = sp_dup(pool, k);
        names_match_key(fld[1], k, sizeof k);
        e->name_key = sp_dup(pool, k);
        int d = atoi(fld[11]);
        e->archive_days = atoi(fld[8]) ? (d > 0 ? d : default_days) : 0;
    }
    free(line);
    fclose(f);
    return 0;
}

static int load_channels(int pl_id) {
    char path[700];
    repo_channels_path(pl_id, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int cap = 1024;
    R.ch = calloc((size_t)cap, sizeof *R.ch);
    char *line = NULL;
    size_t lcap = 0;
    ssize_t len;
    while (R.ch && (len = getline(&line, &lcap, f)) > 0) {
        if (line[len - 1] == '\n') line[--len] = 0;
        char *fld[16];
        int nf = split_tsv(line, fld, 16);
        if (nf < 15) continue;
        if (R.nch >= cap) {
            cap *= 2;
            channel *n = realloc(R.ch, (size_t)cap * sizeof *n);
            if (!n) break;
            R.ch = n;
        }
        channel *c = &R.ch[R.nch];
        memset(c, 0, sizeof *c);
        c->idx = R.nch++;
        c->num = atoi(fld[0]);
        c->name = sp_dup(&R.pool, fld[1]);
        c->url = sp_dup(&R.pool, fld[2]);
        c->group = sp_dup(&R.pool, fld[3]);
        c->logo = sp_dup(&R.pool, fld[4]);
        c->tvg_id = sp_dup(&R.pool, fld[5]);
        c->tvg_name = sp_dup(&R.pool, fld[6]);
        c->tvg_shift = atof(fld[7]);
        if (atoi(fld[8])) {
            c->cu_type = sp_dup(&R.pool, fld[9]);
            c->cu_source = sp_dup(&R.pool, fld[10]);
            int d = atoi(fld[11]);
            c->archive_days = d > 0 ? d : P.archive_days;
        }
        c->ua = sp_dup(&R.pool, fld[12]);
        c->ref = sp_dup(&R.pool, fld[13]);
        c->headers_json = sp_dup(&R.pool, fld[14]);
        char k[400];
        names_key(c->name, k, sizeof k);
        c->key = sp_dup(&R.pool, k);
        names_match_key(c->name, k, sizeof k);
        c->mkey = sp_dup(&R.pool, k);
        names_match_key(c->tvg_name, k, sizeof k);
        c->tkey = sp_dup(&R.pool, k);
        names_monogram(c->name, c->mono, sizeof c->mono);
        c->color = names_color(c->key);
        c->epg_src = c->epg_ch = -1;
    }
    free(line);
    fclose(f);
    /* категории в порядке появления */
    smap gi;
    smap_init(&gi, 64);
    int gcap = 0;
    for (int i = 0; i < R.nch; i++) {
        channel *c = &R.ch[i];
        int g = smap_get(&gi, c->group, -1);
        if (g < 0) {
            if (R.nraw >= gcap) {
                gcap = gcap ? gcap * 2 : 32;
                group *n = realloc(R.raw, (size_t)gcap * sizeof *n);
                if (!n) break;
                R.raw = n;
            }
            g = R.nraw++;
            memset(&R.raw[g], 0, sizeof R.raw[g]);
            R.raw[g].key = c->group;
            R.raw[g].title = c->group[0] ? c->group : NULL;
            smap_set(&gi, c->group, g);
        }
        group_add(&R.raw[g], c);
    }
    smap_free(&gi);
    return 0;
}

static int score_of(const channel *c) { return smap_get(&R.scores, c->key, 0); }
static int cmp_score(const void *a, const void *b) {
    const channel *x = *(channel *const *)a, *y = *(channel *const *)b;
    int d = score_of(y) - score_of(x);
    return d ? d : x->num - y->num;
}

void repo_apply_view(void) {
    free_groups(R.groups, R.ngroups);
    R.groups = calloc((size_t)R.nraw + 2, sizeof *R.groups);
    R.ngroups = 0;
    if (!R.groups) return;
    /* избранное */
    group fav = {.key = FAV_KEY, .kind = GK_FAV};
    for (int i = 0; i < R.fav.n; i++) {
        channel *c = repo_by_key(R.fav.v[i]);
        if (c && !smap_get(&R.hidden_ch, c->key, 0)) group_add(&fav, c);
    }
    if (fav.n) R.groups[R.ngroups++] = fav;
    else free(fav.ch);
    /* все каналы */
    group all = {.key = ALL_KEY, .kind = GK_ALL};
    int all_at = R.ngroups;
    R.ngroups++;
    for (int i = 0; i < R.nraw; i++) {
        group *g = &R.raw[i];
        if (smap_get(&R.hidden_gr, g->key, 0)) continue;
        group v = {.key = g->key, .title = g->title, .kind = GK_PLAIN};
        for (int k = 0; k < g->n; k++) if (!smap_get(&R.hidden_ch, g->ch[k]->key, 0)) group_add(&v, g->ch[k]);
        if (!v.n) { free(v.ch); continue; }
        if (P.adaptive) qsort(v.ch, (size_t)v.n, sizeof *v.ch, cmp_score);
        R.groups[R.ngroups++] = v;
    }
    for (int i = 0; i < R.nch; i++) {
        channel *c = &R.ch[i];
        if (!smap_get(&R.hidden_gr, c->group, 0) && !smap_get(&R.hidden_ch, c->key, 0)) group_add(&all, c);
    }
    if (P.adaptive && all.n) qsort(all.ch, (size_t)all.n, sizeof *all.ch, cmp_score);
    if (!all.n && R.ngroups == all_at + 1) {   /* всё скрыто */
        free(all.ch);
        R.ngroups--;
    } else R.groups[all_at] = all;
    /* категория без названия — «Без категории» показывается через title == NULL */
    R.rev++;
}

group *repo_group(const char *key) {
    if (!key) return NULL;
    for (int i = 0; i < R.ngroups; i++) if (!strcmp(R.groups[i].key, key)) return &R.groups[i];
    return NULL;
}
int repo_group_index(const group *g) { return g ? (int)(g - R.groups) : -1; }
int repo_index_in(const group *g, const channel *c) {
    if (!g || !c) return -1;
    for (int i = 0; i < g->n; i++) if (g->ch[i] == c) return i;
    return -1;
}
group *repo_group_of(const channel *c, const char *prefer) {
    group *p = repo_group(prefer);
    if (p && repo_index_in(p, c) >= 0) return p;
    for (int i = 0; i < R.ngroups; i++) if (R.groups[i].kind == GK_PLAIN && repo_index_in(&R.groups[i], c) >= 0) return &R.groups[i];
    for (int i = 0; i < R.ngroups; i++) if (repo_index_in(&R.groups[i], c) >= 0) return &R.groups[i];
    return R.ngroups ? &R.groups[0] : NULL;
}
channel *repo_by_key(const char *key) {
    for (int i = 0; i < R.nch; i++) if (!strcmp(R.ch[i].key, key)) return &R.ch[i];
    return NULL;
}
channel *repo_by_num(int num) {
    for (int i = 0; i < R.nch; i++) if (R.ch[i].num == num && !smap_get(&R.hidden_ch, R.ch[i].key, 0) && !smap_get(&R.hidden_gr, R.ch[i].group, 0)) return &R.ch[i];
    return NULL;
}

int repo_is_fav(const channel *c) { return c && sl_find(&R.fav, c->key) >= 0; }
void repo_toggle_fav(const channel *c) {
    int i = sl_find(&R.fav, c->key);
    if (i >= 0) sl_remove(&R.fav, i);
    else sl_add(&R.fav, c->key);
    save_list(&R.fav, "fav");
    repo_apply_view();
}
void repo_move_fav(const channel *c, int dir) {
    int i = sl_find(&R.fav, c->key), j = i + dir;
    if (i < 0 || j < 0 || j >= R.fav.n) return;
    char *t = R.fav.v[i];
    R.fav.v[i] = R.fav.v[j];
    R.fav.v[j] = t;
    save_list(&R.fav, "fav");
    repo_apply_view();
}
void repo_add_history(const channel *c) {
    int i = sl_find(&R.history, c->key);
    if (i >= 0) sl_remove(&R.history, i);
    sl_insert(&R.history, 0, c->key);
    while (R.history.n > HISTORY_SIZE) sl_remove(&R.history, R.history.n - 1);
    save_list(&R.history, "hist");
}
int repo_recent(const channel *cur, channel **out, int max) {
    int n = 0;
    for (int i = 0; i < R.history.n && n < max; i++) {
        channel *c = repo_by_key(R.history.v[i]);
        if (c && c != cur && !repo_is_hidden(c)) out[n++] = c;
    }
    return n;
}
int repo_is_hidden(const channel *c) { return smap_get(&R.hidden_ch, c->key, 0) || smap_get(&R.hidden_gr, c->group, 0); }
void repo_hide_channel(const channel *c) {
    smap_set(&R.hidden_ch, c->key, 1);
    save_set(&R.hidden_ch, "hidc");
    /* название скрытого — для списка «Скрытое» */
    char k[200];
    snprintf(k, sizeof k, "hname.%s", c->key);
    store_set(k, c->name);
    repo_apply_view();
}
void repo_hide_group(const char *key) {
    if (!strcmp(key, FAV_KEY) || !strcmp(key, ALL_KEY)) return;
    smap_set(&R.hidden_gr, key, 1);
    save_set(&R.hidden_gr, "hidg");
    repo_apply_view();
}
void repo_unhide(int is_group, const char *key) {
    smap *m = is_group ? &R.hidden_gr : &R.hidden_ch;
    smap_set(m, key, 0);
    save_set(m, is_group ? "hidg" : "hidc");
    repo_apply_view();
}
int repo_hidden_count(void) {
    int n = 0;
    for (int i = 0; i < R.hidden_ch.cap; i++) if (R.hidden_ch.keys[i] && R.hidden_ch.vals[i]) n++;
    for (int i = 0; i < R.hidden_gr.cap; i++) if (R.hidden_gr.keys[i] && R.hidden_gr.vals[i]) n++;
    return n;
}
int repo_channel_locked(const channel *c) { return sl_find(&R.locked_ch, c->key) >= 0; }
int repo_group_locked(const char *key) { return sl_find(&R.locked_gr, key) >= 0; }
int repo_is_locked(const channel *c) { return c && P.pin[0] && (repo_channel_locked(c) || repo_group_locked(c->group)); }
void repo_set_locked(int is_group, const char *key, int on) {
    strlist *l = is_group ? &R.locked_gr : &R.locked_ch;
    int i = sl_find(l, key);
    if (on && i < 0) sl_add(l, key);
    if (!on && i >= 0) sl_remove(l, i);
    save_list(l, is_group ? "lockg" : "lockc");
}
void repo_add_score(const channel *c) {
    smap_set(&R.scores, c->key, smap_get(&R.scores, c->key, 0) + 1);
    save_scores();
}
void repo_reset_scores(void) {
    smap_free(&R.scores);
    smap_init(&R.scores, 16);
    save_scores();
    repo_apply_view();
}

/* ---------- программа ---------- */
/* Сопоставление каналов текущего плейлиста с каналами XMLTV источников (по порядку источников: первый, где канал
 * нашёлся, — его). В источнике: tvg-id, затем tvg-name, затем имя; при равенстве — первый канал XMLTV. Каналы XMLTV
 * читаются построчно из c<gen>.tsv, в памяти — только сопоставленные (слоты источника). */
typedef struct { int ch, rank, next; } mref;
typedef struct {
    int src;
    smap ids, keys;         /* tvg-id / ключ имени -> первая ссылка */
    mref *ref; int nref;
    int *best_src, *best_rank, *best_slot;
} mctx;

static void mref_add(mctx *m, smap *map, const char *key, int ch, int rank) {
    if (!key || !*key) return;
    mref *r = &m->ref[m->nref];
    r->ch = ch;
    r->rank = rank;
    r->next = smap_get(map, key, -1);
    smap_set(map, key, m->nref++);
}

static void m_assign(mctx *m, int ch, int rank, int *slot, const char *id, const char *icon) {
    if (m->best_src[ch] >= 0 && m->best_src[ch] < m->src) return;   /* нашёлся в источнике раньше */
    if (m->best_src[ch] == m->src && m->best_rank[ch] <= rank) return;
    if (*slot < 0) *slot = epg_slot_add(&R.epg[m->src], id, icon);
    if (*slot < 0) return;
    m->best_src[ch] = m->src;
    m->best_rank[ch] = rank;
    m->best_slot[ch] = *slot;
}

static void m_chan(void *ctx, const char *id, const char *icon, const char *const *names, int n) {
    mctx *m = ctx;
    int slot = -1;
    for (int r = smap_get(&m->ids, id, -1); r >= 0; r = m->ref[r].next) m_assign(m, m->ref[r].ch, 0, &slot, id, icon);
    char key[300];
    for (int k = 0; k < n; k++) {
        names_match_key(names[k], key, sizeof key);
        if (!key[0]) continue;
        for (int r = smap_get(&m->keys, key, -1); r >= 0; r = m->ref[r].next) m_assign(m, m->ref[r].ch, m->ref[r].rank, &slot, id, icon);
    }
}

void repo_map_epg(void) {
    mctx m;
    memset(&m, 0, sizeof m);
    int n = R.nch;
    m.ref = malloc((size_t)(n * 3 + 1) * sizeof *m.ref);
    m.best_src = malloc((size_t)(n + 1) * sizeof(int));
    m.best_rank = malloc((size_t)(n + 1) * sizeof(int));
    m.best_slot = malloc((size_t)(n + 1) * sizeof(int));
    for (int s = 0; s < R.nepg; s++) epg_slots_clear(&R.epg[s]);
    if (m.ref && m.best_src && m.best_rank && m.best_slot) {
        smap_init(&m.ids, n + 16);
        smap_init(&m.keys, n * 2 + 16);
        /* ссылки добавляются с конца: в списке ключа первым оказывается канал с меньшим номером */
        for (int i = n - 1; i >= 0; i--) {
            channel *c = &R.ch[i];
            mref_add(&m, &m.ids, c->tvg_id, i, 0);
            mref_add(&m, &m.keys, c->tkey, i, 1);
            if (strcmp(c->mkey, c->tkey)) mref_add(&m, &m.keys, c->mkey, i, 2);
            m.best_src[i] = -1;
            m.best_rank[i] = 99;
            m.best_slot[i] = -1;
        }
        for (int s = 0; s < R.nepg; s++) {
            m.src = s;
            epg_scan(&R.epg[s], m_chan, &m);
        }
        smap_free(&m.ids);
        smap_free(&m.keys);
    }
    for (int i = 0; i < n; i++) {
        channel *c = &R.ch[i];
        int ok = m.best_src && m.best_slot && m.best_src[i] >= 0;
        c->epg_src = ok ? m.best_src[i] : -1;
        c->epg_ch = ok ? m.best_slot[i] : -1;
    }
    free(m.ref);
    free(m.best_src);
    free(m.best_rank);
    free(m.best_slot);
    R.rev++;
}

void repo_open_epg(void) {
    for (int i = 0; i < R.nepg; i++) epg_close(&R.epg[i]);
    R.nepg = 0;
    playlist *p = repo_cur();
    if (!p) return;
    strlist u = {0};
    repo_epg_urls(p, &u);
    for (int i = 0; i < u.n && i < MAX_EPG; i++) {
        memset(&R.epg[i], 0, sizeof R.epg[i]);
        epg_open(&R.epg[i], u.v[i]);
        R.nepg++;
    }
    sl_clear(&u);
    repo_map_epg();
}

void repo_reopen_epg(int i) {
    if (i < 0 || i >= R.nepg) return;
    char url[2048];
    snprintf(url, sizeof url, "%s", R.epg[i].url);
    int loading = R.epg[i].loading;
    epg_open(&R.epg[i], url);
    R.epg[i].loading = loading;
    repo_map_epg();
}

int repo_epg_loading(void) {
    for (int i = 0; i < R.nepg; i++) if (R.epg[i].loading) return 1;
    return 0;
}

/* Источники, на которые ссылается хоть один плейлист, остаются; остальное — удалить. */
void repo_gc_epg(void) {
    if (repo_epg_loading()) return;   /* недописанные файлы идущей сборки не трогать */
    uint32_t keep[64 * MAX_EPG];
    int n = 0;
    strlist u = {0};
    for (int i = 0; i < R.npl; i++) {
        repo_epg_urls(&R.pl[i], &u);
        for (int k = 0; k < u.n && n < COUNT(keep); k++) keep[n++] = hash_str(u.v[k]);
    }
    sl_clear(&u);
    if (n == COUNT(keep)) return;   /* плейлистов слишком много — лучше ничего не удалять */
    epg_gc(keep, n);
}

/* Плейлисты, у которых среди источников программы есть url (их каналы нужны выгрузке) */
int repo_epg_users(const char *url, int *ids, int max) {
    int n = 0;
    strlist u = {0};
    for (int i = 0; i < R.npl && n < max; i++) {
        repo_epg_urls(&R.pl[i], &u);
        if (sl_find(&u, url) >= 0) ids[n++] = R.pl[i].id;
    }
    sl_clear(&u);
    return n;
}

/* Подпись плейлистов источника: сменилась (новый плейлист, перезагружен) — программу пора проверить: может, нужны
 * другие каналы (решает сборка по хэшу каналов, иначе — условный запрос). */
uint32_t repo_epg_users_sig(const char *url) {
    uint32_t h = 2166136261u;
    strlist u = {0};
    char b[400];
    for (int i = 0; i < R.npl; i++) {
        playlist *p = &R.pl[i];
        repo_epg_urls(p, &u);
        if (sl_find(&u, url) < 0) continue;
        int tag = p->etag[0] || p->last_modified[0];
        snprintf(b, sizeof b, "%d|%d|%s|%s|%lld", p->id, p->count, p->etag, p->last_modified, tag ? 0LL : (long long)p->loaded_at);
        h ^= hash_str(b);
        h *= 16777619u;
    }
    sl_clear(&u);
    return h ? h : 1;
}

int repo_prog_count(const channel *c) {
    if (!c || c->epg_src < 0 || c->epg_src >= R.nepg) return 0;
    return epg_count(&R.epg[c->epg_src], c->epg_ch);
}
prog repo_prog(const channel *c, int i) {
    prog r;
    memset(&r, 0, sizeof r);
    r.title = r.sub = r.desc = r.cat = r.icon = "";
    if (!c || c->epg_src < 0 || c->epg_src >= R.nepg) return r;
    epg_item it;
    if (epg_get(&R.epg[c->epg_src], c->epg_ch, i, &it)) return r;
    int64_t sh = (int64_t)(c->tvg_shift * 3600);
    r.start = it.start + sh;
    r.stop = it.stop + sh;
    r.title = it.title;
    r.sub = it.sub;
    r.desc = it.desc;
    r.cat = it.cat;
    r.icon = it.icon;
    return r;
}
/* первая передача с stop > t */
static int lower(const channel *c, int64_t t) {
    if (!repo_prog_count(c)) return -1;
    return epg_lower(&R.epg[c->epg_src], c->epg_ch, t - (int64_t)(c->tvg_shift * 3600));
}
int repo_prog_at(const channel *c, int64_t t) {
    int i = lower(c, t);
    if (i < 0 || i >= repo_prog_count(c)) return -1;
    prog p = repo_prog(c, i);
    return p.start <= t ? i : -1;
}
int repo_prog_after(const channel *c, int64_t t) {
    int i = lower(c, t);
    if (i < 0) return -1;
    int n = repo_prog_count(c);
    while (i < n && repo_prog(c, i).start < t) i++;
    return i < n ? i : -1;
}
const char *repo_epg_icon(const channel *c) {
    if (!c || c->epg_src < 0 || c->epg_src >= R.nepg) return "";
    return epg_slot_icon(&R.epg[c->epg_src], c->epg_ch);
}
const char *repo_logo(const channel *c) {
    const char *a = c->logo, *b = repo_epg_icon(c);
    if (P.logo_priority == 1) { const char *t = a; a = b; b = t; }
    return a && *a ? a : b ? b : "";
}

/* ---------- воспроизведение ---------- */
void repo_headers(const channel *c, char *out, size_t cap) {
    sbuf b = {0};
    if (c->ua && *c->ua) sb_printf(&b, "User-Agent: %s\r\n", c->ua);
    if (c->ref && *c->ref) sb_printf(&b, "Referer: %s\r\n", c->ref);
    if (c->headers_json && *c->headers_json) {
        json_doc *d = json_parse(c->headers_json, strlen(c->headers_json));
        if (d) {
            for (json *e = json_root(d) ? json_root(d)->child : NULL; e; e = e->next)
                if (e->key && e->type == J_STR) sb_printf(&b, "%s: %s\r\n", e->key, e->str);
            json_free(d);
        }
    }
    u8_copy(out, cap, b.s ? b.s : "");
    sb_free(&b);
}
int repo_has_archive(const channel *c) { return c && c->cu_type != NULL && c->archive_days > 0; }

int repo_archive_url(const channel *c, int64_t start, int64_t dur, char *out, size_t cap) {
    if (!repo_has_archive(c)) return -1;
    char tpl[4096];
    if (catchup_template(c->url, c->cu_type, c->cu_source, tpl, sizeof tpl)) return -1;
    catchup_prepare(tpl, start, dur, clock_now(), clock_tz, out, cap);
    return 0;
}

/* ---------- напоминания ---------- */
static void load_reminders(void) {
    free(R.rem);
    R.rem = NULL;
    R.nrem = 0;
    size_t n;
    char *t = file_read(data_path("reminders.json"), &n);
    if (!t) return;
    json_doc *d = json_parse(t, n);
    free(t);
    if (!d) return;
    json *a = json_root(d);
    int len = json_len(a);
    R.rem = calloc((size_t)(len > 0 ? len : 1), sizeof *R.rem);
    for (int i = 0; i < len && R.rem; i++) {
        json *o = json_at(a, i);
        reminder *r = &R.rem[R.nrem++];
        snprintf(r->key, sizeof r->key, "%s", json_str(json_get(o, "key"), ""));
        snprintf(r->name, sizeof r->name, "%s", json_str(json_get(o, "name"), ""));
        snprintf(r->logo, sizeof r->logo, "%s", json_str(json_get(o, "logo"), ""));
        snprintf(r->title, sizeof r->title, "%s", json_str(json_get(o, "title"), ""));
        r->num = (int)json_int(json_get(o, "num"), 0);
        r->playlist = (int)json_int(json_get(o, "playlist"), 0);
        r->start = json_int(json_get(o, "start"), 0);
        r->stop = json_int(json_get(o, "stop"), 0);
        r->snooze = json_int(json_get(o, "snooze"), 0);
        r->shown = (int)json_int(json_get(o, "shown"), 0);
    }
    json_free(d);
}
void repo_reminders_save(void) {
    sbuf b = {0};
    sb_add(&b, "[");
    for (int i = 0; i < R.nrem; i++) {
        reminder *r = &R.rem[i];
        sb_printf(&b, "%s\n{\"num\":%d,\"playlist\":%d,\"start\":%lld,\"stop\":%lld,\"snooze\":%lld,\"shown\":%d", i ? "," : "", r->num, r->playlist,
                  (long long)r->start, (long long)r->stop, (long long)r->snooze, r->shown);
        jput(&b, "key", r->key, 1);
        jput(&b, "name", r->name, 1);
        jput(&b, "logo", r->logo, 1);
        jput(&b, "title", r->title, 1);
        sb_add(&b, "}");
    }
    sb_add(&b, "\n]\n");
    file_write_atomic(data_path("reminders.json"), b.s, b.n);
    sb_free(&b);
}
int repo_reminder_find(const char *key, int64_t start) {
    for (int i = 0; i < R.nrem; i++) if (R.rem[i].start == start && !strcmp(R.rem[i].key, key)) return i;
    return -1;
}
void repo_reminder_add(const channel *c, const prog *p) {
    if (repo_reminder_find(c->key, p->start) >= 0) return;
    reminder *n = realloc(R.rem, (size_t)(R.nrem + 1) * sizeof *n);
    if (!n) return;
    R.rem = n;
    reminder *r = &R.rem[R.nrem++];
    memset(r, 0, sizeof *r);
    snprintf(r->key, sizeof r->key, "%s", c->key);
    snprintf(r->name, sizeof r->name, "%s", c->name);
    snprintf(r->logo, sizeof r->logo, "%s", repo_logo(c));
    snprintf(r->title, sizeof r->title, "%s", p->title);
    r->num = c->num;
    r->playlist = repo_cur() ? repo_cur()->id : 0;
    r->start = p->start;
    r->stop = p->stop;
    repo_reminders_save();
}
void repo_reminder_remove(int i) {
    if (i < 0 || i >= R.nrem) return;
    memmove(R.rem + i, R.rem + i + 1, (size_t)(R.nrem - i - 1) * sizeof *R.rem);
    R.nrem--;
    repo_reminders_save();
}

/* ---------- выбор плейлиста ---------- */
int repo_select(int index) {
    free_channels();
    R.cur = index >= 0 && index < R.npl ? index : -1;
    sl_clear(&R.fav);
    sl_clear(&R.history);
    sl_clear(&R.locked_ch);
    sl_clear(&R.locked_gr);
    if (R.cur < 0) { repo_apply_view(); return -1; }
    playlist *p = &R.pl[R.cur];
    load_list(&R.fav, "fav");
    load_list(&R.history, "hist");
    load_list(&R.locked_ch, "lockc");
    load_list(&R.locked_gr, "lockg");
    load_set(&R.hidden_ch, "hidc");
    load_set(&R.hidden_gr, "hidg");
    load_scores();
    int rc = load_channels(p->id);
    repo_apply_view();
    repo_open_epg();
    P.last_playlist = p->id;
    return rc;
}

/* После восстановления копии: всё заново из файлов (плейлисты, напоминания, пользовательские данные). */
void repo_reload(void) {
    for (int i = 0; i < R.nepg; i++) epg_close(&R.epg[i]);
    R.nepg = 0;
    free_channels();
    for (int i = 0; i < R.npl; i++) { free(R.pl[i].user_epg); free(R.pl[i].header_epg); }
    free(R.pl);
    R.pl = NULL;
    R.npl = 0;
    sl_clear(&R.fav);
    sl_clear(&R.history);
    sl_clear(&R.locked_ch);
    sl_clear(&R.locked_gr);
    smap_free(&R.hidden_ch);
    smap_free(&R.hidden_gr);
    smap_free(&R.scores);
    R.rev++;
    repo_init();
}

void repo_init(void) {
    load_playlists();
    load_reminders();
    smap_init(&R.hidden_ch, 16);
    smap_init(&R.hidden_gr, 16);
    smap_init(&R.scores, 16);
    R.cur = -1;
    repo_gc_epg();
    int idx = R.npl ? 0 : -1;
    for (int i = 0; i < R.npl; i++) if (R.pl[i].id == P.last_playlist) idx = i;
    repo_select(idx);
}
