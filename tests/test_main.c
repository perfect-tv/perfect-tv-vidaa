/* Юнит-тесты ядра на ПК: python3 tools/build.py --tests. Те же случаи, что в тестах Android- и веб-версии. */
#define _GNU_SOURCE
#include "app.h"
#include "core/clock.h"
#include "core/lang.h"
#include "core/prefs.h"
#include "core/unpack.h"
#include "core/phone.h"
#include "core/qr.h"
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
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

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
static int build_xml(uint32_t h, uint32_t old_gen, uint32_t gen, const char *url, const epg_want *w, int nw, int64_t now,
                     const char *xml, size_t len) {
    epg_build *b = epg_build_new(h, old_gen, gen, w, nw, now, "ru");
    if (!b) return -1;
    /* кусками по 7 байт: элементы на границах */
    for (size_t i = 0; i < len; i += 7) epg_build_push(b, xml + i, len - i < 7 ? len - i : 7);
    char err[100] = "";
    int rc = epg_build_finish(b, err, sizeof err);
    long stored = epg_build_stored(b);
    epg_build_free(b);
    if (rc) { fprintf(stderr, "build: %s\n", err); return rc; }
    epg_src m;
    memset(&m, 0, sizeof m);
    snprintf(m.url, sizeof m.url, "%s", url);
    m.hash = h;
    m.gen = gen;
    m.programmes = stored;
    epg_save_meta(&m);
    return 0;
}

static const char *title_of(epg_src *s, int slot, int i) {
    static epg_item it;
    epg_get(s, slot, i, &it);
    return it.title;
}

static int nscan;
static void on_scan(void *ctx, const char *id, const char *icon, const char *const *names, int n) {
    (void)ctx; (void)icon;
    if (!strcmp(id, "a")) { CHECK(n == 1); if (n) EQS(names[0], "Первый"); }
    nscan++;
}

static void test_epg(void) {
    int64_t now = 1700000000;
    clock_fake(now);
    char a1[200], b1[200];
    names_match_key("Первый", a1, sizeof a1);
    names_match_key("B", b1, sizeof b1);
    epg_want w[2] = {{"", a1, a1, 3}, {"", b1, b1, 0}};
    const char *xml1 =
        "<tv><channel id=\"a\"><display-name>Первый</display-name></channel><channel id=\"b\"><display-name>B</display-name></channel>"
        "<channel id=\"z\"><display-name>Не нужен</display-name></channel>"
        "<programme start=\"20231114190000\" stop=\"20231114200000\" channel=\"a\"><title>Старая</title><desc>Описание</desc></programme>"
        "<programme start=\"20231114200000\" stop=\"20231114210000\" channel=\"a\"><title>Вторая</title></programme>"
        "<programme start=\"20231114200000\" stop=\"20231114203000\" channel=\"b\"><title>B1</title></programme>"
        "<programme start=\"20231114202000\" stop=\"20231114210000\" channel=\"b\"><title>B2</title></programme>"
        "<programme start=\"20201114200000\" stop=\"20201114210000\" channel=\"a\"><title>Слишком старая</title></programme>"
        "<programme start=\"20231114200000\" stop=\"20231114210000\" channel=\"z\"><title>Чужая</title></programme>"
        /* канал a ещё раз, не подряд: собирается с прежними кусками */
        "<programme start=\"20231115230000\" stop=\"20231116010000\" channel=\"a\"><title>Завтра</title></programme>"
        "</tv>";
    const char *url = "http://test/epg";
    uint32_t h = hash_str(url);
    epg_remove_files(h);
    CHECK(!build_xml(h, 0, 1, url, w, 2, now, xml1, strlen(xml1)));
    epg_src s;
    memset(&s, 0, sizeof s);
    epg_open(&s, url);
    CHECK(epg_has_data(&s));
    EQI(s.gen, 1);
    EQI(s.programmes, 5);
    nscan = 0;
    epg_scan(&s, on_scan, NULL);
    EQI(nscan, 3);
    int sa = epg_slot_add(&s, "a", ""), sb = epg_slot_add(&s, "b", "http://i/b.png"), sz = epg_slot_add(&s, "z", "");
    EQI(epg_count(&s, sa), 3);
    EQS(title_of(&s, sa, 0), "Старая");
    epg_item it;
    epg_get(&s, sa, 0, &it);
    EQS(it.desc, "Описание");
    EQS(it.sub, "");
    EQS(title_of(&s, sa, 2), "Завтра");
    EQI(epg_count(&s, sb), 2);
    epg_get(&s, sb, 0, &it);
    EQI(it.stop, 1699993200);   /* B1 обрезана по B2 (20:20) */
    EQS(epg_slot_icon(&s, sb), "http://i/b.png");
    EQI(epg_count(&s, sz), 0);   /* не нужен плейлистам — не хранится */
    EQI(epg_count(&s, 99), 0);
    CHECK(epg_get(&s, sa, 3, &it) != 0);
    /* поиск по времени: первая с stop > t */
    EQI(epg_lower(&s, sa, 1699988400 - 1), 0);
    EQI(epg_lower(&s, sa, 1699992000), 1);        /* 20:00 — конец первой */
    EQI(epg_lower(&s, sa, 1699995000), 1);        /* 20:50 — идёт вторая */
    EQI(epg_lower(&s, sa, 1699995600), 2);
    EQI(epg_lower(&s, sa, 1800000000), 3);
    epg_close(&s);
    /* новая выгрузка без прошлого: прошлое канала a переносится, b пропал из XMLTV — его прошедшее тоже */
    const char *xml2 =
        "<tv><channel id=\"a\"><display-name>Первый</display-name></channel><channel id=\"b\"><display-name>B</display-name></channel>"
        "<programme start=\"20231114200000\" stop=\"20231114210000\" channel=\"a\"><title>Вторая новая</title></programme>"
        "<programme start=\"20231114210000\" stop=\"20231114220000\" channel=\"a\"><title>Третья</title></programme></tv>";
    CHECK(!build_xml(h, 1, 2, url, w, 2, now, xml2, strlen(xml2)));
    epg_open(&s, url);
    EQI(s.gen, 2);
    sa = epg_slot_add(&s, "a", "");
    sb = epg_slot_add(&s, "b", "");
    EQI(epg_count(&s, sa), 3);
    EQS(title_of(&s, sa, 0), "Старая");
    EQS(title_of(&s, sa, 1), "Вторая новая");
    EQS(title_of(&s, sa, 2), "Третья");
    EQI(epg_count(&s, sb), 2);   /* прошедшие B1, B2 */
    epg_close(&s);
    /* уборка: прежняя выгрузка, затем источник целиком */
    char p1[800], d[700];
    epg_dir(h, d, sizeof d);
    snprintf(p1, sizeof p1, "%s/d1.bin", d);
    CHECK(file_exists(p1));
    epg_prune(h, 2);
    CHECK(!file_exists(p1));
    epg_open(&s, url);
    CHECK(epg_has_data(&s));
    epg_close(&s);
    uint32_t keep[1] = {h};
    epg_gc(keep, 1);
    epg_open(&s, url);
    CHECK(epg_has_data(&s));
    epg_close(&s);
    epg_gc(keep, 0);
    epg_open(&s, url);
    CHECK(!epg_has_data(&s));
    epg_close(&s);
    /* хэш нужных каналов */
    CHECK(epg_want_hash(w, 2) != epg_want_hash(w, 1));
    epg_cache_free();
    clock_fake(0);
}

/* Синтетический XMLTV: nch каналов × days суток × 24 передачи, описание desc байт. Подаётся кусками. */
typedef struct { epg_build *b; sbuf x; long bytes; } gen_ctx;
static void gen_flush(gen_ctx *g, int force) {
    if (g->x.n >= (1 << 16) || (force && g->x.n)) { if (g->b) epg_build_push(g->b, g->x.s, g->x.n); g->bytes += (long)g->x.n; sb_clear(&g->x); }
}
static void xml_time(char *out, int64_t t) {
    time_t tt = (time_t)t;
    struct tm m;
    gmtime_r(&tt, &m);
    snprintf(out, 32, "%04d%02d%02d%02d%02d%02d +0000", m.tm_year + 1900, m.tm_mon + 1, m.tm_mday, m.tm_hour, m.tm_min, m.tm_sec);
}
static long gen_xml(epg_build *b, int nch, int days, int desc, int64_t now) {
    gen_ctx g = {b, {0}, 0};
    static const char *words[] = {"фильм", "новости", "погода", "сериал", "герой", "город", "история", "события", "семья",
                                  "детектив", "спорт", "матч", "ведущий", "гость", "студия", "выпуск", "жизнь", "тайна"};
    uint32_t rnd = 12345;
    sb_add(&g.x, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<tv>\n");
    for (int c = 0; c < nch; c++) {
        sb_printf(&g.x, "<channel id=\"ch%d\"><display-name>Канал %d</display-name><icon src=\"http://i/%d.png\"/></channel>\n", c, c, c);
        gen_flush(&g, 0);
    }
    int64_t t0 = (now / 86400 - (days - 8)) * 86400;
    for (int c = 0; c < nch; c++) {
        for (int k = 0; k < days * 24; k++) {
            char a[32], e[32];
            xml_time(a, t0 + k * 3600);
            xml_time(e, t0 + (k + 1) * 3600);
            sb_printf(&g.x, "<programme start=\"%s\" stop=\"%s\" channel=\"ch%d\"><title>Передача %d канала %d</title>", a, e, c, k, c);
            sb_add(&g.x, "<desc>");
            for (size_t n0 = g.x.n; g.x.n - n0 < (size_t)desc;) {
                rnd = rnd * 1103515245u + 12345u;
                sb_add(&g.x, words[(rnd >> 16) % COUNT(words)]);
                sb_printf(&g.x, " %u ", (rnd >> 8) % 100);
            }
            sb_add(&g.x, "</desc><category>Фильм</category></programme>\n");
            gen_flush(&g, 0);
        }
    }
    sb_add(&g.x, "</tv>\n");
    gen_flush(&g, 1);
    sb_free(&g.x);
    return g.bytes;
}

static long rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    char l[200];
    long v = -1;
    while (f && fgets(l, sizeof l, f)) if (!strncmp(l, "VmHWM:", 6)) v = atol(l + 6);
    if (f) fclose(f);
    return v;
}

static void big_epg(int nch, int days, int desc, int bench) {
    int64_t now = 1700000000;
    clock_fake(now);
    strpool pool = {0};
    epg_want *w = calloc((size_t)nch, sizeof *w);
    char k[100];
    for (int c = 0; c < nch; c++) {
        snprintf(k, sizeof k, "ch%d", c);
        w[c].tvg_id = sp_dup(&pool, k);
        w[c].tvg_name_key = w[c].name_key = "";
        w[c].archive_days = 7;
    }
    const char *url = "http://test/big";
    uint32_t h = hash_str(url);
    epg_remove_files(h);
    FILE *cr = fopen("/proc/self/clear_refs", "w");   /* сбросить пик памяти процесса */
    if (cr) { fputs("5", cr); fclose(cr); }
    long rss0 = rss_kb();
    double t = mono_now();
    epg_build *b = epg_build_new(h, 0, 1, w, nch, now, "ru");
    CHECK(b != NULL);
    if (!b) return;
    long xml = gen_xml(b, nch, days, desc, now);
    char err[100] = "";
    CHECK(!epg_build_finish(b, err, sizeof err));
    long stored = epg_build_stored(b);
    epg_build_free(b);
    double tb = mono_now() - t;
    long rss1 = rss_kb();
    epg_src m;
    memset(&m, 0, sizeof m);
    snprintf(m.url, sizeof m.url, "%s", url);
    m.hash = h;
    m.gen = 1;
    epg_save_meta(&m);
    /* окно: архив 7 + 1 сутки назад от начала суток … +8 суток */
    CHECK(stored > (long)nch * 24 * 15 && stored <= (long)nch * days * 24);
    epg_src s;
    memset(&s, 0, sizeof s);
    epg_open(&s, url);
    for (int c = 0; c < nch; c++) { snprintf(k, sizeof k, "ch%d", c); epg_slot_add(&s, k, ""); }
    /* все каналы по кадру, как листание гида: кэш не растёт */
    long total = 0, maxc = 0, bad = 0;
    t = mono_now();
    for (int c = 0; c < nch; c++) {
        epg_frame();
        int n = epg_count(&s, c);
        total += n;
        int i = epg_lower(&s, c, now);
        epg_item it;
        if (epg_get(&s, c, i, &it) || !(it.start <= now && it.stop > now)) bad++;
        snprintf(k, sizeof k, " канала %d", c);
        if (!strstr(it.title, k)) bad++;
        if (epg_cache_bytes() > maxc) maxc = epg_cache_bytes();
    }
    double tr_ = mono_now() - t;
    EQI(total, stored);
    EQI(bad, 0);
    CHECK(maxc < (8L << 20));
    char p[800], d[700];
    epg_dir(h, d, sizeof d);
    snprintf(p, sizeof p, "%s/d1.bin", d);
    long long dsz = file_size(p);
    if (bench)
        printf("epg bench: %d каналов, XML %.0f МБ, передач %ld, сборка %.1f с (%.0f МБ/с), данные %.1f МБ, "
               "пик памяти +%ld МБ, чтение %.2f мс/канал, кэш до %.1f МБ\n",
               nch, xml / 1048576.0, stored, tb, xml / 1048576.0 / tb, dsz / 1048576.0, (rss1 - rss0) / 1024, tr_ * 1000 / nch,
               maxc / 1048576.0);
    epg_close(&s);
    epg_cache_free();
    epg_remove_files(h);
    free(w);
    sp_free(&pool);
    clock_fake(0);
}

/* ---------- ввод с телефона ---------- */
/* запрос к странице: ответ целиком (сервер опрашивается, пока не ответит) */
static char *phone_req(int port, const char *req, phone_result *r, int *got) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa)) { close(fd); return NULL; }
    if (send(fd, req, strlen(req), 0) < 0) {}
    sbuf b = {0};
    for (int i = 0; i < 200; i++) {
        if (phone_poll(r)) *got = 1;
        char buf[4096];
        ssize_t n = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
        if (n > 0) { sb_addn(&b, buf, (size_t)n); continue; }
        if (n == 0) break;
        usleep(5000);
    }
    close(fd);
    return sb_take(&b);
}

static void test_phone(void) {
    char url[200], err[100];
    if (phone_start(url, sizeof url, err, sizeof err)) { printf("phone: нет сети (%s), пропуск\n", err); return; }
    CHECK(phone_running());
    const char *tok = strrchr(url, '/') + 1, *pc = strrchr(url, ':');
    int port = atoi(pc + 1);
    EQI(strlen(tok), 16);
    uint8_t m[QR_SIZE][QR_SIZE];
    EQI(qr_encode(url, m), 0);
    phone_result r;
    memset(&r, 0, sizeof r);
    int got = 0;
    char req[2000];
    char *resp = phone_req(port, "GET /0000000000000000 HTTP/1.1\r\nHost: x\r\n\r\n", &r, &got);
    CHECK(resp && strstr(resp, "404"));
    free(resp);
    snprintf(req, sizeof req, "GET /%s HTTP/1.1\r\nHost: x\r\n\r\n", tok);
    resp = phone_req(port, req, &r, &got);
    CHECK(resp && strstr(resp, "200 OK") && strstr(resp, "name=\"url\"") && strstr(resp, "Content-Security-Policy"));
    free(resp);
    const char *bad = "url=ftp%3A%2F%2Fx";
    snprintf(req, sizeof req, "POST /%s HTTP/1.1\r\nHost: x\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\n\r\n%s", tok, strlen(bad), bad);
    resp = phone_req(port, req, &r, &got);
    CHECK(!got && resp && strstr(resp, "https://"));
    free(resp);
    const char *body = "url=https%3A%2F%2Fexample.com%2Fpl.m3u%3Fa%3D1%26b%3D2&name=%D0%9C%D0%BE%D0%B9+IPTV&epg=http%3A%2F%2Fe.ru%2Fepg.xml.gz";
    snprintf(req, sizeof req, "POST /%s HTTP/1.1\r\nHost: x\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\n\r\n%s", tok, strlen(body), body);
    resp = phone_req(port, req, &r, &got);
    CHECK(got);
    free(resp);
    EQS(r.url, "https://example.com/pl.m3u?a=1&b=2");
    EQS(r.name, "Мой IPTV");
    EQS(r.epg, "http://e.ru/epg.xml.gz");
    CHECK(!phone_running());   /* одноразовая */
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
    big_epg(1000, 17, 60, 0);
    if (getenv("PTV_BENCH")) {
        double t = mono_now();
        long n = gen_xml(NULL, 2000, 17, 300, 1700000000);
        printf("epg bench: генератор XML %.0f МБ/с\n", n / 1048576.0 / (mono_now() - t));
        big_epg(10000, 17, 300, 1);
    }
    test_phone();
    test_lang();
    test_clock();
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd)) {}
    printf("%d проверок, ошибок: %d\n", checks, fails);
    return fails ? 1 : 0;
}
