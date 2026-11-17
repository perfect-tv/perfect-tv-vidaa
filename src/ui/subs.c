/* Субтитры: дорожки, выбор, показ (docs/controls.md, «Субтитры»). Три вида, которые умеем показывать сами (системный
 * плеер субтитры не показывает): телетекст (core/ttx.c), DVB-картинки (core/dvbsub.c), WebVTT и TTML / IMSC из
 * HLS (core/webvtt.c) — все берутся из потока через ретранслятор. Остальные форматы видны в списке как «не
 * поддерживается». Дорожки: поток через ретранслятор — из его разбора (+ прочие из системного плеера); напрямую —
 * из системного плеера (выбор переоткрывает поток через ретранслятор). Выбор — ключ «вид:язык[:слабослышащие]»,
 * хранится за каналом (subs.<ключ канала>) и каждый кадр сопоставляется с дорожкой ретранслятора. */
#include "screens.h"
#include "tv.h"
#include "u.h"
#include "app.h"
#include "core/lang.h"
#include "core/relay.h"
#include "core/ttx.h"
#include "core/dvbsub.h"
#include "core/webvtt.h"
#include "player/player.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* ---------- языки: код ISO 639 <-> название (системный плеер подписывает дорожки по-русски) ---------- */
static const struct { const char *c3, *c3b, *c2, *ru, *en; } LANGS[] = {
    {"rus", "", "ru", "Русский", "Russian"},      {"eng", "", "en", "Английский", "English"},   {"ukr", "", "uk", "Украинский", "Ukrainian"},
    {"bel", "", "be", "Белорусский", "Belarusian"}, {"kaz", "", "kk", "Казахский", "Kazakh"},     {"deu", "ger", "de", "Немецкий", "German"},
    {"fra", "fre", "fr", "Французский", "French"},  {"spa", "", "es", "Испанский", "Spanish"},     {"ita", "", "it", "Итальянский", "Italian"},
    {"pol", "", "pl", "Польский", "Polish"},        {"tur", "", "tr", "Турецкий", "Turkish"},     {"jpn", "", "ja", "Японский", "Japanese"},
    {"zho", "chi", "zh", "Китайский", "Chinese"},   {"ara", "", "ar", "Арабский", "Arabic"},      {"heb", "", "he", "Иврит", "Hebrew"},
};
#define NLANGS (int)(sizeof LANGS / sizeof LANGS[0])
static int lang_find(const char *s) {
    if (!s || !*s) return -1;
    for (int i = 0; i < NLANGS; i++)
        if (!strcasecmp(s, LANGS[i].c3) || (LANGS[i].c3b[0] && !strcasecmp(s, LANGS[i].c3b)) || !strcasecmp(s, LANGS[i].c2) ||
            !strcmp(s, LANGS[i].ru) || !strcasecmp(s, LANGS[i].en))
            return i;
    return -1;
}
/* код для ключа выбора: известный язык — трёхбуквенный, иначе как есть (нижним регистром) */
static void lang_code(const char *s, char *out, size_t cap) {
    int i = lang_find(s);
    if (i >= 0) { snprintf(out, cap, "%s", LANGS[i].c3); return; }
    u8_lower_str(out, cap, s && *s ? s : "und");
}
static const char *lang_label(const char *s) {
    int i = lang_find(s);
    if (i >= 0) return lang_is_ru() ? LANGS[i].ru : LANGS[i].en;
    return s && *s && strcmp(s, "und") ? s : tr(S_SUB_UNKNOWN_LANG);
}

/* ---------- дорожки ---------- */
static int player_has(const char *codec) {
    for (int i = 0; i < pl_sub_count(); i++) if (!strcmp(pl_sub_codec(i), codec)) return 1;
    return 0;
}

int subs_tracks(subtrack *out, int max) {
    int n = 0;
    int parsed = T.relayed && relay_ts_bytes() > 0;
    if (parsed) {
        if (relay_has_teletext() && ttx_subtitles_known(relay_ttx()) && n < max) {
            subtrack *t = &out[n++];
            *t = (subtrack){ST_TTX, 0, 1, "ttx", ""};
            snprintf(t->label, sizeof t->label, "%s", tr(S_SUB_TELETEXT));
        }
        relay_dvb d[8];
        int nd = relay_dvb_list(d, 8);
        for (int i = 0; i < nd && n < max; i++) {
            subtrack *t = &out[n++];
            memset(t, 0, sizeof *t);
            int hoh = d[i].type >= 0x20 && d[i].type <= 0x24;
            char lc[16];
            lang_code(d[i].lang, lc, sizeof lc);
            *t = (subtrack){ST_DVB, i, 1, "", ""};
            snprintf(t->key, sizeof t->key, "dvb:%s:%d", lc, hoh);
            snprintf(t->label, sizeof t->label, "%s · DVB%s%s", lang_label(d[i].lang), hoh ? " · " : "", hoh ? tr(S_SUB_HOH) : "");
        }
        static relay_vtt v[8];
        int nv = relay_vtt_list(v, 8);
        for (int i = 0; i < nv && n < max; i++) {
            subtrack *t = &out[n++];
            char lc[16];
            lang_code(v[i].lang[0] ? v[i].lang : v[i].name, lc, sizeof lc);
            *t = (subtrack){ST_VTT, i, 1, "", ""};
            snprintf(t->key, sizeof t->key, "vtt:%s", lc);
            /* название рендиции — если оно не служебное («subtitle_0» у FFmpeg); иначе — язык */
            int generic = !v[i].name[0] || !strncasecmp(v[i].name, "subtitle", 8) || !strncasecmp(v[i].name, "subs", 4);
            const char *fmt = !strcmp(v[i].codec, "stpp") ? "TTML" : "WebVTT";
            if (generic || lang_find(v[i].name) >= 0) snprintf(t->label, sizeof t->label, "%s · %s", lang_label(v[i].lang), fmt);
            else snprintf(t->label, sizeof t->label, "%s · %s · %s", lang_label(v[i].lang), v[i].name, fmt);
        }
    }
    /* системный плеер: напрямую — всё; через ретранслятор — только то, чего ретранслятор не умеет */
    for (int i = 0; i < pl_sub_count() && n < max; i++) {
        const char *codec = pl_sub_codec(i);
        int ttx = !strcmp(codec, "dvb_teletext"), dvb = !strcmp(codec, "dvb_subtitle"), vtt = !strcmp(codec, "webvtt") || !strcmp(codec, "ttml");
        if (parsed && (ttx || dvb || vtt)) continue;
        if (ttx) {   /* телетекст: одна строка, даже если дорожек несколько */
            int dup = 0;
            for (int k = 0; k < n; k++) dup |= out[k].kind == ST_TTX;
            if (dup) continue;
        }
        subtrack *t = &out[n++];
        char lc[16];
        lang_code(pl_sub_label(i), lc, sizeof lc);
        *t = (subtrack){ttx ? ST_TTX : dvb ? ST_DVB : vtt ? ST_VTT : ST_OTHER, -1, ttx || dvb || vtt, "", ""};
        if (ttx) { snprintf(t->key, sizeof t->key, "ttx"); snprintf(t->label, sizeof t->label, "%s", tr(S_SUB_TELETEXT)); }
        else {
            snprintf(t->key, sizeof t->key, "%s:%s%s", dvb ? "dvb" : vtt ? "vtt" : "other", lc, dvb ? ":0" : "");
            snprintf(t->label, sizeof t->label, "%s · %s", lang_label(pl_sub_label(i)), dvb ? "DVB" : !strcmp(codec, "ttml") ? "TTML" : vtt ? "WebVTT" : codec);
        }
    }
    return n;
}

int subs_available(void) { subtrack t[16]; return subs_tracks(t, 16); }

/* ---------- выбор ---------- */
static char key_store[200];
static const char *store_key(void) {
    snprintf(key_store, sizeof key_store, "subs.%s", T.cur ? T.cur->key : "");
    return key_store;
}

/* сохранённый выбор канала (прежнее «1» — телетекст) */
void subs_load(void) {
    const char *v = T.cur ? store_get(store_key()) : NULL;
    snprintf(T.sub_key, sizeof T.sub_key, "%s", !v ? "" : !strcmp(v, "1") ? "ttx" : v);
    T.subs = T.sub_key[0] != 0;
}

void subs_select(const char *key) {
    snprintf(T.sub_key, sizeof T.sub_key, "%s", key ? key : "");
    T.subs = T.sub_key[0] != 0;
    if (T.cur) store_set(store_key(), T.subs ? T.sub_key : NULL);
    if (!T.subs) { relay_dvb_select(-1); webvtt_stop(); }
    app_dirty();
}

const char *subs_current_label(void) {
    if (!T.subs) return tr(S_OFF);
    static subtrack t[16];
    int n = subs_tracks(t, 16);
    for (int i = 0; i < n; i++) if (!strcmp(t[i].key, T.sub_key)) return t[i].label;
    return T.sub_key;
}

/* ключ «вид:язык[:слабослышащие]» -> дорожка ретранслятора (тот же язык; нет — первая того же вида) */
static int match(const subtrack *t, int n, int kind, const char *key) {
    int any = -1;
    for (int i = 0; i < n; i++) {
        if (t[i].kind != kind || !t[i].supported || t[i].idx < 0) continue;
        if (!strcmp(t[i].key, key)) return i;
        if (any < 0) any = i;
    }
    /* тот же язык без признака «для слабослышащих» */
    for (int i = 0; i < n; i++)
        if (t[i].kind == kind && t[i].idx >= 0 && !strncmp(t[i].key, key, strlen(key) > 2 ? strlen(key) - 2 : 0)) return i;
    return any;
}

static char vtt_url[4096];

/* каждый кадр: выбор -> декодер ретранслятора */
void subs_apply(void) {
    webvtt_clock(tv_frame_pts(), pl_position());
    if (!T.subs || !T.relayed) {
        if (relay_dvb_selected() >= 0) relay_dvb_select(-1);
        if (webvtt_running()) { webvtt_stop(); vtt_url[0] = 0; }
        return;
    }
    static subtrack t[16];
    int n = subs_tracks(t, 16);
    int kind = !strncmp(T.sub_key, "dvb:", 4) ? ST_DVB : !strncmp(T.sub_key, "vtt:", 4) ? ST_VTT : !strcmp(T.sub_key, "ttx") ? ST_TTX : 0;
    int i = kind == ST_DVB || kind == ST_VTT ? match(t, n, kind, T.sub_key) : -1;
    int dvb = kind == ST_DVB && i >= 0 ? t[i].idx : -1;
    if (relay_dvb_selected() != dvb) relay_dvb_select(dvb);
    if (kind == ST_VTT && i >= 0) {
        relay_vtt v[8];
        int nv = relay_vtt_list(v, 8);
        if (t[i].idx < nv && strcmp(vtt_url, v[t[i].idx].url)) {
            snprintf(vtt_url, sizeof vtt_url, "%s", v[t[i].idx].url);
            webvtt_start(v[t[i].idx].url, v[t[i].idx].hdr);
        }
    } else if (webvtt_running()) { webvtt_stop(); vtt_url[0] = 0; }
}

/* поток переоткрыт (новая ссылка): WebVTT — заново по новому главному плейлисту */
void subs_stream_changed(void) {
    webvtt_stop();
    vtt_url[0] = 0;
}

/* ---------- показ ---------- */
static void draw_text(gfx_t *g, char *text, int bottom) {
    const char *lines[8];
    int n = 0;
    for (char *p = text; *p && n < 8;) { lines[n++] = p; char *e = strchr(p, '\n'); if (!e) break; *e = 0; p = e + 1; }
    int lh = d_line_h(1.9, F_S) + E(0.6), y = bottom - n * lh;
    for (int i = 0; i < n; i++, y += lh) {
        int w = d_text_w(1.9, F_S, lines[i]) + E(1.6);
        if (w > SW - E(8)) w = SW - E(8);
        d_round(g, (SW - w) / 2, y, w, lh - E(0.3), E(0.4), 0xc0000000u);
        d_text_c(g, (SW - w) / 2, w, y + E(0.3), 1.9, F_S, C_WHITE, lines[i]);
    }
}

void subs_draw(gfx_t *g, int bottom) {
    if (!T.subs || !T.relayed || !pl_has_picture()) return;
    int64_t pts = tv_frame_pts();
    if (pts < 0) return;
    static char text[2048];
    if (!strcmp(T.sub_key, "ttx")) {
        static ttx_page pg;
        static ttx_grid gr;
        if (ttx_subtitle_at(relay_ttx(), pts, &pg)) return;
        ttx_layout(&pg, NULL, &gr);
        if (ttx_subtitle_text(&gr, text, sizeof text)) draw_text(g, text, bottom);
    } else if (!strncmp(T.sub_key, "vtt:", 4)) {
        if (webvtt_text_at(pts, text, sizeof text)) draw_text(g, text, bottom);
    } else if (!strncmp(T.sub_key, "dvb:", 4)) {
        /* картинки DVB: свой экран (обычно 720×576) растягивается на весь; копия — только при новом показе */
        static dvbsub_show cache;
        static dvbsub *from;
        dvbsub *d = relay_dvbsub();
        if (!d) return;
        if (d != from) { dvbsub_show_clear(&cache); cache.id = 0; from = d; }
        int rc = dvbsub_at(d, pts, cache.id, &cache);
        if (rc < 0) return;
        for (int i = 0; i < cache.nreg; i++) {
            dvbsub_region *r = &cache.reg[i];
            int x = r->x * SW / cache.dw, y = r->y * SH / cache.dh, w = r->w * SW / cache.dw, h = r->h * SH / cache.dh;
            d_bitmap(g, r->px, r->w, r->h, x, y, w, h);
        }
    }
}

/* ---------- меню «Субтитры» ---------- */
static subtrack M[16];
static int nm;
static void chosen(int i, void *c) {
    (void)c;
    if (i <= 0) { subs_select(NULL); tv_toast(tr(S_SUBS_OFF)); return; }
    subtrack *t = &M[i - 1];
    if (!t->supported) { tv_toast(tr(S_SUB_UNSUPPORTED)); return; }
    subs_select(t->key);
    tv_subtitles_need_relay();
    tv_toast(t->label);
}
void subs_menu_open(void) {
    nm = subs_tracks(M, 16);
    static char labels[17][200];
    static const char *items[17];
    int sel = 0;
    snprintf(labels[0], sizeof labels[0], "%s", tr(S_OFF));
    for (int i = 0; i < nm; i++) {
        snprintf(labels[i + 1], sizeof labels[i + 1], "%s%s", M[i].label, M[i].supported ? "" : tr(S_SUB_NOT_SUPPORTED_SUFFIX));
        if (T.subs && !strcmp(M[i].key, T.sub_key)) sel = i + 1;
    }
    for (int i = 0; i <= nm; i++) items[i] = labels[i];
    choose_open(tr(S_SUBTITLES_MENU), items, nm + 1, sel, chosen, NULL);
}
/* кнопка SUBTITLE: выкл. — или включить запомненные / первые, что умеем показать */
void subs_toggle(void) {
    if (T.subs) { subs_select(NULL); tv_toast(tr(S_SUBS_OFF)); return; }
    const char *v = T.cur ? store_get(store_key()) : NULL;
    nm = subs_tracks(M, 16);
    const char *key = NULL;
    for (int i = 0; i < nm && !key; i++) if (M[i].supported) key = M[i].key;
    if (v && *v) key = !strcmp(v, "1") ? "ttx" : v;
    if (!key) { tv_toast(tr(S_SUB_NONE)); return; }
    subs_select(key);
    tv_subtitles_need_relay();
    tv_toast(subs_current_label());
}
