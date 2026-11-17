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
#include "core/substyle.h"
#include "player/player.h"
#include <stdio.h>
#include <stdlib.h>
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
        /* закрытые субтитры в видео: CC1…CC4 (608), сервисы 708 */
        relay_cc cc[16];
        int ncc = relay_cc_list(cc, 16);
        for (int i = 0; i < ncc && n < max; i++) {
            subtrack *t = &out[n++];
            *t = (subtrack){ST_CC, i, 1, "", ""};
            int known = lang_find(cc[i].lang) >= 0 || (cc[i].lang[0] && strcmp(cc[i].lang, "und"));
            if (cc[i].kind == CC_608) {
                char ch[8];
                snprintf(ch, sizeof ch, "CC%d", cc[i].num);
                snprintf(t->key, sizeof t->key, "cc:%s", ch);
                if (known) snprintf(t->label, sizeof t->label, "%s · %s", lang_label(cc[i].lang), ch);
                else snprintf(t->label, sizeof t->label, "%s", trf(S_SUB_CC_CHANNEL, ch));
            } else {
                snprintf(t->key, sizeof t->key, "cc:S%d", cc[i].num);
                if (known && cc[i].num == 1) snprintf(t->label, sizeof t->label, "%s · CEA-708", lang_label(cc[i].lang));
                else if (known) snprintf(t->label, sizeof t->label, "%s · %s", lang_label(cc[i].lang), trf(S_SUB_CC_SERVICE, cc[i].num));
                else snprintf(t->label, sizeof t->label, "%s", trf(S_SUB_CC_SERVICE, cc[i].num));
            }
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
    if (!T.subs) { relay_dvb_select(-1); relay_cc_select(0, 0); webvtt_stop(); }
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
        relay_cc_select(0, 0);
        return;
    }
    /* закрытые субтитры: ключ — канал / сервис, а не язык */
    if (!strncmp(T.sub_key, "cc:CC", 5)) relay_cc_select(CC_608, atoi(T.sub_key + 5));
    else if (!strncmp(T.sub_key, "cc:S", 4)) relay_cc_select(CC_708, atoi(T.sub_key + 4));
    else relay_cc_select(0, 0);
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
/* Оформленный текст (метки core/substyle.h): куски одного начертания подряд по общей базовой линии. Строка — [s, e);
 * g == NULL — только ширина; maxw > 0 — не шире (остаток обрезается многоточием). */
static int sface(int f) { return f & SS_B ? (f & SS_I ? F_SUB_BI : F_SUB_B) : (f & SS_I ? F_SUB_I : F_SUB); }
static int styled(gfx_t *g, int x, int base, double em, uint32_t defc, const char *s, const char *e, int maxw) {
    int flags = 0, w = 0;
    uint32_t col = SS_NOCOLOR;
    char seg[512];
    for (const char *p = s; p < e;) {
        int l = ss_mark_len(p);
        if (l) { ss_apply(p, &flags, &col); p += l; continue; }
        const char *q = p;
        while (q < e && !ss_mark_len(q)) q++;
        size_t n = (size_t)(q - p) < sizeof seg ? (size_t)(q - p) : sizeof seg - 1;
        memcpy(seg, p, n);
        seg[n] = 0;
        p = q;
        int f = sface(flags), sw = d_text_w(em, f, seg);
        if (g) {
            int lim = maxw > 0 ? maxw - w : 0;
            if (maxw > 0 && lim <= 0) break;
            uint32_t c = col == SS_NOCOLOR ? defc : col;
            sw = d_text_base(g, x + w, base, em, f, c, seg, maxw > 0 && sw > lim ? lim : 0);
            if (flags & SS_U) { int t = E(em * 0.07); d_fill(g, x + w, base + E(em * 0.12), sw, t < 1 ? 1 : t, c); }
        }
        w += sw;
    }
    return w;
}
static int base_off(double em) { return text_ascent_w((int)(em * EM + 0.5), F_SUB); }

static void draw_text(gfx_t *g, const char *text, int bottom) {
    const char *ls[8], *le[8];
    int n = 0;
    for (const char *p = text; *p && n < 8;) { const char *e = strchr(p, '\n'); ls[n] = p; le[n] = e ? e : p + strlen(p); n++; if (!e) break; p = e + 1; }
    double em = 1.9;
    int lh = d_line_h(em, F_SUB) + E(0.6), y = bottom - n * lh;
    for (int i = 0; i < n; i++, y += lh) {
        int tw = styled(NULL, 0, 0, em, 0, ls[i], le[i], 0), w = tw + E(1.6);
        if (w > SW - E(8)) w = SW - E(8);
        int x = (SW - w) / 2;
        d_round(g, x, y, w, lh - E(0.3), E(0.4), 0xc0000000u);
        styled(g, x + E(0.8), y + E(0.3) + base_off(em), em, C_WHITE, ls[i], le[i], w - E(1.6));
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
    } else if (!strncmp(T.sub_key, "cc:", 3)) {
        /* CEA-608 / 708: строки на своих местах — сетка 15 строк в безопасной зоне 80 % экрана */
        static cc_show s;
        if (!relay_cc_at(pts, &s)) return;
        int ax = SW / 10, aw = SW * 8 / 10, ay = SH / 10, ah = SH * 8 / 10, rh = ah / 15;
        double em = rh * 0.62 / EM;
        for (int i = 0; i < s.n; i++) {
            cc_row *r = &s.row[i];
            const char *te = r->text + strlen(r->text);
            int w = styled(NULL, 0, 0, em, 0, r->text, te, 0) + E(1.0);
            if (w > aw) w = aw;
            int x = ax + (int)(r->x * aw), y = ay + (int)(r->y * ah);
            if (r->align == 1) x -= w / 2;
            else if (r->align == 2) x -= w;
            if (x + w > ax + aw) x = ax + aw - w;
            if (x < ax) x = ax;
            d_fill(g, x, y, w, rh, 0xc0000000u);
            styled(g, x + E(0.5), y + (rh - d_line_h(em, F_SUB)) / 2 + base_off(em), em, 0xffebebebu, r->text, te, w - E(1.0));
        }
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
