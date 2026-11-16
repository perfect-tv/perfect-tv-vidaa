#!/usr/bin/env python3
"""Демо-данные для проверки на ПК: плейлист M3U (категории, архив Flussonic, один канал без архива и «падающий»)
и XMLTV (gzip) с передачами от −3 до +3 суток вокруг текущего времени.

    python3 tools/make_demo.py build/host/demo      # demo.m3u, demo.xml.gz
Затем в запускалке на ПК добавить плейлист по пути …/demo.m3u (README, «Проверка на ПК»).
"""
import gzip, os, random, sys, time

out = sys.argv[1] if len(sys.argv) > 1 else 'build/host/demo'
os.makedirs(out, exist_ok=True)
out = os.path.abspath(out)
random.seed(7)
groups = {
    'Новости': ['Первый канал', 'Россия 24', 'РБК', 'Euronews'],
    'Кино': ['Кинохит', 'Дом кино', 'TV1000', 'Кинопремьера', 'Мир сериала'],
    'Детские': ['Карусель', 'Мульт', 'Nickelodeon'],
    'Спорт': ['Матч! ТВ', 'Матч! Футбол 1', 'Eurosport 1'],
    'Познавательные': ['Discovery Channel', 'National Geographic', 'Моя планета', 'Animal Planet', 'History'],
}
titles = ['Новости', 'Время покажет', 'Доброе утро', 'Вечерний Ургант', 'Документальный фильм', 'Сериал «Тайны следствия»',
          'Футбол. Премьер-лига', 'Мультфильмы', 'Х/ф «Иван Васильевич меняет профессию»', 'Погода', 'Пусть говорят',
          'Как это устроено', 'Дикая природа Африки', 'Лучший город Земли', 'Ток-шоу «Мужское / Женское»']
lines = ['#EXTM3U url-tvg="file://%s/demo.xml.gz" catchup="flussonic" catchup-days="3"' % out]
chans = []
n = 0
for g, names in groups.items():
    for name in names:
        n += 1
        cid = 'ch%d' % n
        chans.append((cid, name))
        extra = ''
        url = 'http://demo.invalid/%s/index.m3u8' % cid
        if name == 'Euronews':
            extra = ' catchup="disabled"'
        if name == 'History':
            url = 'http://demo.invalid/fail/index.m3u8'
        lines.append('#EXTINF:-1 tvg-id="%s" group-title="%s"%s,%s' % (cid, g, extra, name))
        lines.append(url)
open(os.path.join(out, 'demo.m3u'), 'w', encoding='utf-8').write('\n'.join(lines) + '\n')

now = int(time.time())
start0 = now - 3 * 86400
start0 -= start0 % 3600
x = ['<?xml version="1.0" encoding="utf-8"?>', '<tv generator-info-name="make_demo">']
for cid, name in chans:
    x.append('<channel id="%s"><display-name lang="ru">%s</display-name></channel>' % (cid, name))
def ts(t):
    return time.strftime('%Y%m%d%H%M%S +0000', time.gmtime(t))
for cid, name in chans:
    t = start0
    while t < now + 3 * 86400:
        d = random.choice([15, 30, 45, 60, 90, 120]) * 60
        title = random.choice(titles)
        desc = ('Описание передачи «%s» на канале %s. ' % (title, name)) * random.randint(1, 4)
        x.append('<programme start="%s" stop="%s" channel="%s"><title lang="ru">%s</title><desc lang="ru">%s</desc>'
                 '<category lang="ru">%s</category></programme>' % (ts(t), ts(t + d), cid, title.replace('&', '&amp;'), desc, random.choice(['Новости', 'Кино', 'Спорт', 'Детям'])))
        t += d
x.append('</tv>')
with gzip.open(os.path.join(out, 'demo.xml.gz'), 'wt', encoding='utf-8') as f:
    f.write('\n'.join(x))
print(os.path.join(out, 'demo.m3u'))
