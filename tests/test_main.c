/* Юнит-тесты ядра на ПК: python3 tools/build.py --tests. Те же случаи, что в тестах Android- и веб-версии. */
#define _GNU_SOURCE
#include "app.h"
#include "core/clock.h"
#include "core/lang.h"
#include "core/prefs.h"
#include "core/unpack.h"
#include "core/util.h"
#include "data/catchup.h"
#include "data/epg.h"
#include "data/m3u.h"
#include "data/names.h"
#include "data/xmltv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

extern const unsigned char ptv_font_regular[], ptv_font_regular_end[], ptv_font_bold[], ptv_font_bold_end[];

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: CHECK %s\n", __FILE__, __LINE__, #c); } } while (0)
#define EQS(a, b) do { const char *a_ = (a), *b_ = (b); checks++; if (strcmp(a_ ? a_ : "(null)", b_)) { fails++; fprintf(stderr, "%s:%d: %s\n   got: «%s»\n  want: «%s»\n", __FILE__, __LINE__, #a, a_ ? a_ : "(null)", b_); } } while (0)
#define EQI(a, b) do { long long a_ = (a), b_ = (b); checks++; if (a_ != b_) { fails++; fprintf(stderr, "%s:%d: %s = %lld, want %lld\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

/* ---------- M3U ---------- */
#define MAXCH 16
static struct { char name[200], url[400], group[100], logo[200], tvg_id[100], ua[300], ref[200], hdr[200], cu_type[40], cu_src[200]; int num, cu, days; double shift; } got[MAXCH];
static int ngot;
static void on_ch(const m3u_channel *c, void *ctx) {
    (void)ctx;
    if (ngot >= MAXCH) return;
    got[ngot].num = c->num;
    snprintf(got[ngot].name, sizeof got[0].name, "%s", c->name);
    snprintf(got[ngot].url, sizeof got[0].url, "%s", c->url);
    snprintf(got[ngot].group, sizeof got[0].group, "%s", c->group);
    snprintf(got[ngot].logo, sizeof got[0].logo, "%s", c->logo);
    snprintf(got[ngot].tvg_id, sizeof got[0].tvg_id, "%s", c->tvg_id);
    snprintf(got[ngot].ua, sizeof got[0].ua, "%s", c->user_agent);
    snprintf(got[ngot].ref, sizeof got[0].ref, "%s", c->referrer);
    snprintf(got[ngot].hdr, sizeof got[0].hdr, "%s", c->headers_json);
    got[ngot].cu = c->has_catchup;
    if (c->has_catchup) {
        snprintf(got[ngot].cu_type, sizeof got[0].cu_type, "%s", c->cu_type);
        snprintf(got[ngot].cu_src, sizeof got[0].cu_src, "%s", c->cu_source);
        got[ngot].days = c->cu_days;
    }
    got[ngot].shift = c->tvg_shift;
    ngot++;
}
static m3u *parse_m3u(const char *text, size_t chunk) {
    ngot = 0;
    m3u *p = m3u_new(on_ch, NULL);
    size_t n = strlen(text);
    for (size_t i = 0; i < n; i += chunk) m3u_push(p, text + i, MIN(chunk, n - i));
    m3u_end(p);
    return p;
}

static void test_m3u(void) {
    const char *pl =
        "\xef\xbb\xbf#EXTM3U url-tvg=\"http://e1/a.xml.gz,http://e2/b.xml\" catchup=\"shift\" catchup-days=\"3\"\r\n"
        "#EXTINF:-1 tvg-id=\"ch1\" tvg-logo=\"http://l/1.png\" group-title=\"Новости\" http-user-agent=\"Mozilla/5.0 (KHTML, like Gecko) x\",Первый канал ⓢ\r\n"
        "http://s/1.m3u8\r\n"
        "#EXTGRP:Кино\n"
        "#EXTINF:0,Фильм  HD\n"
        "#EXTVLCOPT:http-referrer=http://ref/\n"
        "#EXTHTTP:{\"Cookie\":\"a=1\"}\n"
        "http://s/2.ts\n"
        "#EXTINF:-1 catchup=\"disabled\",Без архива\n"
        "http://s/3\n"
        "#EXTINF:-1,YouTube\n"
        "https://www.youtube.com/watch?v=1\n"
        "http://s/4\n"
        "#EXTINF:-1 catchup=\"flussonic\" catchup-days=\"7\" tvg-shift=\"-2\",Архивный\n"
        "http://s/5/index.m3u8";
    size_t chunks[] = {1, 7, 4096};
    for (int k = 0; k < 3; k++) {
        m3u *p = parse_m3u(pl, chunks[k]);
        EQI(ngot, 5);
        EQS(m3u_epg_urls(p), "http://e1/a.xml.gz\nhttp://e2/b.xml");
        EQS(got[0].name, "Первый канал");
        EQS(got[0].group, "Новости");
        EQS(got[0].ua, "Mozilla/5.0 (KHTML, like Gecko) x");
        EQS(got[0].tvg_id, "ch1");
        EQI(got[0].cu, 1);
        EQS(got[0].cu_type, "shift");
        EQI(got[0].days, 3);
        EQS(got[1].name, "Фильм HD");
        EQS(got[1].group, "Кино");
        EQS(got[1].ref, "http://ref/");
        EQS(got[1].hdr, "{\"Cookie\":\"a=1\"}");
        EQI(got[2].cu, 0);
        EQS(got[3].name, "Ch 4");   /* без #EXTINF, после пропущенного YouTube */
        EQI(got[3].num, 4);
        EQS(got[4].cu_type, "flussonic");
        EQI(got[4].days, 7);
        CHECK(got[4].shift == -2);
        m3u_free(p);
    }
    m3u *p = parse_m3u("<html>\n#EXTM3U\nhttp://x\n", 100);
    CHECK(m3u_rejected(p));
    EQI(ngot, 0);
    m3u_free(p);
    /* обрыв посередине */
    p = parse_m3u("#EXTM3U\n#EXTINF:-1,A\nhttp://a\n#EXTINF:-1,B\nhttp://b", 3);
    EQI(ngot, 2);
    m3u_free(p);
}

/* ---------- имена ---------- */
static void test_names(void) {
    char b[200];
    names_key("Первый Канал HD!", b, sizeof b); EQS(b, "первыйканалhd");
    names_short("Первый канал (архив)", b, sizeof b); EQS(b, "первый");
    names_short("Телеканал Звезда", b, sizeof b); EQS(b, "звезда");
    names_short("Канал 5 (+2)", b, sizeof b); EQS(b, "канал5 +2");
    names_short("Discovery Channel orig", b, sizeof b); EQS(b, "discovery channel");
    names_match_key("НТВ", b, sizeof b);
    char c[200];
    names_match_key("HTB", c, sizeof c);
    EQS(b, c);
    names_monogram("НТВ", b, sizeof b); EQS(b, "НТВ");
    names_monogram("Россия 24", b, sizeof b); EQS(b, "Р24");
    names_monogram("Матч! ТВ", b, sizeof b); EQS(b, "МТ");
    names_monogram("Discovery", b, sizeof b); EQS(b, "DI");
    names_monogram("!!!", b, sizeof b); EQS(b, "?");
}

/* ---------- архив ---------- */
static int tz_msk(int64_t t) { (void)t; return 10800; }
static int tz_utc(int64_t t) { (void)t; return 0; }
static void test_catchup(void) {
    char t[1024], u[1024];
    CHECK(!catchup_template("http://h/ch/index.m3u8", "shift", "", t, sizeof t));
    EQS(t, "http://h/ch/index.m3u8?utc=${start}&lutc=${timestamp}");
    catchup_template("http://h/ch/index.m3u8?a=1", "", "", t, sizeof t);
    EQS(t, "http://h/ch/index.m3u8?a=1&utc=${start}&lutc=${timestamp}");
    catchup_template("http://h/ch/index.m3u8?token=1", "flussonic", "", t, sizeof t);
    EQS(t, "http://h/ch/archive-${start}-${duration}.m3u8?token=1");
    catchup_prepare(t, 1700000000, 5400, 1700003000, tz_utc, u, sizeof u);
    EQS(u, "http://h/ch/archive-1700000000-5400.m3u8?token=1");
    CHECK(catchup_flussonic_hls("flussonic", t));
    char ev[1024];
    catchup_flussonic_now(t, ev, sizeof ev);
    catchup_prepare(ev, 1700000000, 5400, 1700003000, tz_utc, u, sizeof u);
    EQS(u, "http://h/ch/archive-1700000000-now.m3u8?token=1");
    catchup_template("http://h/ch/mpegts?token=1", "fs", "", t, sizeof t);
    EQS(t, "http://h/ch/timeshift_abs-${start}.ts?token=1");
    CHECK(!catchup_flussonic_hls("fs", t));
    catchup_template("http://h/325/mono.m3u8?token=s", "flussonic-hls", "", t, sizeof t);
    EQS(t, "http://h/325/mono-${start}-${duration}.m3u8?token=s");
    catchup_template("http://h/325/video.ts", "flussonic-ts", "", t, sizeof t);
    EQS(t, "http://h/325/video-${start}-${duration}.ts");
    catchup_template("http://h/325/playlist.m3u8", "Flussonic", "", t, sizeof t);
    EQS(t, "http://h/325/archive-${start}-${duration}.m3u8");
    catchup_template("http://h:8080/live/u/p/1477.m3u8", "xc", "", t, sizeof t);
    EQS(t, "http://h:8080/timeshift/u/p/${(d)M}/${(b)yyyy-MM-dd:HH-mm}/1477.m3u8");
    catchup_template("http://h/ch.m3u8", "append", "?s=${start}", t, sizeof t);
    EQS(t, "http://h/ch.m3u8?s=${start}");
    CHECK(catchup_template("http://h/ch.m3u8", "disabled", "", t, sizeof t) < 0);
    catchup_prepare("http://h/archive-${start}-${duration}.m3u8?d=${(d)M}&b=${(bu)yyyy-MM-dd:HH-mm}", 1700000000, 3600, 1700010000, tz_msk, u, sizeof u);
    EQS(u, "http://h/archive-1700000000-3600.m3u8?d=60&b=2023-11-14%3A22-13");
    catchup_prepare("${(b)HH:mm}|${(b+1)HH}|${offset}|${timestamp}", 1700000000, 3600, 1700010000, tz_msk, u, sizeof u);
    EQS(u, "01%3A13|02|10000|1700010000");
}

/* ---------- XMLTV ---------- */
typedef struct { int slot; int64_t start, stop; char title[100], desc[200], cat[50], icon[100]; } xp;
static xp xps[64];
static int nxp, nxc, before_calls;
static char xc_names[8][200];
static void x_ch(void *ctx, const char *id, const char *const *names, int n, const char *icon) {
    (void)ctx; (void)icon;
    if (nxc < 8) { snprintf(xc_names[nxc], sizeof xc_names[0], "%s:%s%s%s", id, n ? names[0] : "", n > 1 ? "|" : "", n > 1 ? names[1] : ""); }
    nxc++;
}
static void x_before(void *ctx) { (void)ctx; before_calls++; }
static int x_wanted(void *ctx, const char *id) { (void)ctx; return !strcmp(id, "a") ? 0 : !strcmp(id, "b&c") ? 1 : -1; }
static void x_prog(void *ctx, int slot, int64_t start, int64_t stop, const char *t, const char *s, const char *d, const char *c, const char *i) {
    (void)ctx; (void)s;
    if (nxp >= 64) return;
    xp *p = &xps[nxp++];
    p->slot = slot; p->start = start; p->stop = stop;
    snprintf(p->title, sizeof p->title, "%s", t);
    snprintf(p->desc, sizeof p->desc, "%s", d);
    snprintf(p->cat, sizeof p->cat, "%s", c);
    snprintf(p->icon, sizeof p->icon, "%s", i);
}
static void parse_xml(const char *xml, size_t chunk, const char *lang) {
    nxp = nxc = before_calls = 0;
    xmltv_cb cb = {.on_channel = x_ch, .before_programmes = x_before, .wanted = x_wanted, .on_programme = x_prog, .nlangs = 2};
    cb.langs[0] = lang;
    cb.langs[1] = "ru";
    xmltv *x = xmltv_new(&cb);
    size_t n = strlen(xml);
    for (size_t i = 0; i < n; i += chunk) xmltv_push(x, xml + i, MIN(chunk, n - i));
    xmltv_end(x);
    xmltv_free(x);
}
static void test_xmltv(void) {
    EQI(xmltv_time("20231114220000 +0300", 20), 1699988400);
    EQI(xmltv_time("20231114190000", 14), 1699988400);
    EQI(xmltv_time("202311141900", 12), 1699988400);
    EQI(xmltv_time("20231114220000 +03:00", 21), 1699988400);
    EQI(xmltv_time("bad", 3), 0);
    const char *xml =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE tv SYSTEM \"xmltv.dtd\">\n<tv>"
        "<channel id=\"a\"><display-name lang=\"ru\">Первый</display-name><display-name>Channel One</display-name><icon src=\"http://i/a.png\"/></channel>"
        "<channel id=\"b&amp;c\"><display-name>B</display-name></channel>"
        "<!-- comment <programme> -->"
        "<programme start=\"20231114190000 +0000\" stop=\"20231114200000 +0000\" channel=\"a\">"
        "<title lang=\"en\">News</title><title lang=\"ru\">Новости</title><desc><![CDATA[Описание <b>с</b> тегом]]></desc>"
        "<category lang=\"ru\">Инфо</category><icon src=\"http://i/p.jpg\" /></programme>"
        "<programme start=\"20231114200000 +0000\" channel=\"a\"><title>Без конца &amp; &#1046;</title></programme>"
        "<programme start=\"20231114213000 +0000\" stop=\"20231114220000 +0000\" channel=\"x\"><title>чужой</title></programme>"
        "<programme start=\"20231114210000 +0000\" stop=\"20231114220000 +0000\" channel=\"a\"><title>Третья</title><desc>a<br/>b</desc></programme>"
        "<programme start=\"20231114210000 +0000\" stop=\"20231114220000 +0000\" channel=\"b&amp;c\"/>"
        "</tv>";
    size_t chunks[] = {1, 13, 1 << 20};
    for (int k = 0; k < 3; k++) {
        parse_xml(xml, chunks[k], "ru");
        EQI(nxc, 2);
        EQS(xc_names[0], "a:Первый|Channel One");
        EQS(xc_names[1], "b&c:B");
        EQI(before_calls, 1);
        EQI(nxp, 4);
        EQS(xps[0].title, "Новости");
        EQS(xps[0].desc, "Описание <b>с</b> тегом");
        EQS(xps[0].cat, "Инфо");
        EQS(xps[0].icon, "http://i/p.jpg");
        EQS(xps[1].title, "Без конца & Ж");
        EQI(xps[1].stop, 1699995600);   /* до начала следующей */
        EQS(xps[2].desc, "ab");
        EQI(xps[3].slot, 1);
    }
    parse_xml(xml, 1 << 20, "en");
    EQS(xps[0].title, "News");
    /* windows-1251 */
    const char *x1251 = "<?xml version=\"1.0\" encoding=\"windows-1251\"?><tv><channel id=\"a\"><display-name>\xcf\xe5\xf0\xe2\xfb\xe9</display-name></channel>"
                        "<programme start=\"20231114190000\" stop=\"20231114200000\" channel=\"a\"><title>\xc8\xed\xf4\xee</title></programme></tv>";
    parse_xml(x1251, 5, "ru");
    EQS(xc_names[0], "a:Первый");
    EQS(xps[0].title, "Инфо");
}

/* ---------- распаковка ---------- */
typedef struct { const unsigned char *p; size_t n, pos, step; } memsrc;
static long mem_read(void *ctx, void *buf, size_t n) {
    memsrc *m = ctx;
    size_t k = MIN(n, MIN(m->step, m->n - m->pos));
    memcpy(buf, m->p + m->pos, k);
    m->pos += k;
    return (long)k;
}
static void test_unpack(void) {
    const char *text = "#EXTM3U\n#EXTINF:-1,Канал\nhttp://x\n";
    unsigned char gz[512];
    z_stream z = {0};
    deflateInit2(&z, 9, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    z.next_in = (unsigned char *)text;
    z.avail_in = (uInt)strlen(text);
    z.next_out = gz;
    z.avail_out = sizeof gz;
    deflate(&z, Z_FINISH);
    size_t gzn = sizeof gz - z.avail_out;
    deflateEnd(&z);
    memsrc m = {gz, gzn, 0, 3};
    up *u = up_new(mem_read, &m);
    char out[256];
    size_t n = 0;
    long r;
    while ((r = up_read(u, out + n, 5)) > 0) n += (size_t)r;
    out[n] = 0;
    EQS(out, text);
    EQS(up_kind(u), "gzip");
    up_free(u);
    memsrc m2 = {(const unsigned char *)"\xfd" "7zXZ\0\0", 6, 0, 100};
    u = up_new(mem_read, &m2);
    CHECK(up_read(u, out, sizeof out) < 0);
    EQS(up_error(u), "format");
    up_free(u);
}

/* ---------- хранилище программы ---------- */
static void test_epg(void) {
    int64_t now = 1700000000;
    clock_fake(now);
    char a1[200], a2[200], b1[200];
    names_match_key("Первый", a1, sizeof a1);
    names_match_key("Второй", a2, sizeof a2);
    names_match_key("B", b1, sizeof b1);
    epg_want w[2] = {{"", a1, a1, 3}, {"", b1, b1, 0}};
    (void)a2;
    const char *xml1 =
        "<tv><channel id=\"a\"><display-name>Первый</display-name></channel><channel id=\"b\"><display-name>B</display-name></channel>"
        "<programme start=\"20231114190000\" stop=\"20231114200000\" channel=\"a\"><title>Старая</title></programme>"
        "<programme start=\"20231114200000\" stop=\"20231114210000\" channel=\"a\"><title>Вторая</title></programme>"
        "<programme start=\"20231114200000\" stop=\"20231114203000\" channel=\"b\"><title>B1</title></programme>"
        "<programme start=\"20231114202000\" stop=\"20231114210000\" channel=\"b\"><title>B2</title></programme>"
        "<programme start=\"20201114200000\" stop=\"20201114210000\" channel=\"a\"><title>Слишком старая</title></programme>"
        "</tv>";
    uint32_t h = hash_str("http://test/epg");
    epg_remove_files(h);
    epg_build *b = epg_build_new(h, w, 2, now, "ru");
    epg_build_push(b, xml1, strlen(xml1));
    char err[100] = "";
    CHECK(!epg_build_finish(b, err, sizeof err));
    epg_build_free(b);
    epg_src s;
    memset(&s, 0, sizeof s);
    epg_open(&s, "http://test/epg");
    EQI(s.nch, 2);
    int ca = epg_find_id(&s, "a"), cb = epg_find_name(&s, b1);
    CHECK(ca >= 0 && cb >= 0);
    EQI(s.ch[ca].count, 2);
    EQS(epg_s(&s, s.pr[s.ch[ca].first].title), "Старая");
    EQI(s.ch[cb].count, 2);
    EQI(s.pr[s.ch[cb].first].stop, 1699993200);   /* B1 обрезана по B2 (20:20) */
    epg_close(&s);
    /* новая выгрузка без прошлого: прошлое канала a остаётся */
    const char *xml2 =
        "<tv><channel id=\"a\"><display-name>Первый</display-name></channel>"
        "<programme start=\"20231114200000\" stop=\"20231114210000\" channel=\"a\"><title>Вторая новая</title></programme>"
        "<programme start=\"20231114210000\" stop=\"20231114220000\" channel=\"a\"><title>Третья</title></programme></tv>";
    b = epg_build_new(h, w, 2, now, "ru");
    epg_build_push(b, xml2, strlen(xml2));
    CHECK(!epg_build_finish(b, err, sizeof err));
    epg_build_free(b);
    epg_open(&s, "http://test/epg");
    ca = epg_find_id(&s, "a");
    CHECK(ca >= 0);
    if (ca >= 0) {
        EQI(s.ch[ca].count, 3);
        EQS(epg_s(&s, s.pr[s.ch[ca].first].title), "Старая");
        EQS(epg_s(&s, s.pr[s.ch[ca].first + 1].title), "Вторая новая");
    }
    epg_close(&s);
    epg_remove_files(h);
    clock_fake(0);
}

static void test_lang(void) {
    lang_set(LANG_RU);
    EQS(trp(S_CHANNELS_N, 1), "1 канал");
    EQS(trp(S_CHANNELS_N, 3), "3 канала");
    EQS(trp(S_CHANNELS_N, 11), "11 каналов");
    EQS(trp(S_CHANNELS_N, 21), "21 канал");
    lang_set(LANG_EN);
    EQS(trp(S_CHANNELS_N, 1), "1 channel");
    EQS(trp(S_CHANNELS_N, 2), "2 channels");
    /* у каждой строки — оба языка и одинаковые подстановки */
    for (int i = 0; i < S_COUNT; i++) {
        lang_set(LANG_EN);
        const char *e = tr(i);
        lang_set(LANG_RU);
        const char *r = tr(i);
        CHECK(*e && *r);
        int pe = 0, pr = 0;
        for (const char *p = e; (p = strchr(p, '%')); p++) pe++;
        for (const char *p = r; (p = strchr(p, '%')); p++) pr++;
        int fe = 1, fr = 1;
        for (const char *p = e; (p = strchr(p, '|')); p++) fe++;
        for (const char *p = r; (p = strchr(p, '|')); p++) fr++;
        /* подстановок на одну форму — поровну */
        if (pe / fe != pr / fr) { fails++; fprintf(stderr, "строка %d: подстановки en «%s» / ru «%s»\n", i, e, r); }
    }
}

static void test_clock(void) {
    P.tz = 12;   /* +3 ч в четвертях */
    int64_t t = 1700000000;   /* 2023-11-14 22:13:20 UTC = 01:13 15.11 МСК */
    ltm m = local_tm(t);
    EQI(m.h, 1); EQI(m.m, 13); EQI(m.d, 15); EQI(m.mo, 11); EQI(m.y, 2023); EQI(m.wd, 2);
    EQI(day_start(t), 1699995600);
    char b[16];
    fmt_hm(t, b);
    EQS(b, "01:13");
    P.tz = TZ_AUTO;
}

int main(void) {
    char dir[] = "/tmp/ptv-test-XXXXXX";
    if (!mkdtemp(dir)) return 2;
    app_platform pl = {.data_dir = dir};
    pl.font_mem[0] = ptv_font_regular; pl.font_len[0] = (size_t)(ptv_font_regular_end - ptv_font_regular);
    pl.font_mem[1] = ptv_font_bold; pl.font_len[1] = (size_t)(ptv_font_bold_end - ptv_font_bold);
    if (app_init(&pl)) { fprintf(stderr, "app_init\n"); return 2; }
    P.tz = TZ_AUTO;
    test_m3u();
    test_names();
    test_catchup();
    test_xmltv();
    test_unpack();
    test_epg();
    test_lang();
    test_clock();
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd)) {}
    printf("%d проверок, ошибок: %d\n", checks, fails);
    return fails ? 1 : 0;
}
