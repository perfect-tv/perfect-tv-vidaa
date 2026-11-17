/* Ретранслятор потока для телетекста и субтитров. Системный плеер (vendor/player-hmc.c) отбрасывает все дорожки,
 * кроме видео и звука, а vendor/ не правится, поэтому, когда пользователь включил телетекст или субтитры, плееру
 * отдаётся адрес http://127.0.0.1:<порт>/s?u=…&h=…: ретранслятор сам скачивает поток (свой неблокирующий клиент:
 * HTTPS, редиректы, заголовки канала и плеера; UDP / RTP-мультикаст и RTSP — тоже, плееру они уходят как HTTP
 * с MPEG-TS) и отдаёт байты как есть, попутно разбирая MPEG-TS: PAT → PMT → дорожка телетекста
 * (дескриптор 0x56 / 0x46) → PES → core/ttx.c. Плейлисты HLS переписываются: адреса сегментов, ключей и вариантов
 * тоже идут через ретранслятор. Второго подключения к провайдеру нет.
 * Устройство: один поток с циклом событий epoll («ptv-relay»): неблокирующие сокеты и TLS, keep-alive с обеих
 * сторон, плеер не успевает читать — провайдер приостанавливается. DNS — короткий поток на запрос («ptv-dns»),
 * итог — через трубу в цикл, кэш минуту. Слушает только 127.0.0.1. Адреса потоков в журнал не пишутся. */
#ifndef PTV_RELAY_H
#define PTV_RELAY_H
#include <stddef.h>
#include <stdint.h>
#include "ttx.h"
#include "dvbsub.h"

/* Поднять сервер (один раз); 0 — работает. */
int relay_start(void);
/* Адрес для плеера: http(s), udp:// и rtp:// (мультикаст или приём на порт), rtsp:// (только MPEG-TS); иначе -1. */
int relay_url(const char *url, const char *headers, char *out, size_t cap);
int relay_scheme_ok(const char *url);
/* SDP RTSP (для тестов): адреса SETUP медиа с MPEG-TS и PLAY сеанса; -1 — MPEG-TS нет */
int relay_sdp_pick(const char *sdp, const char *base, char *media, char *aggr, size_t cap);
/* Новый канал / новая ссылка: разбор потока и телетекст — заново. */
void relay_reset(void);
ttx *relay_ttx(void);
/* Состояние разбора: байтов TS, найдена ли дорожка телетекста, есть ли DVB-субтитры. */
long long relay_ts_bytes(void);
int relay_has_teletext(void);
int relay_has_dvbsub(void);
/* Время кадра потока: первая метка PTS, которую видел ретранслятор (90 кГц); -1 — ещё нет. */
int64_t relay_base_pts(void);
/* DVB-субтитры (дескриптор 0x59 в PMT): дорожки и выбранная. Выбор — главный поток; декодер живёт до
 * следующего выбора или relay_reset (тоже главный поток), поэтому указатель из relay_dvbsub — до них. */
typedef struct { int pid, type, comp, anc; char lang[4]; } relay_dvb;
int relay_dvb_list(relay_dvb *out, int max);
void relay_dvb_select(int index);        /* номер в relay_dvb_list; -1 — выключить */
int relay_dvb_selected(void);
dvbsub *relay_dvbsub(void);
/* Текстовые субтитры, объявленные в главном плейлисте HLS (#EXT-X-MEDIA:TYPE=SUBTITLES): адрес провайдера и заголовки
 * канала — для core/webvtt.c; вид (WebVTT / TTML) — по CODECS вариантов с этой группой субтитров. */
typedef struct { char name[64], lang[16], url[4096], hdr[2048], group[64], codec[16]; } relay_vtt;   /* codec: wvtt, stpp или «» */
int relay_vtt_list(relay_vtt *out, int max);
/* разбор TS (для тестов): кусок байтов как из сети */
void relay_tap(const uint8_t *d, size_t n);
#endif
