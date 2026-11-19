#!/usr/bin/env python3
"""Упаковка в .ipk для VIDAA (порт scripts/pack.ps1 SDK, docs/BUILD-GUIDE.md, глава 5).

    python3 tools/pack.py [--version 0.1.1] [--app-id N] [--package tv.x.y]

ar(debian-binary, control.tar.gz, data.tar.gz); файлы — в APPS/pkgs/<package>/, конфиг менеджера ещё и в
APPS/pkgs/appconfigs/<AppId>_config.json (владелец 314, побайтно равен appconfig.json). Пакет НЕ подписан:
подпись и публикация — publish/vidaapp-author.cjs из SDK. Версия должна расти при каждой загрузке.
"""
import argparse, glob, gzip, hashlib, io, json, os, re, struct, sys, tarfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build import sdk_dir, load_cfg, git_version  # noqa: E402

VENDOR = ('/usr/local/lib:/lib:/basic:/basic/lib:/basic/lib/directfb:/vendor/lib:/vendor/lib/appipc:/vendor/lib/aipq:'
          '/vendor/lib/utopia:/vendor/lib/utopia/mi_ipc_client:/opt/mmlib:/opt/hisenseUI/lib')


def tar_gz(entries):
    """entries: (name, data|None для каталога, mode, owner); mtime 0 — пакет воспроизводим."""
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode='w', format=tarfile.USTAR_FORMAT) as t:
        for name, data, mode, owner in entries:
            ti = tarfile.TarInfo(name)
            ti.mode = mode
            ti.uid = ti.gid = owner
            ti.mtime = 0
            ti.uname = ti.gname = ''
            if data is None:
                ti.type = tarfile.DIRTYPE
                t.addfile(ti)
            else:
                ti.size = len(data)
                t.addfile(ti, io.BytesIO(data))
    out = io.BytesIO()
    with gzip.GzipFile(fileobj=out, mode='wb', mtime=0) as z:
        z.write(raw.getvalue())
    return out.getvalue()


def ar_member(name, data):
    head = ('%-16s%-12s%-6s%-6s%-8s%-10s`\n' % (name + '/', '0', '0', '0', '100644', len(data))).encode()
    return head + data + (b'\n' if len(data) % 2 else b'')


def runtime_lib(sdk, n):
    for f in sorted(glob.glob(os.path.join(sdk, 'runtime-libs', 'lib%s.so.*' % n))):
        if re.match(r'^lib%s\.so\.\d+$' % re.escape(n), os.path.basename(f)):
            return f
    sys.exit('нет runtime-libs/lib%s.so.N в SDK' % n)


def is_arm(b):
    return len(b) > 20 and b[:4] == b'\x7fELF' and b[4] == 1 and struct.unpack('<H', b[18:20])[0] == 40


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version')
    ap.add_argument('--app-id')
    ap.add_argument('--package')
    a = ap.parse_args()
    cfg = load_cfg()
    sdk = sdk_dir()
    version = a.version or git_version(cfg)
    m = re.match(r'^(\d+\.\d+\.\d+)', version)
    version = m.group(1) if m else cfg['version']   # магазин и менеджер: только числа
    app_id = a.app_id or cfg['appId']
    package = a.package or cfg['package']
    name = cfg['name']
    arch = cfg.get('architecture', 'mtk9655')
    if arch not in ('mtk9655', 'mtk9618'):
        sys.exit('architecture: mtk9655 или mtk9618')
    pfx = cfg.get('envPrefix', 'APP')
    out = os.path.join(ROOT, 'build', 'arm')
    exe = os.path.join(out, cfg['binary'])
    if not os.path.isfile(exe):
        sys.exit('сначала: python3 tools/build.py')
    binary = open(exe, 'rb').read()
    if not is_arm(binary):
        sys.exit('не ARM32 ELF')
    if cfg['registerName'].encode() not in binary:
        sys.exit('в бинарнике нет RegisterName ' + cfg['registerName'])
    slug = package.split('.')[-1]

    icon_path = os.path.join(ROOT, cfg.get('icon', 'assets/icon-400.png'))
    icon = open(icon_path, 'rb').read()

    acfg = json.load(open(os.path.join(sdk, 'reference', 'minimal-appconfig.json'), encoding='utf-8'))
    acfg['AppId'] = app_id
    acfg['Name'] = name
    acfg['Version'] = version
    acfg['PackageName'] = package
    acfg['Main'] = 'start_%s.sh' % slug
    portal = acfg['Portals'][0]
    portal['Runtime'] = 'Native'   # в объекте портала, НЕ в StartCfg
    portal['StartCfg']['RegisterName'] = cfg['registerName']
    portal['StartCfg']['IsNetworkRequired'] = bool(cfg.get('network', True))
    config_data = (json.dumps(acfg, ensure_ascii=False, indent=2) + '\n').encode()
    meta = {'AppId': app_id, 'Id': package, 'AppName': name, 'Architecture': arch, 'Version': version, 'Format': 1,
            'PackageType': 'Entry', 'Persistent': False, 'Section': 'native'}
    meta_data = (json.dumps(meta, ensure_ascii=False, indent=2) + '\n').encode()

    start = f'''#!/bin/sh
umask 077
HERE=/APPS/pkgs/{package}
export LD_LIBRARY_PATH="$HERE/lib:{VENDOR}"
# Данные: $HERE/data, если доступна запись; иначе свой каталог в /APPS (переживает перезагрузку); иначе /tmp.
if ! {{ mkdir -p "$HERE/data" && [ -w "$HERE/data" ]; }} 2>/dev/null; then
  export {pfx}_DATA_DIR=/APPS/{slug}-native-data
  mkdir -p "${pfx}_DATA_DIR" 2>/dev/null || export {pfx}_DATA_DIR=/tmp/{slug}-native-data
fi
LOG="$HERE/.startup.log"
{{ : >>"$LOG"; }} 2>/dev/null || LOG=/tmp/{slug}-native.log
[ "$(wc -c <"$LOG" 2>/dev/null || echo 0)" -gt 4194304 ] && : >"$LOG"
export {pfx}_LOG="$LOG" {pfx}_VER="{version}"
exec "$HERE/{cfg['binary']}" {cfg.get('startArgs', '--ui')} >>"$LOG" 2>&1
'''.replace('${pfx}_DATA_DIR', '$' + pfx + '_DATA_DIR').encode()

    files = [('packageinfo.json', meta_data, 0o644), ('appconfig.json', config_data, 0o644),
             ('start_%s.sh' % slug, start, 0o755), (cfg['binary'], binary, 0o755), ('icon.png', icon, 0o644)]
    for n in dict.fromkeys(cfg.get('shared', []) + cfg.get('shipExtra', [])):
        f = runtime_lib(sdk, n)
        b = open(f, 'rb').read()
        if not is_arm(b):
            sys.exit(f + ': не ARM32')
        files.append(('lib/' + os.path.basename(f), b, 0o755))
    if cfg.get('shipCa'):
        ca = open(os.path.join(sdk, 'assets', 'cacert.pem'), 'rb').read()
        for x in sorted(glob.glob(os.path.join(ROOT, 'assets', '*.pem'))):
            ca += b'\n' + open(x, 'rb').read()
        files.append(('cacert.pem', ca, 0o644))
    files.append(('SOURCE.txt', (cfg.get('credits', '') + '\n').encode(), 0o644))
    font_lic = open(os.path.join(sdk, 'fonts', 'LICENSE'), 'rb').read()
    onest = os.path.join(sdk, 'fonts', 'OFL-Onest.txt')
    if os.path.isfile(onest):
        font_lic += b'\n\n' + open(onest, 'rb').read()
    files.append(('LICENSE-fonts.txt', font_lic, 0o644))
    files.append(('LICENSE.txt', open(os.path.join(ROOT, 'LICENSE'), 'rb').read(), 0o644))

    root = 'APPS/pkgs/' + package
    control = (f'Package: {package}\nVersion: {version}\nArchitecture: {arch}\nAppId: {app_id}\nAppName: {name}\n'
               f'Format: 1\nPackageType: Entry\nPersistent: false\nSection: native\nDescription: {name}\n').encode()
    data = [(root, None, 0o755, 0), (root + '/lib', None, 0o755, 0)]
    data += [(root + '/' + n, b, mode, 0) for n, b, mode in files]
    data.append(('APPS/pkgs/appconfigs/%s_config.json' % app_id, config_data, 0o751, 314))
    ipk = b'!<arch>\n' + ar_member('debian-binary', b'2.0\n') + ar_member('control.tar.gz', tar_gz([('control', control, 0o644, 0)])) + \
        ar_member('data.tar.gz', tar_gz(data))
    path = os.path.join(out, package + '.ipk')
    open(path, 'wb').write(ipk)
    if len(ipk) > 32 << 20:
        print('ВНИМАНИЕ: магазин принимает .ipk до 32 МБ, этот больше')
    manifest = {'package': package, 'appId': app_id, 'registerName': cfg['registerName'], 'version': version, 'architecture': arch,
                'sha256': hashlib.sha256(ipk).hexdigest(), 'bytes': len(ipk), 'binarySha256': hashlib.sha256(binary).hexdigest(),
                'tvInstalled': False, 'note': 'builds; NOT run on the TV; not signed', 'packed': time.strftime('%Y-%m-%d')}
    json.dump(manifest, open(os.path.join(out, 'manifest.json'), 'w'), indent=2)
    print('%s  %.1f МБ  версия %s  AppId %s' % (os.path.relpath(path, ROOT), len(ipk) / 1048576, version, app_id))


if __name__ == '__main__':
    main()
