/* Юнит-тесты ядра на ПК: python3 tools/build.py --tests. Те же случаи, что в тестах Android- и веб-версии. */
#define _GNU_SOURCE
#include "app.h"
#include "core/clock.h"
#include "core/lang.h"
#include "core/prefs.h"
#include "core/unpack.h"
#include "core/phone.h"
#include "core/qr.h"
#include "core/usb.h"
#include "core/relay.h"
#include "core/ttx.h"
#include "core/dvbsub.h"
#include "core/cc.h"
#include "core/cookies.h"
#include "core/substyle.h"
#include "core/webvtt.h"
#include "core/util.h"
#include "data/catchup.h"
#include "data/backup.h"
#include "data/tmdb.h"
#include "data/epg.h"
#include "data/repo.h"
#include "data/m3u.h"
#include "data/names.h"
#include "data/xmltv.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <zlib.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdatomic.h>
#include "core/http.h"
#include "core/logocache.h"
#include "img.h"

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

static void count_hit(void *ctx, int tag, const epg_item *it) {
    (void)it;
    int *c = ctx;
    c[tag == 17 ? 0 : 1]++;
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
    /* поиск: «канала 17» у ch17 за ±сутки — 49 передач (по часу; окно не по границе часа — с краёв по одной
       частично), у ch18 в его окне — ни одной */
    {
        char f[100];
        search_fold("Ёлка,  НОВОСТИ!  ", f, sizeof f);
        EQS(f, "елка новости");
        epg_sq q[2] = {{"ch17", now - 86400, now + 86400, 17}, {"ch18", now, now + 3600, 18}};
        const char *words[2] = {"канала", "17"};
        int cnt[2] = {0, 0};
        int found = epg_search(h, 1, q, 2, words, 2, 1000, count_hit, cnt, NULL);
        EQI(found, 49);
        EQI(cnt[0], 49);
        EQI(cnt[1], 0);
        EQI(epg_search(h, 1, q, 1, words, 2, 10, count_hit, cnt, NULL), 10);   /* предел находок */
    }
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
    if (phone_start(PHONE_PLAYLIST, url, sizeof url, err, sizeof err)) { printf("phone: нет сети (%s), пропуск\n", err); return; }
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

/* ---------- файлы с флешки ---------- */
static void test_usb(const char *tmp) {
    char d[600], p[700];
    snprintf(d, sizeof d, "%s/usb", tmp);
    mkdir(d, 0755);
    const char *files[] = {"b.M3U", "a.m3u8", "guide.xml.gz", "photo.jpg", ".hidden.m3u", "list.txt"};
    for (int i = 0; i < 6; i++) { snprintf(p, sizeof p, "%s/%s", d, files[i]); file_write_atomic(p, "x", 1); }
    snprintf(p, sizeof p, "%s/Папка", d);
    mkdir(p, 0755);
    usb_entry *e;
    int n = usb_list(d, &e);
    EQI(n, 5);
    if (n == 5) {
        EQS(e[0].name, "Папка"); EQI(e[0].dir, 1);
        EQS(e[1].name, "a.m3u8");
        EQS(e[2].name, "b.M3U");
        EQS(e[3].name, "guide.xml.gz");
        EQS(e[4].name, "list.txt");
    }
    free(e);
    CHECK(usb_is_epg("guide.xml.gz") && usb_is_epg("a.XMLTV") && !usb_is_epg("pl.m3u") && !usb_is_epg("x.gz"));
    CHECK(usb_list("/nonexistent-dir", &e) < 0);
    setenv("PTV_USB_ROOT", d, 1);
    usb_root r[4];
    CHECK(usb_roots(r, 4) >= 1 && !strcmp(r[0].path, d) && !strcmp(r[0].name, "usb"));
    unsetenv("PTV_USB_ROOT");
}

/* ---------- кэш логотипов: картинка с диска декодируется без сети ---------- */
static void test_logocache(void) {
    static const unsigned char png[] = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,6,0,0,0,31,21,196,137,
        0,0,0,13,73,68,65,84,120,218,99,252,207,192,80,15,0,4,133,1,128,132,169,140,33,0,0,0,0,73,69,78,68,174,66,96,130};
    char path[700];
    CHECK(logo_cache_path("http://logo.invalid/a.png", path, sizeof path) == 1);
    if (!path[0]) return;
    CHECK(!file_write_atomic(path, png, sizeof png));
    const gfx_image *im = logo_img("http://logo.invalid/a.png", 8, 8, IMG_FIT);
    CHECK(im && im->w == 8 && im->h == 8);
    logo_clear();
    CHECK(!file_exists(path));
}

/* ---------- напоминания: глубина архива хранится в самом напоминании ---------- */
static void test_reminders(void) {
    const char *rem = "[{\"key\":\"ch\",\"name\":\"Канал\",\"start\":100,\"stop\":200,\"playlist\":2,\"archive_days\":7,\"desc\":\"Описание\"}]";
    file_write_atomic(data_path("reminders.json"), rem, strlen(rem));
    repo_init();
    EQI(R.nrem, 1);
    if (R.nrem == 1) { EQI(R.rem[0].archive_days, 7); EQI(R.rem[0].playlist, 2); EQS(R.rem[0].key, "ch"); EQS(R.rem[0].desc, "Описание"); }
    repo_reminders_save();
    size_t n;
    char *t = file_read(data_path("reminders.json"), &n);
    CHECK(t && strstr(t, "\"archive_days\":7"));
    free(t);
}

/* ---------- резервная копия ---------- */
static void test_backup(void) {
    store_set("fav.1", "первый\nвторой");
    store_set("pin", "1234");
    const char *pl = "[{\"id\":1,\"name\":\"Мой\",\"url\":\"http://x/p.m3u\",\"epg_mode\":1,\"loaded_at\":1700000000,\"etag\":\"e1\",\"user_epg\":\"http://e/1.xml\"}]\n";
    file_write_atomic(data_path("playlists.json"), pl, strlen(pl));
    const char *rem = "[{\"key\":\"k\",\"start\":5}]";
    file_write_atomic(data_path("reminders.json"), rem, strlen(rem));
    char where[600] = "", err[100] = "";
    CHECK(!backup_save(where, sizeof where, err, sizeof err));
    CHECK(strstr(where, "/backups/backup-") && ends_with_ci(where, ".perfecttv"));
    int64_t created = 0;
    CHECK(!backup_check(where, &created) && created > 0);
    backup_item it[4];
    EQI(backup_list(it, 4), 1);
    /* поменяли всё, затем восстановили */
    store_set("fav.1", "другое");
    store_set("lockc.1", "канал");
    file_write_atomic(data_path("playlists.json"), "[]", 2);
    file_write_atomic(data_path("channels_1.tsv"), "x", 1);
    CHECK(!backup_restore(where, err, sizeof err));
    EQS(store_get("fav.1"), "первый\nвторой");
    EQS(store_get("pin"), "1234");
    CHECK(store_get("lockc.1") == NULL);
    CHECK(!file_exists(data_path("channels_1.tsv")));
    size_t n;
    char *t = file_read(data_path("playlists.json"), &n);
    CHECK(t && strstr(t, "\"name\":\"Мой\"") && strstr(t, "\"epg_mode\":1") && strstr(t, "http://e/1.xml") && !strstr(t, "loaded_at") && !strstr(t, "etag"));
    free(t);
    t = file_read(data_path("reminders.json"), &n);
    CHECK(t && strstr(t, "\"start\":5"));
    free(t);
    /* не копия */
    file_write_atomic(data_path("bad.perfecttv"), "{\"x\":1}", strlen("{\"x\":1}"));
    CHECK(backup_check(data_path("bad.perfecttv"), NULL) != 0);
    CHECK(backup_restore(data_path("bad.perfecttv"), err, sizeof err) != 0);
    EQS(err, "format");
    store_set("fav.1", NULL);
    store_set("pin", NULL);
}

/* ---------- TMDB (те же случаи, что TmdbTest Android-версии) ---------- */
static tmdb_hit th(const char *title, const char *date, const char *type, const char *orig, const char *backdrop) {
    tmdb_hit h;
    memset(&h, 0, sizeof h);
    snprintf(h.title, sizeof h.title, "%s", title);
    snprintf(h.date, sizeof h.date, "%s", date);
    snprintf(h.type, sizeof h.type, "%s", type);
    snprintf(h.original, sizeof h.original, "%s", orig);
    snprintf(h.backdrop, sizeof h.backdrop, "%s", backdrop);
    return h;
}
static void test_tmdb(void) {
    char q[300];
    tmdb_query("т/с Белые волки 2. 9 с.", q, sizeof q);
    CHECK(q[0] != 0);
    tmdb_query("Шерлок (2010) 16+", q, sizeof q);
    EQS(q, "шерлок");
    tmdb_query("«Брат 2». Фильм", q, sizeof q);
    EQS(q, "брат 2");
    tmdb_query("Тайны следствия. 5 сезон. 12 серия", q, sizeof q);
    EQS(q, "тайны следствия");
    tmdb_query("Склифосовский (с. 7)", q, sizeof q);
    EQS(q, "склифосовский");
    tmdb_query("Новости. Дайджест", q, sizeof q);
    EQS(q, "новости");
    tmdb_query("Мультфильмы. Сборник 3", q, sizeof q);
    EQS(q, "мультфильмы");
    tmdb_query("Ирония судьбы, или С лёгким паром!", q, sizeof q);
    EQS(q, "ирония судьбы, или с лёгким паром!");
    EQI(tmdb_year("Шерлок (2010)", ""), 2010);
    EQI(tmdb_year("Брат", "Россия, 1997 г. Режиссёр..."), 1997);
    EQI(tmdb_year("Новости", "Главные события дня"), 0);
    EQI(tmdb_year("Фильм", "Комедия 2003, США"), 2003);
    EQI(tmdb_year("Фильм", "Тел. 12003456"), 0);
    CHECK(tmdb_similarity("Ёлки!", "елки") > 0.999);
    CHECK(tmdb_similarity("Верить и ждать", "Верить и ждать 2") > 0.8);
    CHECK(tmdb_similarity("Новости", "Новый Человек-паук") < 0.72);
    tmdb_hit r[3] = {th("Брат 2", "2000-05-11", "movie", "", "/a.jpg"), th("Брат", "1997-12-12", "movie", "", "/b.jpg"), th("Брат", "", "person", "", "")};
    EQI(tmdb_pick(r, 3, "Брат", 1997), 1);
    EQI(tmdb_pick(r, 3, "Брат 2", 2000), 0);
    tmdb_hit np = th("Новый Человек-паук", "", "tv", "", "");
    EQI(tmdb_pick(&np, 1, "Новости", 0), -1);
    tmdb_hit today = th("Сегодня", "1952-01-14", "tv", "Today", "");
    tmdb_hit two[2] = {today, th("Сегодня вечером", "", "movie", "", "")};
    EQI(tmdb_pick(two, 2, "Сегодня", 0), -1);
    EQI(tmdb_pick(&today, 1, "Сегодня", 0), 0);
    tmdb_hit ev = th("Сегодня вечером", "", "movie", "", "");
    EQI(tmdb_pick(&ev, 1, "Сегодня", 0), -1);
    const char *js = "{\"page\":1,\"results\":[{\"media_type\":\"person\",\"name\":\"X\"},"
                     "{\"media_type\":\"tv\",\"name\":\"Шерлок\",\"original_name\":\"Sherlock\",\"first_air_date\":\"2010-07-25\","
                     "\"backdrop_path\":\"/b.jpg\",\"poster_path\":null,\"overview\":\"Детектив\",\"vote_average\":8.5}]}";
    tmdb_hit *h;
    int n = tmdb_parse(js, strlen(js), &h);
    EQI(n, 1);
    if (n == 1) {
        EQS(h[0].title, "Шерлок");
        EQS(h[0].original, "Sherlock");
        EQS(h[0].date, "2010-07-25");
        EQS(h[0].backdrop, "/b.jpg");
        EQS(h[0].poster, "");
        CHECK(h[0].vote > 8.4 && h[0].vote < 8.6);
        EQI(tmdb_pick(h, n, "шерлок", 2010), 0);
    }
    free(h);
}

/* ---------- телетекст через разбор TS ---------- */
static uint8_t ham(int n) {   /* Хэмминг 8/4, затем биты наоборот (как в PES) */
    for (int b = 0; b < 256; b++) if (ttx_ham84((uint8_t)b) == n) {
        int d1 = n & 1, d2 = (n >> 1) & 1, d3 = (n >> 2) & 1, d4 = (n >> 3) & 1;
        int p1 = 1 ^ d1 ^ d3 ^ d4, p2 = 1 ^ d1 ^ d2 ^ d4, p3 = 1 ^ d1 ^ d2 ^ d3;
        int low = p1 | (d1 << 1) | (p2 << 2) | (d2 << 3) | (p3 << 4) | (d3 << 5) | (d4 << 7), ones = 0;
        for (int k = 0; k < 8; k++) ones += (low >> k) & 1;
        return ttx_rev((uint8_t)(low | (((ones + 1) & 1) << 6)));
    }
    return 0;
}
/* блок данных телетекста: магазин, строка, 40 байт (для строки 0 первые 8 — Хэмминг) */
static void ttx_unit(sbuf *b, int mag, int row, const uint8_t *data) {
    uint8_t u[46] = {0x02, 44, 0x2C, 0xE4};
    int addr = mag | (row << 3);
    u[4] = ham(addr & 15);
    u[5] = ham(addr >> 4);
    for (int i = 0; i < 40; i++) u[6 + i] = ttx_rev(data[i]);
    sb_addn(b, (const char *)u, 46);
}
static void ttx_head(sbuf *b, int mag, int page, int flags_s4) {
    uint8_t d[40];
    memset(d, 0x20, 40);
    int nib[8] = {page & 15, (page >> 4) & 15, 0, 8 /* стирание */, 0, flags_s4, 0, 0};
    for (int i = 0; i < 8; i++) d[i] = 0;   /* заполняется ниже — уже закодированным */
    uint8_t row[40];
    for (int i = 0; i < 40; i++) row[i] = 0x20;
    memcpy(row + 8, "  NEWS  ", 8);
    /* ttx_unit переворачивает биты — подаём заранее «развёрнутые» значения Хэмминга */
    for (int i = 0; i < 8; i++) row[i] = ttx_rev(ham(nib[i]));
    ttx_unit(b, mag, 0, row);
}
static void ts_put(sbuf *ts, int pid, int pusi, const uint8_t *pl, size_t n) {
    size_t off = 0;
    int first = 1;
    static int cc;
    while (off < n || first) {
        uint8_t p[188];
        size_t room = 184, take = n - off < room ? n - off : room;
        p[0] = 0x47;
        p[1] = (uint8_t)((pusi && first ? 0x40 : 0) | (pid >> 8));
        p[2] = (uint8_t)pid;
        if (take < room) {   /* добивка полем адаптации */
            size_t af = room - take;
            p[3] = (uint8_t)(0x30 | (cc++ & 15));
            p[4] = (uint8_t)(af - 1);
            if (af > 1) { p[5] = 0; memset(p + 6, 0xFF, af - 2); }
            memcpy(p + 4 + af, pl + off, take);
        } else {
            p[3] = (uint8_t)(0x10 | (cc++ & 15));
            memcpy(p + 4, pl + off, take);
        }
        sb_addn(ts, (const char *)p, 188);
        off += take;
        first = 0;
    }
}
static void pes(sbuf *out, int sid, int64_t pts, const uint8_t *data, size_t n) {
    uint8_t h[14] = {0, 0, 1, (uint8_t)sid, 0, 0, 0x80, 0x80, 5};
    h[9] = (uint8_t)(0x21 | ((pts >> 29) & 0x0E));
    h[10] = (uint8_t)(pts >> 22);
    h[11] = (uint8_t)(0x01 | ((pts >> 14) & 0xFE));
    h[12] = (uint8_t)(pts >> 7);
    h[13] = (uint8_t)(0x01 | ((pts << 1) & 0xFE));
    size_t len = n + 8;
    h[4] = (uint8_t)(len >> 8);
    h[5] = (uint8_t)len;
    sb_addn(out, (const char *)h, 14);
    sb_addn(out, (const char *)data, n);
}
/* поддельный «провайдер» для ретранслятора: плейлист HLS и сегмент; keep-alive, после 3 запросов на соединении
 * молча закрывает его (как сервер по пределу или простою) */
static struct { int fd, port; sbuf seg; atomic_int conns, reqs; atomic_llong big_sent; } UP;
/* субтитры IMSC в fMP4: инициализация и сегмент (собирает mp4_build) */
static struct { sbuf init, seg; } IM;
static void box(sbuf *b, const char *type, const void *d, size_t n) {
    uint8_t h[8] = {(uint8_t)((n + 8) >> 24), (uint8_t)((n + 8) >> 16), (uint8_t)((n + 8) >> 8), (uint8_t)(n + 8)};
    memcpy(h + 4, type, 4);
    sb_addn(b, (const char *)h, 8);
    if (n) sb_addn(b, (const char *)d, n);
}
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
/* init: moov/trak/mdia (mdhd: шкала ts; minf/stbl/stsd: запись kind); сегмент: moof (tfhd, tfdt base, trun) + mdat */
static void mp4_build(sbuf *init, sbuf *segb, const char *kind, uint32_t ts, uint32_t base, const sbuf *samples, const uint32_t *durs, int ns) {
    uint8_t mdhd[24] = {0};
    put32(mdhd + 12, ts);
    sbuf entry = {0}, stsd = {0}, stbl = {0}, minf = {0}, mdia = {0}, trak = {0};
    uint8_t se[8] = {0};
    box(&entry, kind, se, sizeof se);
    uint8_t sh[8] = {0, 0, 0, 0, 0, 0, 0, 1};
    sb_addn(&stsd, (const char *)sh, 8);
    sb_addn(&stsd, entry.s, entry.n);
    box(&stbl, "stsd", stsd.s, stsd.n);
    box(&minf, "stbl", stbl.s, stbl.n);
    box(&mdia, "mdhd", mdhd, sizeof mdhd);
    box(&mdia, "minf", minf.s, minf.n);
    box(&trak, "mdia", mdia.s, mdia.n);
    sbuf moov = {0};
    box(&moov, "trak", trak.s, trak.n);
    sb_clear(init);
    box(init, "ftyp", "iso6\0\0\0\0", 8);
    box(init, "moov", moov.s, moov.n);
    sb_free(&entry); sb_free(&stsd); sb_free(&stbl); sb_free(&minf); sb_free(&mdia); sb_free(&trak); sb_free(&moov);
    /* moof: tfhd (default-base-is-moof), tfdt, trun (длительность и размер на образец, data_offset) */
    uint8_t tfhd[8] = {0, 2, 0, 0, 0, 0, 0, 1}, tfdt[8] = {0};
    put32(tfdt + 4, base);
    size_t trl = 12 + 8 * (size_t)ns;
    uint8_t *trun = calloc(1, trl);
    trun[1] = 0; trun[2] = 3; trun[3] = 1;   /* флаги 0x301 */
    put32(trun + 4, (uint32_t)ns);
    for (int i = 0; i < ns; i++) { put32(trun + 12 + 8 * i, durs[i]); put32(trun + 16 + 8 * i, (uint32_t)samples[i].n); }
    sbuf traf = {0}, moof = {0};
    box(&traf, "tfhd", tfhd, sizeof tfhd);
    box(&traf, "tfdt", tfdt, sizeof tfdt);
    size_t moof_len = 8 + 8 + traf.n + 8 + trl;
    put32(trun + 8, (uint32_t)(moof_len + 8));   /* данные — сразу за заголовком mdat */
    box(&traf, "trun", trun, trl);
    box(&moof, "traf", traf.s, traf.n);
    sb_clear(segb);
    box(segb, "moof", moof.s, moof.n);
    sbuf mdat = {0};
    for (int i = 0; i < ns; i++) sb_addn(&mdat, samples[i].s, samples[i].n);
    box(segb, "mdat", mdat.s, mdat.n);
    free(trun);
    sb_free(&traf); sb_free(&moof); sb_free(&mdat);
}

static void up_answer(int c, const char *req) {
    char head[400];
    if (strstr(req, "GET /live/pl.m3u8")) {
        const char *pl = "#EXTM3U\n#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"s\",NAME=\"Русские\",LANGUAGE=\"ru\",URI=\"subs/ru.m3u8\"\n"
                         "#EXT-X-STREAM-INF:BANDWIDTH=1,CODECS=\"avc1.64001f,mp4a.40.2,stpp.ttml.im1t\",SUBTITLES=\"s\"\nvar.m3u8\n"
                         "#EXT-X-TARGETDURATION:6\n#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n#EXTINF:6,\nseg1.ts?x=1\n";
        snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nSet-Cookie: sid=abc; Path=/live; HttpOnly\r\n"
                 "Set-Cookie: evil=1; Domain=provider.tv\r\nSet-Cookie: sec=1; Secure\r\n\r\n", strlen(pl));
        if (send(c, head, strlen(head), 0) < 0 || send(c, pl, strlen(pl), 0) < 0) {}
    } else if (strstr(req, "GET /live/seg1.ts?x=1") && strstr(req, "User-Agent: Test/1") && !strstr(req, "PerfectTV/") &&
               !strstr(req, "Accept-Encoding: gzip")) {
        const char *rg = strstr(req, "Range: bytes=");
        if (rg && strstr(req, "\r\nCookie: sid=abc\r\n")) {   /* только из хранилища: чужой домен, Secure и cookie плеера — нет */   /* часть — 206, как у настоящего сервера */
            size_t a0 = (size_t)atol(rg + 13), b0 = (size_t)atol(strchr(rg, '-') + 1);
            snprintf(head, sizeof head, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes %zu-%zu/%zu\r\nContent-Length: %zu\r\n\r\n",
                     a0, b0, UP.seg.n, b0 - a0 + 1);
            if (send(c, head, strlen(head), 0) < 0 || send(c, UP.seg.s + a0, b0 - a0 + 1, 0) < 0) {}
        } else {
            snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", UP.seg.n);
            if (send(c, head, strlen(head), 0) < 0 || send(c, UP.seg.s, UP.seg.n, 0) < 0) {}
        }
    } else if (strstr(req, "GET /live/big.ts")) {   /* 64 МБ: сколько провайдер успел отдать, пока плеер не читает */
        long long total = 64ll << 20;
        snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Length: %lld\r\n\r\n", total);
        if (send(c, head, strlen(head), 0) < 0) return;
        static char blk[65536];
        for (long long sent = 0; sent < total;) {
            ssize_t w = send(c, blk, sizeof blk, MSG_NOSIGNAL);
            if (w <= 0) break;
            sent += w;
            atomic_store(&UP.big_sent, sent);
        }
    } else if (strstr(req, "GET /live/subs/ru.m3u8") && strstr(req, "User-Agent: Test/1") && strstr(req, "\r\nCookie: sid=abc\r\n")) {
        const char *pl = "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXTINF:6,\nru0.vtt\n#EXT-X-ENDLIST\n";
        snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", strlen(pl));
        if (send(c, head, strlen(head), 0) < 0 || send(c, pl, strlen(pl), 0) < 0) {}
    } else if (strstr(req, "GET /live/subs/ru0.vtt")) {
        const char *v = "WEBVTT\nX-TIMESTAMP-MAP=MPEGTS:900000,LOCAL:00:00:00.000\n\n00:00:01.000 --> 00:00:03.000\nПривет из WebVTT\n";
        snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", strlen(v));
        if (send(c, head, strlen(head), 0) < 0 || send(c, v, strlen(v), 0) < 0) {}
    } else if (strstr(req, "GET /live/subs/im.m3u8")) {
        const char *pl = "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MAP:URI=\"im_init.mp4\"\n#EXTINF:6,\nim0.m4s\n#EXT-X-ENDLIST\n";
        snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", strlen(pl));
        if (send(c, head, strlen(head), 0) < 0 || send(c, pl, strlen(pl), 0) < 0) {}
    } else if (strstr(req, "GET /live/subs/im_init.mp4") || strstr(req, "GET /live/subs/im0.m4s")) {
        sbuf *b = strstr(req, "im_init") ? &IM.init : &IM.seg;
        snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", b->n);
        if (send(c, head, strlen(head), 0) < 0 || send(c, b->s, b->n, 0) < 0) {}
    } else if (strstr(req, "GET /live/chunked.ts")) {   /* длина неизвестна: куски */
        const char *h0 = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
        if (send(c, h0, strlen(h0), 0) < 0) {}
    } else {
        const char *nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        if (send(c, nf, strlen(nf), 0) < 0) {}
    }
}
static void *up_conn(void *a) {
    int c = (int)(intptr_t)a;
    char req[4096];
    for (int k = 0; k < 3; k++) {
        size_t n = 0;
        req[0] = 0;
        while (n < sizeof req - 1 && !strstr(req, "\r\n\r\n")) { ssize_t r = recv(c, req + n, sizeof req - 1 - n, 0); if (r <= 0) { close(c); return NULL; } n += (size_t)r; req[n] = 0; }
        atomic_fetch_add(&UP.reqs, 1);
        up_answer(c, req);
    }
    close(c);
    return NULL;
}
static void *upstream(void *a) {
    (void)a;
    for (;;) {
        int c = accept(UP.fd, NULL, NULL);
        if (c < 0) break;
        atomic_fetch_add(&UP.conns, 1);
        pthread_t t;
        if (!pthread_create(&t, NULL, up_conn, (void *)(intptr_t)c)) pthread_detach(t);
    }
    return NULL;
}
static char *fetch(const char *url, size_t *len);
/* потоки ретранслятора (имена ptv-relay / ptv-dns) */
static int threads_now(void) {
    int n = 0;
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[300], name[32] = "";
        snprintf(p, sizeof p, "/proc/self/task/%s/comm", e->d_name);
        FILE *f = fopen(p, "r");
        if (f) { if (!fgets(name, sizeof name, f)) name[0] = 0; fclose(f); }
        if (!strncmp(name, "ptv-relay", 9) || !strncmp(name, "ptv-dns", 7)) n++;
    }
    if (d) closedir(d);
    return n;
}
static int relay_sock(const char *rurl) {
    int port = atoi(strchr(rurl + 7, ':') + 1);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa)) { close(fd); return -1; }
    char q[9000];
    snprintf(q, sizeof q, "GET %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", strstr(rurl + 7, "/"));
    if (send(fd, q, strlen(q), 0) < 0) {}
    return fd;
}
/* 20 плееров сразу: все получают сегмент целиком, а потоков в процессе не прибавляется (один цикл событий) */
static void relay_many(const char *segurl) {
    int fds[20], th0 = threads_now(), thmax = th0;
    size_t got[20] = {0};
    for (int i = 0; i < 20; i++) fds[i] = relay_sock(segurl);
    double end = mono_now() + 5;
    int open_n = 20;
    while (open_n && mono_now() < end) {
        open_n = 0;
        for (int i = 0; i < 20; i++) {
            if (fds[i] < 0) continue;
            char buf[8192];
            ssize_t r = recv(fds[i], buf, sizeof buf, MSG_DONTWAIT);
            if (r > 0) got[i] += (size_t)r;
            if (r == 0) { close(fds[i]); fds[i] = -1; continue; }
            open_n++;
        }
        int t = threads_now();
        if (t > thmax) thmax = t;
        usleep(2000);
    }
    int full = 0;
    for (int i = 0; i < 20; i++) { if (got[i] > UP.seg.n) full++; if (fds[i] >= 0) close(fds[i]); }
    EQI(full, 20);
    EQI(thmax, 1);   /* один поток цикла; DNS 127.0.0.1 — из кэша, не поток на соединение */
    (void)th0;
}
/* плеер не читает 64 МБ — ретранслятор не выкачивает их у провайдера; другой плеер тем временем обслужен */
static void relay_backpressure(const char *bigurl, const char *segurl) {
    int slow = relay_sock(bigurl);
    usleep(800000);
    long long sent = atomic_load(&UP.big_sent);
    CHECK(sent > 0 && sent < (24ll << 20));   /* очередь ретранслятора (256 КБ) + буферы сокетов, а не всё */
    double t = mono_now();
    size_t n = 0;
    char *b = fetch(segurl, &n);
    CHECK(b && n == UP.seg.n && mono_now() - t < 2);
    free(b);
    close(slow);
}

/* несколько запросов в одном соединении с ретранслятором (keep-alive со стороны плеера) */
static int two_on_one(const char *relay_url_a, const char *relay_url_b) {
    const char *pa = strstr(relay_url_a + 7, "/"), *pb = strstr(relay_url_b + 7, "/");
    int port = atoi(strchr(relay_url_a + 7, ':') + 1);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa)) { close(fd); return -1; }
    char q[9000];
    snprintf(q, sizeof q, "GET %s HTTP/1.1\r\nHost: x\r\n\r\nGET %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", pa, pb);
    if (send(fd, q, strlen(q), 0) < 0) {}
    sbuf b = {0};
    char buf[8192];
    ssize_t r;
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while ((r = recv(fd, buf, sizeof buf, 0)) > 0) sb_addn(&b, buf, (size_t)r);
    close(fd);
    int n = 0;   /* в теле — двоичный TS с нулями: искать memmem, не strstr */
    for (const char *p = b.s, *e = b.s + b.n; p && p < e && (p = memmem(p, (size_t)(e - p), "HTTP/1.1 200", 12)); p++) n++;
    static const char want[] = "\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
    const char *hw = b.n ? memmem(b.s, b.n, want, sizeof want - 1) : NULL;
    int ok = n == 2 && hw;   /* второй ответ — кусками, как и пришёл от провайдера без длины */
    sb_free(&b);
    return ok;
}
static char resp_hdr[8192];
static char *fetch_h(const char *url, const char *headers, size_t *len) {
    char err[120] = "";
    hs_req rq = {.url = url, .headers = headers, .timeout_ms = 5000};
    hs *h = hs_open(&rq, err, sizeof err);
    if (!h) { fprintf(stderr, "fetch: %s\n", err); return NULL; }
    sbuf b = {0};
    char buf[8192];
    long r;
    snprintf(resp_hdr, sizeof resp_hdr, "%d\n%s", hs_status(h), hs_headers(h));
    while ((r = hs_read(h, buf, sizeof buf)) > 0) sb_addn(&b, buf, (size_t)r);
    hs_close(h);
    *len = b.n;
    return sb_take(&b);
}
static char *fetch(const char *url, size_t *len) { return fetch_h(url, NULL, len); }
static void test_relay_http(void) {
    UP.fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET};
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t sl = sizeof sa;
    if (bind(UP.fd, (struct sockaddr *)&sa, sizeof sa) || listen(UP.fd, 8) || getsockname(UP.fd, (struct sockaddr *)&sa, &sl)) { printf("relay: нет сокета, пропуск\n"); return; }
    UP.port = ntohs(sa.sin_port);
    pthread_t th;
    pthread_create(&th, NULL, upstream, NULL);
    atomic_store(&UP.conns, 0);
    atomic_store(&UP.reqs, 0);
    relay_reset();
    char src[200], url[4096];
    snprintf(src, sizeof src, "http://127.0.0.1:%d/live/pl.m3u8", UP.port);
    CHECK(!relay_url(src, "User-Agent: Test/1\r\n", url, sizeof url));
    CHECK(starts_with(url, "http://127.0.0.1:"));
    size_t n = 0;
    char *pl = fetch(url, &n);
    CHECK(pl && starts_with(pl, "#EXTM3U"));
    /* cookie плееру не уходят — их хранит ретранслятор */
    CHECK(!strstr(resp_hdr, "Set-Cookie"));
    EQI(cookies_count(), 1);
    /* сегмент и ключ — тоже через ретранслятор, адреса абсолютные, заголовки канала переданы */
    char *seg = pl ? strstr(pl, "#EXTINF:6,\n") : NULL;
    CHECK(pl && strstr(pl, "URI=\"http://127.0.0.1:") && strstr(pl, "key.bin"));
    char segurl[4096] = "";
    if (seg) { seg += strlen("#EXTINF:6,\n"); char *e = strchr(seg, '\n'); if (e) *e = 0; snprintf(segurl, sizeof segurl, "%s", seg); }
    CHECK(starts_with(segurl, "http://127.0.0.1:") && strstr(segurl, "seg1.ts%3Fx%3D1") && strstr(segurl, "&h="));
    free(pl);
    char *body = segurl[0] ? fetch(segurl, &n) : NULL;
    CHECK(body && n == UP.seg.n && !memcmp(body, UP.seg.s, n));   /* байты — как есть */
    free(body);
    /* Range плеера — провайдеру (его Cookie — нет, вместо них — из хранилища), 206 и Content-Range — обратно */
    body = segurl[0] ? fetch_h(segurl, "Range: bytes=188-375\r\nCookie: bogus=1\r\n", &n) : NULL;
    CHECK(body && n == 188 && !memcmp(body, UP.seg.s + 188, 188));
    CHECK(starts_with(resp_hdr, "206\n") && strstr(resp_hdr, "Content-Range: bytes 188-375/"));
    free(body);
    /* keep-alive к провайдеру: 3 запроса — одно соединение (дальше провайдер его закрыл — новое, без ошибки) */
    EQI(atomic_load(&UP.reqs), 3);
    EQI(atomic_load(&UP.conns), 1);
    body = segurl[0] ? fetch(segurl, &n) : NULL;
    CHECK(body && n == UP.seg.n);
    free(body);
    EQI(atomic_load(&UP.conns), 2);
    /* keep-alive к плееру: два запроса в одном соединении, второй — кусками (длина у провайдера неизвестна) */
    char churl[300], rch[4096];
    snprintf(churl, sizeof churl, "http://127.0.0.1:%d/live/chunked.ts", UP.port);
    CHECK(!relay_url(churl, "User-Agent: Test/1\r\n", rch, sizeof rch));
    CHECK(two_on_one(segurl, rch) == 1);
    /* субтитры WebVTT из главного плейлиста: ретранслятор запомнил, загрузчик забрал реплики */
    relay_vtt vt[4];
    EQI(relay_vtt_list(vt, 4), 1);
    if (relay_vtt_list(vt, 4) == 1) {
        EQS(vt[0].lang, "ru");
        EQS(vt[0].name, "Русские");
        char want[200];
        snprintf(want, sizeof want, "http://127.0.0.1:%d/live/subs/ru.m3u8", UP.port);
        EQS(vt[0].url, want);
        EQS(vt[0].codec, "stpp");   /* CODECS варианта с этой группой */
        webvtt_clock(900000, 0);
        webvtt_start(vt[0].url, vt[0].hdr);
        for (int i = 0; i < 100 && !webvtt_cues(); i++) usleep(20000);
        char txt[200];
        EQI(webvtt_cues(), 1);
        CHECK(!webvtt_text_at(900000 + 45000, txt, sizeof txt));      /* до 1 с — ничего */
        webvtt_text_at(900000 + 90000 * 2, txt, sizeof txt);
        EQS(txt, "Привет из WebVTT");
        webvtt_stop();
        EQI(webvtt_cues(), 0);
        /* IMSC в fMP4: инициализация из #EXT-X-MAP, время — по шкале потока */
        char imu[200];
        snprintf(imu, sizeof imu, "http://127.0.0.1:%d/live/subs/im.m3u8", UP.port);
        webvtt_clock(11 * 90000, 11);
        webvtt_start(imu, "User-Agent: Test/1\r\n");
        for (int i = 0; i < 100 && !webvtt_cues(); i++) usleep(20000);
        EQI(webvtt_cues(), 1);
        webvtt_text_at(12 * 90000, txt, sizeof txt);
        EQS(txt, "Привет из IMSC");
        webvtt_stop();
    }
    relay_many(segurl);
    char bigsrc[300], bigurl[4096];
    snprintf(bigsrc, sizeof bigsrc, "http://127.0.0.1:%d/live/big.ts", UP.port);
    CHECK(!relay_url(bigsrc, "User-Agent: Test/1\r\n", bigurl, sizeof bigurl));
    relay_backpressure(bigurl, segurl);
    CHECK(relay_has_teletext());   /* и попутно разобраны */
    CHECK(relay_ts_bytes() >= (long long)UP.seg.n);
    shutdown(UP.fd, SHUT_RDWR);
    close(UP.fd);
    pthread_join(th, NULL);
    usleep(100000);   /* потоки соединений провайдера дочитывают и закрываются */
}

/* ---------- ретранслятор: UDP / RTP и RTSP ---------- */
#include <openssl/evp.h>

/* метки оформления (core/substyle.h) — видимыми: {i} {b} {u} {iu} … {} — обычный, {#rrggbb} / {#} — цвет */
static void ss_show(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (const char *p = src; *p && o + 12 < cap;) {
        unsigned char c = (unsigned char)*p;
        if (c >= SS_SET && c <= SS_SET + 7) {
            int f = c - SS_SET;
            o += (size_t)snprintf(dst + o, cap - o, "{%s%s%s}", f & SS_I ? "i" : "", f & SS_B ? "b" : "", f & SS_U ? "u" : "");
            p++;
        } else if (c == SS_COLOR_DEF) { o += (size_t)snprintf(dst + o, cap - o, "{#}"); p++; }
        else if (c == SS_COLOR) { o += (size_t)snprintf(dst + o, cap - o, "{#%.6s}", p + 1); p += ss_mark_len(p); }
        else dst[o++] = *p++;
    }
    dst[o] = 0;
}
static struct { int port, rtp, multicast, stop; struct in_addr grp; atomic_int sent; } US;
/* отправитель: сегмент TS по 7 пакетов в датаграмме (с RTP или без), по кругу */
static void *udp_sender(void *a) {
    (void)a;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons((uint16_t)US.port), .sin_addr = US.grp};
    if (US.multicast) { int one = 1; setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &one, sizeof one); }
    uint8_t dg[12 + 7 * 188];
    uint16_t seq = 0;
    while (!US.stop) {
        for (size_t off = 0; off < UP.seg.n && !US.stop; off += 7 * 188) {
            size_t h = 0, k = UP.seg.n - off < 7 * 188 ? UP.seg.n - off : 7 * 188;
            if (US.rtp) { memset(dg, 0, 12); dg[0] = 0x80; dg[1] = 33; dg[2] = (uint8_t)(seq >> 8); dg[3] = (uint8_t)seq; seq++; h = 12; }
            memcpy(dg + h, UP.seg.s + off, k);
            if (sendto(fd, dg, h + k, 0, (struct sockaddr *)&to, sizeof to) > 0) atomic_fetch_add(&US.sent, 1);
            usleep(300);
        }
    }
    close(fd);
    return NULL;
}
/* ответ ретранслятора на запрос потока: заголовки и сколько байт тела пришло за время t */
static long long relay_read(const char *rurl, double t, char *head, size_t hcap) {
    int fd = relay_sock(rurl);
    if (fd < 0) return -1;
    struct timeval tv = {0, 200000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sbuf b = {0};
    double end = mono_now() + t;
    char buf[16384];
    while (mono_now() < end) {
        ssize_t r = recv(fd, buf, sizeof buf, 0);
        if (r == 0) break;
        if (r > 0 && b.n < (1u << 20)) sb_addn(&b, buf, (size_t)r);
    }
    close(fd);
    const char *e = b.n ? memmem(b.s, b.n, "\r\n\r\n", 4) : NULL;
    snprintf(head, hcap, "%.*s", e ? (int)(e - b.s) : (int)(b.n < hcap ? b.n : hcap - 1), b.s ? b.s : "");
    long long body = e ? (long long)(b.n - (size_t)(e + 4 - b.s)) : 0;
    sb_free(&b);
    return body;
}
static void udp_case(const char *url, int rtp, int multicast, const char *grp, int port) {
    relay_reset();
    US.port = port;
    US.rtp = rtp;
    US.multicast = multicast;
    US.stop = 0;
    atomic_store(&US.sent, 0);
    inet_pton(AF_INET, grp, &US.grp);
    char rurl[4096], head[2000];
    CHECK(!relay_url(url, NULL, rurl, sizeof rurl));
    pthread_t th;
    pthread_create(&th, NULL, udp_sender, NULL);
    long long body = relay_read(rurl, 1.5, head, sizeof head);
    US.stop = 1;
    pthread_join(th, NULL);
    printf("relay %s rtp=%d: датаграмм %d, тела %lld\n", url, rtp, atomic_load(&US.sent), body);
    CHECK(starts_with(head, "HTTP/1.1 200") && strstr(head, "video/mp2t") && strstr(head, "chunked"));
    CHECK(body > (long long)UP.seg.n);
    CHECK(relay_has_teletext());   /* TS разобран по дороге */
}

static void md5h(const char *s, char out[33]) {
    unsigned char d[EVP_MAX_MD_SIZE];
    unsigned l = 0;
    EVP_Digest(s, strlen(s), d, &l, EVP_md5(), NULL);
    for (int i = 0; i < 16; i++) sprintf(out + 2 * i, "%02x", d[i]);
}
static struct { int fd, port, mp2t; atomic_int teardown, play, auth_ok, keepalive; } RS;
static int rtsp_read_req(int c, char *req, size_t cap) {
    size_t n = 0;
    req[0] = 0;
    while (n < cap - 1 && !strstr(req, "\r\n\r\n")) { ssize_t r = recv(c, req + n, cap - 1 - n, 0); if (r <= 0) return -1; n += (size_t)r; req[n] = 0; }
    return 0;
}
static void *rtsp_server(void *a) {
    (void)a;
    for (;;) {
        int c = accept(RS.fd, NULL, NULL);
        if (c < 0) break;
        char req[8192], resp[4096];
        int playing = 0;
        uint16_t seq = 0;
        struct timeval tv = {0, 2000};
        for (;;) {
            if (playing) {   /* данные: RTP в $-кадрах канала 0, между ними — мусорный канал 1 */
                for (size_t off = 0; off < UP.seg.n; off += 7 * 188) {
                    size_t k = UP.seg.n - off < 7 * 188 ? UP.seg.n - off : 7 * 188;
                    uint8_t f[4 + 12 + 7 * 188];
                    f[0] = '$'; f[1] = 0; f[2] = (uint8_t)((12 + k) >> 8); f[3] = (uint8_t)((12 + k) & 255);
                    memset(f + 4, 0, 12); f[4] = 0x80; f[5] = 33; f[6] = (uint8_t)(seq >> 8); f[7] = (uint8_t)seq; seq++;
                    memcpy(f + 16, UP.seg.s + off, k);
                    if (send(c, f, 16 + k, MSG_NOSIGNAL) < 0) goto done;
                    uint8_t junk[] = {'$', 1, 0, 4, 1, 2, 3, 4};
                    if (send(c, junk, sizeof junk, MSG_NOSIGNAL) < 0) goto done;
                }
                setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                char tmp[4096];
                ssize_t r = recv(c, tmp, sizeof tmp - 1, MSG_DONTWAIT);
                if (r == 0) break;
                if (r > 0) { tmp[r] = 0; if (strstr(tmp, "TEARDOWN")) atomic_store(&RS.teardown, 1); if (strstr(tmp, "GET_PARAMETER")) atomic_fetch_add(&RS.keepalive, 1); }
                usleep(2000);
                continue;
            }
            if (rtsp_read_req(c, req, sizeof req)) break;
            int cseq = atoi(strstr(req, "CSeq:") ? strstr(req, "CSeq:") + 5 : "0");
            if (!strncmp(req, "DESCRIBE ", 9)) {
                const char *au = strstr(req, "Authorization: Digest ");
                char want[33] = "", h1[33], h2[33], all[300];
                md5h("user:Камера:p@ss", h1);
                snprintf(all, sizeof all, "DESCRIBE:rtsp://127.0.0.1:%d/live", RS.port);
                md5h(all, h2);
                snprintf(all, sizeof all, "%s:n0nce:%s", h1, h2);
                md5h(all, want);
                if (!au || !strstr(au, want) || !strstr(au, "username=\"user\"")) {
                    snprintf(resp, sizeof resp, "RTSP/1.0 401 Unauthorized\r\nCSeq: %d\r\nWWW-Authenticate: Basic realm=\"Камера\"\r\nWWW-Authenticate: Digest realm=\"Камера\", nonce=\"n0nce\"\r\n\r\n", cseq);
                } else {
                    atomic_store(&RS.auth_ok, 1);
                    const char *sdp = RS.mp2t ? "v=0\r\ns=x\r\na=control:*\r\nm=video 0 RTP/AVP 33\r\na=control:track1\r\n"
                                              : "v=0\r\ns=x\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=control:track1\r\n";
                    snprintf(resp, sizeof resp, "RTSP/1.0 200 OK\r\nCSeq: %d\r\nContent-Base: rtsp://127.0.0.1:%d/live/\r\nContent-Type: application/sdp\r\nContent-Length: %zu\r\n\r\n%s",
                             cseq, RS.port, strlen(sdp), sdp);
                }
            } else if (!strncmp(req, "SETUP ", 6)) {
                char want[200];
                snprintf(want, sizeof want, "SETUP rtsp://127.0.0.1:%d/live/track1 RTSP/1.0", RS.port);
                if (strncmp(req, want, strlen(want)) || !strstr(req, "interleaved=0-1")) snprintf(resp, sizeof resp, "RTSP/1.0 400 Bad\r\nCSeq: %d\r\n\r\n", cseq);
                else snprintf(resp, sizeof resp, "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: 4242;timeout=10\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n", cseq);
            } else if (!strncmp(req, "PLAY ", 5) && strstr(req, "Session: 4242")) {
                snprintf(resp, sizeof resp, "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: 4242\r\n\r\n", cseq);
                playing = 1;
                atomic_store(&RS.play, 1);
            } else snprintf(resp, sizeof resp, "RTSP/1.0 405 Method Not Allowed\r\nCSeq: %d\r\n\r\n", cseq);
            if (send(c, resp, strlen(resp), MSG_NOSIGNAL) < 0) break;
        }
    done:
        /* ретранслятор закрыл: дочитать, что он прислал напоследок (TEARDOWN) */
        for (int k = 0; k < 20; k++) {
            char tmp[4096];
            ssize_t r = recv(c, tmp, sizeof tmp - 1, MSG_DONTWAIT);
            if (r > 0) { tmp[r] = 0; if (strstr(tmp, "TEARDOWN")) atomic_store(&RS.teardown, 1); continue; }
            if (r == 0) break;
            usleep(5000);
        }
        close(c);
    }
    return NULL;
}

static void test_relay_streams(void) {
    char url[200];
    /* UDP на порт (не мультикаст), RTP; затем голый TS */
    snprintf(url, sizeof url, "udp://@127.0.0.1:%d", 47411);
    udp_case(url, 1, 0, "127.0.0.1", 47411);
    snprintf(url, sizeof url, "rtp://127.0.0.1:%d?pkt_size=1316", 47412);
    udp_case(url, 0, 0, "127.0.0.1", 47412);
    /* мультикаст: вход в группу; без сети с мультикастом (песочница) — пропуск */
    {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        struct ip_mreq m = {.imr_interface = {htonl(INADDR_ANY)}};
        inet_pton(AF_INET, "239.255.71.17", &m.imr_multiaddr);
        int ok = !setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m);
        close(fd);
        if (ok) udp_case("udp://@239.255.71.17:47413", 1, 1, "239.255.71.17", 47413);
        else printf("relay: мультикаст недоступен, пропуск\n");
    }
    CHECK(relay_url("rtmp://x/live", NULL, url, sizeof url) != 0);
    /* SDP: медиа с MPEG-TS, адреса относительно Content-Base */
    char media[300], aggr[300];
    CHECK(!relay_sdp_pick("v=0\r\na=control:*\r\nm=audio 0 RTP/AVP 0\r\na=control:a\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 MP2T/90000\r\na=control:v\r\n",
                          "rtsp://h/x/", media, aggr, sizeof media));
    EQS(media, "rtsp://h/x/v");
    EQS(aggr, "rtsp://h/x/");
    CHECK(!relay_sdp_pick("m=video 0 RTP/AVP 33\r\na=control:rtsp://o/t1", "rtsp://h/x", media, aggr, sizeof media));
    EQS(media, "rtsp://o/t1");
    CHECK(relay_sdp_pick("m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n", "rtsp://h/", media, aggr, sizeof media) != 0);
    /* RTSP: Digest (имя и пароль из адреса), SETUP через TCP, данные, поддержка сеанса, TEARDOWN */
    RS.fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET};
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t sl = sizeof sa;
    if (bind(RS.fd, (struct sockaddr *)&sa, sizeof sa) || listen(RS.fd, 4) || getsockname(RS.fd, (struct sockaddr *)&sa, &sl)) { printf("rtsp: нет сокета, пропуск\n"); return; }
    RS.port = ntohs(sa.sin_port);
    RS.mp2t = 1;
    pthread_t th;
    pthread_create(&th, NULL, rtsp_server, NULL);
    relay_reset();
    char src[300], rurl[4096], head[2000];
    snprintf(src, sizeof src, "rtsp://user:p%%40ss@127.0.0.1:%d/live", RS.port);
    CHECK(!relay_url(src, "User-Agent: Test/1\r\n", rurl, sizeof rurl));
    long long body = relay_read(rurl, 7.0, head, sizeof head);
    printf("relay rtsp: тела %lld\n", body);
    CHECK(atomic_load(&RS.auth_ok) && atomic_load(&RS.play));
    CHECK(starts_with(head, "HTTP/1.1 200") && strstr(head, "video/mp2t"));
    CHECK(body > (long long)UP.seg.n);
    CHECK(relay_has_teletext());
    CHECK(atomic_load(&RS.keepalive) >= 1);   /* таймаут 10 с — раз в 5 с */
    for (int i = 0; i < 50 && !atomic_load(&RS.teardown); i++) usleep(20000);
    CHECK(atomic_load(&RS.teardown));
    /* без MPEG-TS — отказ (tv.c откроет канал напрямую) */
    RS.mp2t = 0;
    atomic_store(&RS.play, 0);
    relay_reset();
    CHECK(!relay_url(src, NULL, rurl, sizeof rurl));
    relay_read(rurl, 1.0, head, sizeof head);
    CHECK(starts_with(head, "HTTP/1.1 415") && !atomic_load(&RS.play));
    shutdown(RS.fd, SHUT_RDWR);
    close(RS.fd);
    pthread_join(th, NULL);
}

static void test_teletext(void) {
    relay_reset();
    sbuf ts = {0};
    /* PAT: программа 1 -> PMT 0x100 */
    uint8_t pat[] = {0, 0x00, 0xB0, 13, 0, 1, 0xC1, 0, 0, 0, 1, 0xE1, 0x00, 0, 0, 0, 0};
    ts_put(&ts, 0, 1, pat, sizeof pat);
    /* PMT: видео 0x102, телетекст 0x101 (rus, субтитры на 888) */
    uint8_t pmt[] = {0, 0x02, 0xB0, 30, 0, 1, 0xC1, 0, 0, 0xE1, 0x02, 0xF0, 0,
                     0x1B, 0xE1, 0x02, 0xF0, 0,
                     0x06, 0xE1, 0x01, 0xF0, 7, 0x56, 5, 'r', 'u', 's', (2 << 3) | 0, 0x88,
                     0, 0, 0, 0};
    ts_put(&ts, 0x100, 1, pmt, sizeof pmt);
    uint8_t es[4] = {0, 0, 1, 0xB3};
    sbuf v = {0};
    pes(&v, 0xE0, 900000, es, 4);
    ts_put(&ts, 0x102, 1, (const uint8_t *)v.s, v.n);
    /* страница 888 (субтитры, флаг C6): строка 1 — «Привет» в окне */
    sbuf t = {0};
    sb_addc(&t, 0x10);   /* data_identifier */
    ttx_head(&t, 0, 0x88, 8);
    uint8_t row[40];
    memset(row, 0x20, 40);
    const uint8_t text[] = {0x0B, 0x0B, 0x50, 0x72, 0x69, 0x77, 0x65, 0x74, 0x0A, 0x0A};
    memcpy(row + 5, text, sizeof text);
    ttx_unit(&t, 0, 1, row);
    ttx_head(&t, 0, 0xFF, 0);   /* заполнитель: прежняя страница закончена */
    sbuf tp = {0};
    pes(&tp, 0xBD, 900000 + 180000, (const uint8_t *)t.s, t.n);
    ts_put(&ts, 0x101, 1, (const uint8_t *)tp.s, tp.n);
    ts_put(&ts, 0x101, 1, (const uint8_t *)tp.s, tp.n);   /* следующий PES завершает предыдущий */
    if (getenv("PTV_DUMP_TS")) {   /* для проверки окна на ПК: ещё страница 100 — цвета, двойная высота, мозаика */
        sbuf t2 = {0}, tp2 = {0}, all = {0};
        sb_addc(&t2, 0x10);
        ttx_head(&t2, 1, 0x00, 0);
        uint8_t r1[40], r3[40], r5[40], r7[40], r9[40];
        memset(r1, 0x20, 40); memset(r3, 0x20, 40); memset(r5, 0x20, 40); memset(r7, 0x20, 40); memset(r9, 0x20, 40);
        const uint8_t t1[] = {0x0D, 0x03, 0x54, 0x45, 0x4C, 0x45, 0x54, 0x45, 0x4B, 0x53, 0x54};   /* ТЕЛЕТЕКСТ, двойная высота */
        memcpy(r1 + 2, t1, sizeof t1);
        const uint8_t t3[] = {0x06, 0x6E, 0x6F, 0x77, 0x6F, 0x73, 0x74, 0x69, 0x20, 0x31, 0x30, 0x31};   /* новости 101 */
        memcpy(r3 + 2, t3, sizeof t3);
        r5[1] = 0x11; for (int i = 2; i < 20; i++) r5[i] = (uint8_t)(0x20 | (i * 7 & 0x5F));
        r5[20] = 0x1A; r5[21] = 0x12; for (int i = 22; i < 38; i++) r5[i] = 0x7F;
        const uint8_t t7[] = {0x04, 0x1D, 0x07, 0x50, 0x6F, 0x67, 0x6F, 0x64, 0x61, 0x20, 0x20, 0x31, 0x30, 0x32};   /* синий фон: Погода 102 */
        memcpy(r7 + 1, t7, sizeof t7);
        const uint8_t t9[] = {0x01, 0x6B, 0x75, 0x72, 0x73, 0x79, 0x20, 0x77, 0x61, 0x6C, 0x60, 0x74, 0x20, 0x31, 0x30, 0x33};   /* курсы валют 103 */
        memcpy(r9 + 2, t9, sizeof t9);
        ttx_unit(&t2, 1, 1, r1); ttx_unit(&t2, 1, 3, r3); ttx_unit(&t2, 1, 5, r5); ttx_unit(&t2, 1, 7, r7); ttx_unit(&t2, 1, 9, r9);
        ttx_head(&t2, 1, 0xFF, 0);
        pes(&tp2, 0xBD, 900000 + 90000, (const uint8_t *)t2.s, t2.n);
        sb_addn(&all, ts.s, ts.n);
        ts_put(&all, 0x101, 1, (const uint8_t *)tp2.s, tp2.n);
        ts_put(&all, 0x101, 1, (const uint8_t *)tp2.s, tp2.n);
        file_write_atomic(getenv("PTV_DUMP_TS"), all.s, all.n);
        sb_free(&t2); sb_free(&tp2); sb_free(&all);
    }
    for (size_t i = 0; i < ts.n; i += 77) relay_tap((const uint8_t *)ts.s + i, ts.n - i < 77 ? ts.n - i : 77);
    CHECK(relay_has_teletext());
    EQI(relay_base_pts(), 900000);
    ttx *tx = relay_ttx();
    ttx_page pg;
    int nsub = 0;
    CHECK(!ttx_get(tx, 0x888, -1, &pg, &nsub));
    EQI(nsub, 1);
    CHECK(pg.flags & TTXF_SUBTITLE);
    CHECK(ttx_has_subtitles(tx));
    CHECK(ttx_subtitle_at(tx, 900000, &pg) != 0);   /* раньше субтитра — ничего */
    CHECK(!ttx_subtitle_at(tx, 900000 + 200000, &pg));
    ttx_grid g;
    ttx_layout(&pg, NULL, &g);
    char out[400];
    ttx_subtitle_text(&g, out, sizeof out);
    EQS(out, "Привет");
    uint8_t h0[40];
    ttx_header_row(0x888, &pg, "", h0);
    CHECK(!memcmp(h0 + 1, "P888", 4) && !memcmp(h0 + 8, "  NEWS", 6));
    ttx_header_row(0x888, &pg, "12", h0);
    CHECK(!memcmp(h0 + 1, "P12-", 4));
    EQI(ttx_neighbour(tx, 0x100, 1), 0x888);
    uint16_t nums[8];
    EQI(ttx_numbers(tx, nums, 8), 1);
    EQI(nums[0], 0x888);
    EQI(ttx_subtitle_number(tx), 0x888);
    CHECK(ttx_subtitles_known(tx));
    /* двойная ширина (0x0E): знак на две ячейки, у правой своего нет; мигание (0x08) — со следующей ячейки */
    {
        ttx_page dp;
        memset(&dp, 0, sizeof dp);
        dp.has[2] = 1;
        memset(dp.rows[2], 0x20, 40);
        const uint8_t r2[] = {0x0E, 'A', 'B', 0x0C, 0x08, 'C'};
        memcpy(dp.rows[2], r2, sizeof r2);
        ttx_grid dg;
        ttx_layout(&dp, NULL, &dg);
        CHECK((dg.attr[80 + 1] & TA_DW) && (dg.attr[80 + 2] & TA_DW_RIGHT) && !(dg.attr[80 + 5] & TA_DW));
        CHECK(!(dg.attr[80 + 4] & TA_FLASH) && (dg.attr[80 + 5] & TA_FLASH));
    }
    EQI(ttx_glyph(0x41, 0x24), 0x410);   /* «А» */
    EQI(ttx_glyph(0x23, 0), 0xA3);       /* «£» в английском */
    relay_reset();
    CHECK(!relay_has_teletext() && ttx_count(relay_ttx()) == 0);
    sb_free(&UP.seg);
    sb_addn(&UP.seg, ts.s, ts.n);
    test_relay_http();
    test_relay_streams();
    sb_free(&UP.seg);
    sb_free(&ts); sb_free(&v); sb_free(&t); sb_free(&tp);
}

/* ---------- DVB-субтитры: набор сегментов вручную ---------- */
static void seg(sbuf *b, int type, int page, const uint8_t *d, int n) {
    uint8_t h[6] = {0x0F, (uint8_t)type, (uint8_t)(page >> 8), (uint8_t)page, (uint8_t)(n >> 8), (uint8_t)n};
    sb_addn(b, (const char *)h, 6);
    sb_addn(b, (const char *)d, (size_t)n);
}
static void test_dvbsub(void) {
    dvbsub *d = dvbsub_new(1, 0);
    sbuf b = {0};
    sb_addn(&b, "\x20\x00", 2);
    const uint8_t pcs[] = {5, (0 << 4) | (2 << 2), 0, 0, 0, 10, 0, 20};   /* 5 с, смена режима, область 0 в (10, 20) */
    seg(&b, 0x10, 1, pcs, sizeof pcs);
    const uint8_t rcs[] = {0, 0x08, 0, 8, 0, 3, (2 << 5) | (2 << 2), 0, 0, 0,   /* 8×3, глубина 4 бита, палитра 0, заливка 0 */
                           0, 1, 0x00, 0, 0, 0};                                  /* объект 1 в (0, 0) */
    seg(&b, 0x11, 1, rcs, sizeof rcs);
    const uint8_t clut[] = {0, 0, 1, 0x40 | 1, 235, 128, 128, 0};   /* код 1 (4 бита) — белый, непрозрачный */
    seg(&b, 0x12, 1, clut, sizeof clut);
    /* объект: строка «1 1 1 1» (4 бита), конец строки; нижнее поле — как верхнее */
    const uint8_t ods[] = {0, 1, 0x00, 0, 5, 0, 0, 0x11, 0x11, 0x11, 0x00, 0xF0};
    seg(&b, 0x13, 1, ods, sizeof ods);
    seg(&b, 0x80, 1, NULL, 0);
    sb_addc(&b, (char)0xFF);
    dvbsub_feed(d, (const uint8_t *)b.s, b.n, 900000);
    EQI(dvbsub_count(d), 1);
    dvbsub_show s;
    memset(&s, 0, sizeof s);
    CHECK(dvbsub_at(d, 899999, 0, &s) == -1);   /* раньше показа — ничего */
    EQI(dvbsub_at(d, 900000 + 90000, 0, &s), 1);
    EQI(s.nreg, 1);
    if (s.nreg == 1) {
        EQI(s.reg[0].x, 10); EQI(s.reg[0].y, 20); EQI(s.reg[0].w, 8); EQI(s.reg[0].h, 3);
        EQI(s.reg[0].px[0], 0xffffffff);          /* строка 0 — верхнее поле */
        EQI(s.reg[0].px[8 + 3], 0xffffffff);      /* строка 1 — нижнее (копия) */
        EQI(s.reg[0].px[4], 0);                   /* дальше — заливка 0, прозрачно */
    }
    CHECK(dvbsub_at(d, 900000 + 90000, s.id, &s) == 0);   /* тот же показ — без копии */
    CHECK(dvbsub_at(d, 900000 + 5 * 90000, 0, &s) == -1);  /* page_time_out истёк */
    dvbsub_show_clear(&s);
    sb_free(&b);
    dvbsub_free(d);
}

/* ---------- DVB-субтитры на записанном потоке (PTV_DVBSUB_SAMPLE=файл.ts; без него — пропуск) ---------- */
/* настоящая запись с закрытыми субтитрами (PTV_CC_SAMPLE=файл.ts, PTV_CC_NUM=1…4 или S1…): каждая смена показа —
 * время от начала и строки */
static void cc_sample(void) {
    const char *path = getenv("PTV_CC_SAMPLE");
    if (!path) return;
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cc: нет файла\n"); return; }
    relay_reset();
    const char *num = getenv("PTV_CC_NUM");   /* 1…4 — CC1…CC4, S1… — сервис 708 */
    if (num && *num == 'S') relay_cc_select(CC_708, atoi(num + 1));
    else relay_cc_select(CC_608, num ? atoi(num) : 1);
    static uint8_t buf[188 * 512];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) relay_tap(buf, n);
    fclose(f);
    relay_cc cl[8];
    int ncl = relay_cc_list(cl, 8);
    printf("cc: каналов %d:", ncl);
    for (int i = 0; i < ncl; i++) printf(" %s%d(%s)", cl[i].kind == CC_608 ? "CC" : "S", cl[i].num, cl[i].lang);
    printf("\n");
    cc_show sh, prev;
    memset(&prev, 0, sizeof prev);
    prev.pts = -1;
    int64_t base = relay_base_pts();
    for (int64_t t = base; t < base + 90000LL * 600; t += 3003) {
        int r = relay_cc_at(t, &sh);
        if (!r) sh.n = 0;
        if (sh.n == prev.n && (!sh.n || sh.pts == prev.pts)) continue;
        printf("cc %7.3f с:", (t - base) / 90000.0);
        for (int i = 0; i < sh.n; i++) { char v[600]; ss_show(v, sizeof v, sh.row[i].text); printf(" [стр %2d кол %2d] «%s»", (int)(sh.row[i].y * 15 + 0.5) + 1, (int)(sh.row[i].x * 32 + 0.5), v); }
        printf("\n");
        prev = sh;
        if (!sh.n) prev.pts = -1;
    }
    relay_reset();
}

static void dvbsub_sample(void) {
    const char *path = getenv("PTV_DVBSUB_SAMPLE"), *out = getenv("PTV_DVBSUB_OUT");
    if (!path) return;
    FILE *f = fopen(path, "rb");
    if (!f) { printf("dvbsub: нет файла\n"); return; }
    relay_reset();
    static uint8_t buf[188 * 512];
    size_t n;
    int selected = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        relay_tap(buf, n);
        relay_dvb tr[8];
        if (!selected && relay_dvb_list(tr, 8) > 0) { relay_dvb_select(0); selected = 1; }
    }
    fclose(f);
    dvbsub *d = relay_dvbsub();
    int shows = d ? dvbsub_count(d) : 0;
    printf("dvbsub: показов %d\n", shows);
    CHECK(shows > 0);
    if (!d || !out) return;
    /* несколько показов — картинками (подложка серая, как у ffmpeg-сравнения) */
    int written = 0, have = 0;
    dvbsub_show s;
    memset(&s, 0, sizeof s);
    for (int64_t pts = relay_base_pts(); written < 6 && pts < relay_base_pts() + 90000LL * 600; pts += 90000) {
        if (dvbsub_at(d, pts, have, &s) != 1) continue;
        have = s.id;
        char fn[600];
        snprintf(fn, sizeof fn, "%s/dvb%d.ppm", out, written++);
        FILE *o = fopen(fn, "wb");
        fprintf(o, "P6\n%d %d\n255\n", s.dw, s.dh);
        for (int y = 0; y < s.dh; y++)
            for (int x = 0; x < s.dw; x++) {
                uint32_t c = 0;
                for (int r = 0; r < s.nreg; r++) {
                    dvbsub_region *g = &s.reg[r];
                    if (x >= g->x && x < g->x + g->w && y >= g->y && y < g->y + g->h) c = g->px[(y - g->y) * g->w + (x - g->x)];
                }
                unsigned a = c >> 24, bgc = 96;
                unsigned char px[3] = {(unsigned char)((((c >> 16) & 255) * a + bgc * (255 - a)) / 255), (unsigned char)((((c >> 8) & 255) * a + bgc * (255 - a)) / 255), (unsigned char)(((c & 255) * a + bgc * (255 - a)) / 255)};
                fwrite(px, 1, 3, o);
            }
        fclose(o);
        printf("dvbsub: %s pts=+%.1f с, областей %d\n", fn, (s.pts - relay_base_pts()) / 90000.0, s.nreg);
    }
    dvbsub_show_clear(&s);
}

/* ---------- WebVTT ---------- */
static int64_t vt_s[8], vt_e[8];
static char vt_t[8][200];
static int vt_n;
static void vt_cb(void *ctx, int64_t s, int64_t e, const char *t) {
    (void)ctx;
    if (vt_n < 8) { vt_s[vt_n] = s; vt_e[vt_n] = e; ss_show(vt_t[vt_n], 200, t); vt_n++; }
}
static void test_webvtt(void) {
    const char *v = "WEBVTT\r\nX-TIMESTAMP-MAP=LOCAL:00:00:10.000,MPEGTS:1800000\r\n\r\n"
                    "NOTE это комментарий\r\n\r\n"
                    "1\r\n00:00:11.500 --> 00:00:13.000 align:center\r\n<i>Первая</i> строка\r\nвторая &amp; конец\r\n\r\n"
                    "intro\r\n00:12.000 --> 00:14.250\r\n<v Иван>Реплика</v>\r\n";
    vt_n = 0;
    EQI(webvtt_parse(v, strlen(v), vt_cb, NULL), 2);
    EQI(vt_n, 2);
    EQI(vt_s[0], 1800000 + 135000);   /* MPEGTS + (11,5 − 10) × 90000 */
    EQI(vt_e[0], 1800000 + 270000);
    EQS(vt_t[0], "{i}Первая {}строка\nвторая & конец");
    EQI(vt_s[1], 1800000 + 180000);   /* «мм:сс.ттт» без часов */
    EQI(vt_e[1], 1800000 + 382500);
    EQS(vt_t[1], "Реплика");
    vt_n = 0;
    EQI(webvtt_parse("WEBVTT\n\n00:00:01.000 --> 00:00:02.000\nбез карты\n", 46, vt_cb, NULL), 1);
    EQI(vt_s[0], 90000);
    /* оформление: вложенность, переносы строк внутри тега (на новой строке — заново), цвет класса, мусорные байты */
    const char *st = "WEBVTT\n\n00:01.000 --> 00:02.000\n<i>курсив <b>жирный</b>\nещё</i> <c.yellow.bg_black>жёлтый</c> <u>под черта</u>\x02\n\n"
                     "00:03.000 --> 00:04.000\n<c.unknown><i></i></c>\n";
    vt_n = 0;
    EQI(webvtt_parse(st, strlen(st), vt_cb, NULL), 1);   /* вторая — без видимого текста */
    EQS(vt_t[0], "{i}курсив {ib}жирный\n{i}ещё {}{#ffff00}жёлтый {u}{#}под черта");
}

/* ---------- закрытые субтитры CEA-608 / 708 ---------- */
static uint8_t par(uint8_t b) { int n = 0; for (int i = 0; i < 7; i++) n += (b >> i) & 1; return (uint8_t)(b | (n % 2 ? 0 : 0x80)); }   /* нечётная чётность */
/* кадр: одна пара 608 (поле f) и пустой 708 */
static int64_t ccpts;
static void f608(ccdec *d, int f, uint8_t b1, uint8_t b2) {
    uint8_t t[3] = {(uint8_t)(0xFC | f), par(b1), par(b2)};
    cc_feed(d, ccpts, t, 1);
    ccpts += 3003;
}
static void ctl2(ccdec *d, uint8_t b1, uint8_t b2) { f608(d, 0, b1, b2); f608(d, 0, b1, b2); }   /* команды — дважды */
static void txt608(ccdec *d, const char *s) { for (; *s; s += s[1] ? 2 : 1) f608(d, 0, (uint8_t)s[0], s[1] ? (uint8_t)s[1] : 0); }

static void test_cc(void) {
    ccdec *d = cc_new();
    cc_show sh;
    /* pop-on: текст копится в невидимой памяти, EOC — показать */
    ccpts = 900000;
    cc_select(d, CC_608, 1);
    ctl2(d, 0x14, 0x20);           /* RCL */
    ctl2(d, 0x14, 0x60);           /* PAC: строка 15, белый */
    txt608(d, "HELLO");
    ctl2(d, 0x11, 0x37);           /* ♪ */
    int64_t before = ccpts;
    ctl2(d, 0x14, 0x2F);           /* EOC */
    int64_t shown = ccpts - 3003;  /* второй EOC — повтор, показ — по первому */
    cc_flush(d);
    CHECK(!cc_at(d, before - 1, &sh));
    CHECK(cc_at(d, shown, &sh));
    EQI(sh.n, 1);
    EQS(sh.row[0].text, "HELLO♪");
    CHECK(sh.row[0].y > 0.9f && sh.row[0].x == 0);
    CHECK(cc_present608(d) == 1u);
    /* оформление 608: PAC курсив, смена цвета посреди строки (пробел), курсив с подчёркиванием (цвет прежний) */
    ctl2(d, 0x14, 0x20);           /* RCL */
    ctl2(d, 0x14, 0x2E);           /* ENM */
    ctl2(d, 0x14, 0x6E);           /* PAC: строка 15, курсив */
    txt608(d, "ab");
    ctl2(d, 0x11, 0x22);           /* зелёный */
    txt608(d, "cd");
    ctl2(d, 0x11, 0x2F);           /* курсив, подчёркнутый */
    txt608(d, "e");
    ctl2(d, 0x14, 0x2F);           /* EOC */
    cc_flush(d);
    CHECK(cc_at(d, ccpts, &sh));
    if (sh.n == 1) { char v[400]; ss_show(v, sizeof v, sh.row[0].text); EQS(v, "{i}ab {}{#4cd964}cd{iu} e"); }
    ctl2(d, 0x14, 0x60);           /* PAC: белый, без курсива — дальше как было */
    /* roll-up на 2 строки, PAC с отступом 4, расширенный знак вместо предыдущего (e → É) */
    ctl2(d, 0x14, 0x25);           /* RU2 (стирает показанное) */
    ctl2(d, 0x14, 0x2D);           /* CR */
    txt608(d, "AB");
    ctl2(d, 0x14, 0x2D);
    ctl2(d, 0x14, 0x72);           /* PAC строка 15, отступ 4 */
    txt608(d, "Ce");
    ctl2(d, 0x12, 0x21);           /* É */
    cc_flush(d);
    CHECK(cc_at(d, ccpts, &sh));
    EQI(sh.n, 2);
    if (sh.n == 2) {
        EQS(sh.row[0].text, "AB");
        EQS(sh.row[1].text, "CÉ");
        CHECK(sh.row[1].x > 0.1f);   /* 4 из 32 */
    }
    /* EDM — пусто */
    ctl2(d, 0x14, 0x2C);
    cc_flush(d);
    CHECK(!cc_at(d, ccpts, &sh));
    /* CC3 во втором поле: замечен, а показ CC1 не трогает */
    { uint8_t t[6] = {0xFD, par(0x15), par(0x20), 0xFD, par(0x15), par(0x20)}; cc_feed(d, ccpts, t, 2); ccpts += 3003; }
    { uint8_t t[3] = {0xFD, par('X'), par('Y')}; cc_feed(d, ccpts, t, 1); ccpts += 3003; }
    cc_flush(d);
    EQI(cc_present608(d), 1u | 4u);
    /* B-кадры: пары приходят не по порядку показа — склеиваются по PTS */
    cc_select(d, CC_608, 1);
    ccpts = 2000000;
    ctl2(d, 0x14, 0x29);           /* RDC: сразу на экран */
    ctl2(d, 0x14, 0x60);
    int64_t p0 = ccpts;
    uint8_t a[3] = {0xFC, par('A'), par('B')}, b[3] = {0xFC, par('C'), par('D')}, c[3] = {0xFC, par('E'), par('F')};
    cc_feed(d, p0 + 6006, c, 1);
    cc_feed(d, p0, a, 1);
    cc_feed(d, p0 + 3003, b, 1);
    cc_flush(d);
    CHECK(cc_at(d, p0 + 6006, &sh) && !strcmp(sh.row[0].text, "ABCDEF"));
    CHECK(cc_at(d, p0 + 3003, &sh) && !strcmp(sh.row[0].text, "ABCD"));
    /* 708: окно 0 (видно, отн. позиция 90 % / 50 %, привязка — низ по центру, 2 × 32), текст, CR, ещё строка */
    cc_select(d, CC_708, 1);
    ccpts = 3000000;
    uint8_t blk[] = {0x98, 0x20, 0x80 | 90, 50, (7 << 4) | 1, 31, 0, 0x90, 0x00, 0x80, 'H', 'i', ' ', 0x90, 0x00, 0x00, '7', '0', '8', 0x0D,
                     0x91, 0x30, 0x00, 0x00, 'L', 0xE9, 0x7F};   /* SPA курсив / прямо, SPC красный */
    uint8_t pk[64] = {0};
    int bl = (int)sizeof blk, tot = 2 + bl;
    int code = (tot + 1) / 2;
    pk[0] = (uint8_t)code;   /* последовательность 0, размер */
    pk[1] = (uint8_t)(1 << 5 | bl);
    memcpy(pk + 2, blk, (size_t)bl);
    uint8_t tr[96];
    int nt = 0;
    for (int i = 0; i < code * 2; i += 2, nt++) { tr[nt * 3] = i ? 0xFE : 0xFF; tr[nt * 3 + 1] = pk[i]; tr[nt * 3 + 2] = pk[i + 1]; }
    cc_feed(d, ccpts, tr, nt);
    cc_flush(d);
    CHECK(cc_present708(d) & 2);
    CHECK(cc_at(d, ccpts, &sh));
    EQI(sh.n, 2);
    if (sh.n == 2) {
        char v[400];
        ss_show(v, sizeof v, sh.row[0].text);
        EQS(v, "{i}Hi {}708");
        ss_show(v, sizeof v, sh.row[1].text);
        EQS(v, "{#ff0000}Lé♪");
        EQI(sh.row[0].align, 1);
        CHECK(sh.row[0].y > 0.7f && sh.row[1].y > sh.row[0].y);
    }
    cc_free(d);

    /* сквозь ретранслятор: TS с H.264 (PMT 0x1B), SEI с cc_data в начале каждого PES видео */
    relay_reset();
    sbuf ts = {0};
    uint8_t pat[] = {0, 0x00, 0xB0, 13, 0, 1, 0xC1, 0, 0, 0, 1, 0xE1, 0x00, 0, 0, 0, 0};
    ts_put(&ts, 0, 1, pat, sizeof pat);
    /* PMT: видео 0x102 с caption_service_descriptor: 608 поле 1 — eng */
    uint8_t pmt[] = {0, 0x02, 0xB0, 27, 0, 1, 0xC1, 0, 0, 0xE1, 0x02, 0xF0, 0,
                     0x1B, 0xE1, 0x02, 0xF0, 9, 0x86, 7, 0xC1, 'e', 'n', 'g', 0x3E, 0x3F, 0xFF,
                     0, 0, 0, 0};
    ts_put(&ts, 0x100, 1, pmt, sizeof pmt);
    const char *pairs[][2] = {{"\x14", "\x29"}, {"\x14", "\x29"}, {"\x14", "\x60"}, {"\x14", "\x60"}, {"SE", "I!"}};
    for (int k = 0; k < 20; k++) {   /* после текста — пустые пары: окно упорядочивания (0,3 с) проходит */
        uint8_t b1 = k < 5 ? (uint8_t)pairs[k][0][0] : 0x80, b2 = k < 5 ? (uint8_t)pairs[k][1][0] : 0x80;
        if (k == 4) { b1 = 'S'; b2 = 'E'; }
        if (k == 5) { b1 = 'I'; b2 = '!'; }
        /* AUD, SEI (t35: B5 0031 GA94 03, cc_data: 1 тройка), срез IDR */
        uint8_t es[] = {0, 0, 0, 1, 0x09, 0xF0,
                        0, 0, 0, 1, 0x06, 4, 15, 0xB5, 0x00, 0x31, 'G', 'A', '9', '4', 3, 0x41, 0xFF, 0xFC, par(b1), par(b2), 0xFF, 0x80,
                        0, 0, 0, 1, 0x65, 0x88, 0x84, 0x00};
        sbuf v = {0};
        pes(&v, 0xE0, 900000 + 3600 * k, es, sizeof es);
        ts_put(&ts, 0x102, 1, (const uint8_t *)v.s, v.n);
        sb_free(&v);
        if (k == 0) {   /* выбор — как только канал замечен */
            relay_tap((const uint8_t *)ts.s, ts.n);
            ts.n = 0;
            relay_cc_select(CC_608, 1);
        }
    }
    relay_tap((const uint8_t *)ts.s, ts.n);
    relay_cc cl[8];
    int ncl = relay_cc_list(cl, 8);
    CHECK(ncl >= 1 && cl[0].kind == CC_608 && cl[0].num == 1 && !strcmp(cl[0].lang, "eng"));
    /* последние кадры ждут упорядочивания — показ уже есть по пройденным */
    CHECK(relay_cc_at(900000 + 3600 * 6, &sh));
    if (sh.n) EQS(sh.row[0].text, "SEI!");
    CHECK(relay_cc_at(900000 + 3600 * 4, &sh) && !strcmp(sh.row[0].text, "SE"));   /* по времени кадра */
    relay_reset();
    sb_free(&ts);
}

static void test_cookies(void) {
    cookies_clear();
    clock_fake(1800000000);
    const char *h = "HTTP/1.1 200 OK\r\nDate: Fri, 15 Jan 2027 08:00:00 GMT\r\n"
                    "Set-Cookie: a=1; Domain=.Example.com; Path=/\r\n"                     /* все поддомены */
                    "Set-Cookie: b=2\r\n"                                                  /* только этот хост, каталог /live */
                    "Set-Cookie: s=3; Secure; Path=/\r\n"                                  /* только https */
                    "Set-Cookie: p=4; Path=/live/hd\r\n"                                   /* путь длиннее — раньше */
                    "Set-Cookie: x=5; Domain=other.com\r\n"                                /* чужой домен — нет */
                    "Set-Cookie: t=6; Expires=Fri, 15-Jan-27 09:00:00 GMT\r\n"             /* час по Date ответа */
                    "Set-Cookie: old=7; Expires=Mon, 01 Jan 1990 00:00:00 GMT\r\n";
    cookies_from_headers("https://cdn.example.com/live/index.m3u8", h);
    EQI(cookies_count(), 5);
    char c[512];
    cookies_header("https://cdn.example.com/live/hd/seg.ts?x=1", c, sizeof c);
    EQS(c, "p=4; b=2; t=6; a=1; s=3");
    cookies_header("http://cdn.example.com/live/seg.ts", c, sizeof c);
    EQS(c, "b=2; t=6; a=1");
    cookies_header("http://other.example.com/livex/seg.ts", c, sizeof c);   /* «/live» не префикс «/livex» */
    EQS(c, "a=1");
    cookies_header("http://example.com/", c, sizeof c);
    EQS(c, "a=1");
    cookies_header("http://notexample.com/", c, sizeof c);
    EQS(c, "");
    clock_fake(1800000000 + 3601);   /* t — истёк */
    cookies_header("http://cdn.example.com/live/seg.ts", c, sizeof c);
    EQS(c, "b=2; a=1");
    /* замена и удаление: Max-Age=0 */
    cookies_set("http://cdn.example.com/live/x", "b=new", 0);
    cookies_set("http://cdn.example.com/", "a=; Domain=example.com; Path=/; Max-Age=0", 0);
    cookies_header("http://cdn.example.com/live/seg.ts", c, sizeof c);
    EQS(c, "b=new");
    /* Cookie канала — первыми, одной строкой с хранилищем */
    sbuf o = {0};
    cookies_merge(&o, "User-Agent: T\r\nCookie: ch=9\r\n", "http://cdn.example.com/live/seg.ts");
    EQS(o.s, "User-Agent: T\r\nCookie: ch=9; b=new\r\n");
    sb_free(&o);
    /* IP: Domain не принимается, без него — только этот адрес */
    cookies_clear();
    cookies_set("http://10.0.0.1/a/b", "k=1; Domain=0.0.1", 0);
    cookies_set("http://10.0.0.1/a/b", "k=2", 0);
    cookies_header("http://10.0.0.1/a/c", c, sizeof c);
    EQS(c, "k=2");
    cookies_clear();
    clock_fake(0);
}

static void test_ttml(void) {
    /* тики, кадры, вложенные div (время — от родителя), br, span, сущности, пробелы */
    const char *x = "<?xml version=\"1.0\"?><!-- c --><tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:ttp=\"http://www.w3.org/ns/ttml#parameter\" "
                    "ttp:tickRate=\"10000000\" ttp:frameRate=\"25\"><head><metadata>meta текст</metadata></head><body><div begin=\"10s\">"
                    "<p begin=\"15000000t\" end=\"00:00:03.000\">Первая  <span tts:color=\"red\">строка</span><br/>\n  вторая &amp; &#x416;</p>"
                    "<p begin=\"00:00:04:05\" dur=\"1s\">Кадры</p><p begin=\"5s\" end=\"5s\">пусто</p></div></body></tt>";
    vt_n = 0;
    EQI(ttml_parse(x, strlen(x), 100, vt_cb, NULL), 2);
    EQI(vt_s[0], (int64_t)(111.5 * 90000));   /* 100 + 10 + 1,5 */
    EQI(vt_e[0], (int64_t)(113.0 * 90000));
    EQS(vt_t[0], "Первая {#ff0000}строка\nвторая & Ж");
    {
        const char *y = "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:tts=\"http://www.w3.org/ns/ttml#styling\"><head><styling>"
                        "<style xml:id=\"it\" tts:fontStyle=\"italic\"/><style xml:id=\"yb\" style=\"it\" tts:color=\"#FFFF00FF\" tts:fontWeight=\"bold\"/>"
                        "</styling></head><body><div style=\"it\"><p begin=\"1s\" end=\"2s\">весь <span tts:fontStyle=\"normal\">прямо</span> "
                        "<span style=\"yb\" tts:textDecoration=\"underline\">ярко</span><br/>низ</p></div></body></tt>";
        int keep = vt_n;
        vt_n = 0;
        EQI(ttml_parse(y, strlen(y), 0, vt_cb, NULL), 1);
        EQS(vt_t[0], "{i}весь {}прямо {ibu}{#ffff00}ярко\n{i}низ");
        vt_n = keep;
    }
    EQI(vt_s[1], (int64_t)(114.2 * 90000 + 0.5));   /* 4 с + 5 кадров по 25 */
    EQI(vt_e[1], (int64_t)(115.2 * 90000 + 0.5));
    EQS(vt_t[1], "Кадры");
    /* fMP4 stpp: шкала 1000, tfdt 20 с; первый документ — время по шкале дорожки, второй — от начала образца */
    sbuf sm[2] = {{0}, {0}}, init = {0}, segb = {0};
    sb_add(&sm[0], "<tt xmlns=\"http://www.w3.org/ns/ttml\"><body><div><p begin=\"00:00:20.500\" end=\"00:00:21.500\">Абсолютное</p></div></body></tt>");
    sb_add(&sm[1], "<tt xmlns=\"http://www.w3.org/ns/ttml\"><body><div><p begin=\"0.5s\" end=\"1.5s\">От образца</p></div></body></tt>");
    uint32_t durs[2] = {2000, 2000};
    mp4_build(&init, &segb, "stpp", 1000, 20000, sm, durs, 2);
    mp4sub_init in;
    CHECK(!mp4sub_init_parse((const uint8_t *)init.s, init.n, &in));
    EQI(in.kind, MP4SUB_STPP);
    EQI(in.timescale, 1000);
    vt_n = 0;
    EQI(webvtt_segment(segb.s, segb.n, &in, vt_cb, NULL), 2);
    EQI(vt_s[0], (int64_t)(20.5 * 90000));
    EQS(vt_t[0], "Абсолютное");
    EQI(vt_s[1], (int64_t)(22.5 * 90000));   /* образец с 22 с + 0,5 */
    EQI(vt_e[1], (int64_t)(23.5 * 90000));
    /* fMP4 wvtt: vttc/payl на всю длину образца, vtte — пусто */
    sbuf w[2] = {{0}, {0}}, pay = {0};
    box(&pay, "payl", "Реплика <b>MP4</b>", strlen("Реплика <b>MP4</b>"));
    box(&w[0], "vttc", pay.s, pay.n);
    box(&w[1], "vtte", NULL, 0);
    uint32_t wd[2] = {3000, 1000};
    mp4_build(&init, &segb, "wvtt", 1000, 7000, w, wd, 2);
    CHECK(!mp4sub_init_parse((const uint8_t *)init.s, init.n, &in));
    EQI(in.kind, MP4SUB_WVTT);
    vt_n = 0;
    EQI(webvtt_segment(segb.s, segb.n, &in, vt_cb, NULL), 1);
    EQI(vt_s[0], 7 * 90000);
    EQI(vt_e[0], 10 * 90000);
    EQS(vt_t[0], "Реплика {b}MP4");
    /* битый fMP4: base_data_offset у края uint64 и trun без полей не читают память и не зацикливаются */
    {
        sbuf seg2 = {0}, traf2 = {0}, moof2 = {0};
        uint8_t tf[12] = {0}, tr[20] = {0};
        put32(tf, 1);                       /* base_data_offset */
        uint64_t base = ~(uint64_t)0 - 4;
        for (int i = 0; i < 8; i++) tf[4 + i] = (uint8_t)(base >> (56 - 8 * i));
        put32(tr, 0x301); put32(tr + 4, 1); put32(tr + 8, 0); put32(tr + 12, 1000); put32(tr + 16, 10);
        box(&traf2, "tfhd", tf, sizeof tf);
        box(&traf2, "trun", tr, sizeof tr);
        box(&moof2, "traf", traf2.s, traf2.n);
        box(&seg2, "moof", moof2.s, moof2.n);
        mp4sub_init bad = {.kind = MP4SUB_STPP, .timescale = 1000};
        EQI(mp4sub_parse((const uint8_t *)seg2.s, seg2.n, &bad, vt_cb, NULL), 0);
        sb_clear(&traf2); sb_clear(&moof2); sb_clear(&seg2);
        uint8_t tf2[8] = {0, 2, 0, 0, 0, 0, 0, 1}, tr2[8] = {0};
        put32(tr2 + 4, 0xffffffffu);        /* sample_count, полей нет и size по умолчанию 0 */
        box(&traf2, "tfhd", tf2, sizeof tf2);
        box(&traf2, "trun", tr2, sizeof tr2);
        box(&moof2, "traf", traf2.s, traf2.n);
        box(&seg2, "moof", moof2.s, moof2.n);
        EQI(mp4sub_parse((const uint8_t *)seg2.s, seg2.n, &bad, vt_cb, NULL), 0);
        sb_free(&seg2); sb_free(&traf2); sb_free(&moof2);
    }
    /* голый TTML-сегмент и WebVTT — тем же входом */
    vt_n = 0;
    EQI(webvtt_segment("\n<tt><body><p begin=\"1s\" end=\"2s\">XML</p></body></tt>", 52, NULL, vt_cb, NULL), 1);
    EQI(vt_s[0], 90000);
    for (int i = 0; i < 2; i++) { sb_free(&sm[i]); sb_free(&w[i]); }
    sb_free(&pay);
    /* для сквозной проверки загрузчика (test_relay_http): IMSC в fMP4 */
    sb_add(&sm[0], "<tt xmlns=\"http://www.w3.org/ns/ttml\"><body><p begin=\"00:00:11.000\" end=\"00:00:13.000\">Привет из IMSC</p></body></tt>");
    uint32_t d1[1] = {6000};
    mp4_build(&IM.init, &IM.seg, "stpp", 1000, 10000, sm, d1, 1);
    sb_free(&sm[0]);
    sb_free(&init);
    sb_free(&segb);
}

static int lang_forms(const char *s) {
    int n = 1;
    for (const char *p = s; (p = strchr(p, '|')); p++) n++;
    return n;
}
/* подстановок в форме form (без «%%»); форма за пределами строки — последняя */
static int lang_form_ph(const char *s, int form) {
    const char *p = s;
    for (int f = 0; f < form; f++) {
        const char *q = strchr(p, '|');
        if (!q) break;
        p = q + 1;
    }
    const char *e = strchr(p, '|');
    if (!e) e = p + strlen(p);
    int n = 0;
    for (const char *q = p; q < e; q++) if (*q == '%' && q[1] != '%') n++;
    return n;
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
        int fe = lang_forms(e), fr = lang_forms(r);
        CHECK(fe == 2 || fe == 1);
        CHECK(fr == 3 || fr == 1);
        int pe = lang_form_ph(e, 0), pr = lang_form_ph(r, 0);
        if (pe != pr) { fails++; fprintf(stderr, "строка %d: подстановки en «%s» / ru «%s»\n", i, e, r); }
        for (int f = 1; f < fe; f++) if (lang_form_ph(e, f) != pe) { fails++; fprintf(stderr, "строка %d: форма %d en «%s»\n", i, f, e); }
        for (int f = 1; f < fr; f++) if (lang_form_ph(r, f) != pr) { fails++; fprintf(stderr, "строка %d: форма %d ru «%s»\n", i, f, r); }
    }
}

static void test_epg_urls(void) {
    playlist p = {0};
    strlist o = {0};
    p.epg_mode = EPG_MINE;
    p.user_epg = "http://a/epg\nhttp://a/epg\nhttp://b/epg";
    repo_epg_urls(&p, &o);
    EQI(o.n, 2);
    EQS(o.v[0], "http://a/epg");
    EQS(o.v[1], "http://b/epg");
    sl_clear(&o);
    p.epg_mode = EPG_BOTH;
    p.user_epg = "http://a/epg";
    p.header_epg = "http://a/epg\nhttp://b/epg";
    repo_epg_urls(&p, &o);
    EQI(o.n, 2);
    EQS(o.v[0], "http://a/epg");
    EQS(o.v[1], "http://b/epg");
    sl_clear(&o);
    p.epg_mode = EPG_PLAYLIST;
    p.user_epg = NULL;
    p.header_epg = "http://c/epg\nhttp://c/epg";
    repo_epg_urls(&p, &o);
    EQI(o.n, 1);
    EQS(o.v[0], "http://c/epg");
    sl_clear(&o);
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
    clock_fake(t);
    lang_set(LANG_RU);
    char d[40];
    fmt_day(t, d, sizeof d);
    EQS(d, "Сегодня");
    fmt_day(t - 86400, d, sizeof d);
    EQS(d, "Вчера");
    fmt_day(t + 86400, d, sizeof d);
    EQS(d, "Завтра");
    fmt_day(t - 2 * 86400, d, sizeof d);
    EQS(d, "Пн, 13.11");
    clock_fake(0);
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
    test_usb(dir);
    test_logocache();
    test_reminders();
    test_backup();
    test_tmdb();
    test_cookies();
    test_ttml();
    test_cc();
    test_teletext();
    test_dvbsub();
    test_webvtt();
    dvbsub_sample();
    cc_sample();
    test_lang();
    test_epg_urls();
    test_clock();
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd)) {}
    printf("%d проверок, ошибок: %d\n", checks, fails);
    return fails ? 1 : 0;
}
