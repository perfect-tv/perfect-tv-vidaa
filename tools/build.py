#!/usr/bin/env python3
"""Сборка Perfect TV для VIDAA (Linux/macOS; на Windows — scripts\\build.ps1 из SDK с этим app.json).

    python3 tools/build.py            # ТВ: ELF ARM32 (zig cc, glibc 2.28) -> build/arm/<binary>
    python3 tools/build.py --host     # ПК: тот же интерфейс с заглушкой плеера -> build/host/perfecttv-host
    python3 tools/build.py --tests    # юнит-тесты на ПК (M3U, XMLTV, архив, имена, EPG) и их запуск
    python3 tools/build.py --clean    # с нуля

Повторяет scripts/build.ps1 SDK (docs/BUILD-GUIDE.md, глава 4): шрифты и сертификаты вшиваются (.incbin),
OpenSSL статический, остальное — .so из prefix/, никогда -s. SDK ищется в $VIDAA_SDK, иначе ../native_app.
zig — $ZIG, иначе toolchain/zig-linux-x86_64-0.13.0/zig внутри SDK.
"""
import argparse, concurrent.futures, glob, hashlib, json, os, platform, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sdk_dir():
    d = os.environ.get('VIDAA_SDK') or os.path.join(os.path.dirname(ROOT), 'native_app')
    d = os.path.abspath(d)
    if not os.path.isfile(os.path.join(d, 'fw', 'app.h')):
        sys.exit('SDK VIDAA не найден: %s (задайте VIDAA_SDK)' % d)
    return d


def zig_path(sdk):
    z = os.environ.get('ZIG')
    if z:
        return z
    arch = {'x86_64': 'x86_64', 'AMD64': 'x86_64', 'aarch64': 'aarch64', 'arm64': 'aarch64'}.get(platform.machine(), 'x86_64')
    osn = {'Linux': 'linux', 'Darwin': 'macos'}.get(platform.system(), 'linux')
    p = os.path.join(sdk, 'toolchain', 'zig-%s-%s-0.13.0' % (osn, arch), 'zig')
    if os.path.isfile(p):
        return p
    w = shutil.which('zig')
    if w:
        return w
    sys.exit('нет zig 0.13.0: скачайте zig-%s-%s-0.13.0 с ziglang.org в %s/toolchain/ или задайте ZIG' % (osn, arch, sdk))


def load_cfg():
    with open(os.path.join(ROOT, 'app.json'), encoding='utf-8') as f:
        return json.load(f)


def newest(paths):
    t = 0
    for p in paths:
        try:
            t = max(t, os.path.getmtime(p))
        except OSError:
            pass
    return t


def write_if_changed(path, data):
    if os.path.isfile(path):
        with open(path, 'rb') as f:
            if f.read() == data:
                return
    with open(path, 'wb') as f:
        f.write(data)


def embed_asm(cfg, sdk, out, host):
    """Шрифты и корневые сертификаты в бинарник: .incbin (на ТВ файлы рядом с программой терялись)."""
    sym = cfg.get('symbolPrefix', 'app_')
    lines = ['    .section .rodata']
    for f in cfg.get('fonts', []):
        p = os.path.join(ROOT, 'fonts', f['file'])
        if not os.path.isfile(p):
            p = os.path.join(sdk, 'fonts', f['file'])
        if not os.path.isfile(p):
            sys.exit('нет шрифта ' + f['file'])
        s = '%sfont_%s' % (sym, f['symbol'])
        lines += ['    .balign 16', '    .global ' + s, '    .global %s_end' % s, s + ':', '    .incbin "%s"' % p, s + '_end:']
    if cfg.get('embedCa'):
        pem = open(os.path.join(sdk, 'assets', 'cacert.pem'), 'rb').read()
        for x in sorted(glob.glob(os.path.join(ROOT, 'assets', '*.pem'))):
            pem += b'\n' + open(x, 'rb').read()
        ca = os.path.join(out, 'ca_embed.pem')
        write_if_changed(ca, pem.replace(b'\r', b''))
        s = sym + 'ca_pem'
        lines += ['    .balign 16', '    .global ' + s, '    .global %s_end' % s, s + ':', '    .incbin "%s"' % ca, s + '_end:',
                  '    .byte 0']
    lines += ['    .section .note.GNU-stack,"",%s' % ('@progbits' if host else '%progbits'), '']
    path = os.path.join(out, 'embed.S')
    write_if_changed(path, '\n'.join(lines).encode())
    return path


def compile_all(jobs, verbose):
    """jobs: (cmd, src, obj, deps_mtime); пересобирается то, что старше исходника или заголовков."""
    todo = [j for j in jobs if not os.path.isfile(j[2]) or os.path.getmtime(j[2]) < max(os.path.getmtime(j[1]), j[3])]
    def run(j):
        r = subprocess.run(j[0], capture_output=True, text=True)
        return j, r
    failed = False
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for j, r in ex.map(run, todo):
            if verbose or r.returncode:
                print(' '.join(j[0]) if verbose else os.path.relpath(j[1], ROOT))
            if r.stdout or r.stderr:
                sys.stderr.write(r.stdout + r.stderr)
            if r.returncode:
                failed = True
    if failed:
        sys.exit('ошибка компиляции')
    return len(todo)


OWN_WARN = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-misleading-indentation',
            '-Wno-deprecated-declarations', '-Wno-unused-function']


def own_sources(cfg):
    return [os.path.join(ROOT, s) for s in cfg['sources']]


def headers(sdk):
    hs = []
    for d in (os.path.join(sdk, 'fw'), os.path.join(sdk, 'vendor'), os.path.join(ROOT, 'src'), os.path.join(ROOT, 'host')):
        for dp, _, fs in os.walk(d):
            hs += [os.path.join(dp, f) for f in fs if f.endswith(('.h', '.inc', '.def'))]
    return newest(hs)


def git_version(cfg):
    """Версия из git-тега v1.2.3 (как у Android-версии), иначе из app.json."""
    try:
        v = subprocess.run(['git', 'describe', '--tags', '--match', 'v[0-9]*', '--dirty'], cwd=ROOT,
                           capture_output=True, text=True).stdout.strip()
        if v:
            return v[1:]
    except OSError:
        pass
    return cfg['version']


def build_arm(cfg, sdk, verbose):
    zig = zig_path(sdk)
    out = os.path.join(ROOT, 'build', 'arm')
    os.makedirs(os.path.join(out, 'obj'), exist_ok=True)
    os.makedirs(os.path.join(out, 'objfw'), exist_ok=True)
    prefix = os.path.join(sdk, 'prefix')
    target = 'arm-linux-gnueabihf.2.28'   # glibc на ТВ 2.25–2.28: новее символов не просить
    ver = git_version(cfg)
    common = [zig, 'cc', '-target', target, '-std=gnu11', '-O2', '-fPIC', '-DNDEBUG', '-ffunction-sections', '-fdata-sections',
              '-DPTV_VERSION="%s"' % ver]
    common += ['-D' + d for d in cfg.get('defines', [])]
    if 'avutil' in cfg.get('shared', []):
        common.append('-DFW_USE_AVUTIL')
    inc = ['-I' + os.path.join(sdk, 'fw'), '-I' + os.path.join(sdk, 'vendor'), '-I' + os.path.join(ROOT, 'src'),
           '-I' + os.path.join(sdk, 'directfb-1.4'), '-I' + os.path.join(prefix, 'include'),
           '-I' + os.path.join(prefix, 'include', 'freetype2')]
    hm = headers(sdk)
    jobs = []
    fw = [os.path.join(sdk, 'fw', n + '.c') for n in cfg.get('fw', ['app', 'gfx', 'text', 'json', 'net', 'widgets'])]
    if cfg.get('shell', True):
        fw.append(os.path.join(sdk, 'fw', 'tv_shell.c'))
    if cfg.get('imgDecoder', 'stb') == 'stb':
        fw.append(os.path.join(sdk, 'fw', 'img_stb.c'))
    vend = [os.path.join(sdk, 'vendor', n + '.c') for n in cfg.get('vendorPlayers', [])]
    objs = []
    for s in fw + vend:   # чужой код — без -Werror
        o = os.path.join(out, 'objfw', os.path.splitext(os.path.basename(s))[0] + '.o')
        jobs.append((common + ['-w'] + inc + ['-c', s, '-o', o], s, o, hm))
        objs.append(o)
    for s in own_sources(cfg):
        o = os.path.join(out, 'obj', os.path.relpath(s, ROOT).replace('/', '_')[:-2] + '.o')
        jobs.append((common + OWN_WARN + inc + ['-c', s, '-o', o], s, o, hm))
        objs.append(o)
    n = compile_all(jobs, verbose)
    emb = embed_asm(cfg, sdk, out, False)
    shared = []
    for name in cfg.get('shared', []):
        c = sorted(f for f in glob.glob(os.path.join(prefix, 'lib', 'lib%s.so.*.*' % name)) if os.path.getsize(f) > 1000)
        if not c:
            sys.exit('нет lib%s в prefix/lib' % name)
        shared.append(c[-1])
    static = [os.path.join(prefix, 'lib', 'lib%s.a' % s) for s in cfg.get('staticLibs', ['ssl', 'crypto'])]
    exe = os.path.join(out, cfg['binary'])
    # НИКОГДА -s (strip): такой бинарник падал на ТВ, а по адресу падения потом ничего не найти
    link = [zig, 'cc', '-target', target] + objs + [emb] + static + shared + [
        '-Wl,-rpath,$ORIGIN/lib', '-Wl,--allow-shlib-undefined', '-Wl,--gc-sections', '-ldl', '-lpthread', '-lm', '-o', exe]
    if n or not os.path.isfile(exe) or os.path.getmtime(exe) < newest(objs + [emb]):
        r = subprocess.run(link)
        if r.returncode:
            sys.exit('ошибка компоновки')
    b = open(exe, 'rb').read()
    meta = {'binary': cfg['binary'], 'version': ver, 'size': len(b), 'sha256': hashlib.sha256(b).hexdigest(),
            'toolchain': 'zig cc ' + target, 'elf': 'class=%d machine=%d' % (b[4], int.from_bytes(b[18:20], 'little')),
            'built': time.strftime('%Y-%m-%d'), 'verified': 'builds + ELF only; NOT run on TV'}
    with open(exe + '.json', 'w') as f:
        json.dump(meta, f, indent=2)
    print('%s  %d байт  %s  версия %s' % (os.path.relpath(exe, ROOT), len(b), meta['elf'], ver))


def host_flags():
    def pc(*a):
        r = subprocess.run(['pkg-config'] + list(a), capture_output=True, text=True)
        if r.returncode:
            sys.exit('pkg-config %s: %s' % (' '.join(a), r.stderr.strip()))
        return r.stdout.split()
    return pc('--cflags', 'freetype2', 'zlib', 'openssl'), pc('--libs', 'freetype2', 'zlib', 'openssl')


def build_host(cfg, sdk, verbose, tests):
    cc = os.environ.get('CC', 'gcc')
    out = os.path.join(ROOT, 'build', 'host')
    os.makedirs(os.path.join(out, 'obj'), exist_ok=True)
    cflags, libs = host_flags()
    common = [cc, '-std=gnu11', '-O1', '-g', '-fno-omit-frame-pointer', '-DPTV_HOST=1', '-DPTV_VERSION="%s"' % git_version(cfg)]
    if os.environ.get('ASAN'):
        common += ['-fsanitize=address,undefined']
    inc = ['-I' + os.path.join(sdk, 'fw'), '-I' + os.path.join(sdk, 'vendor'), '-I' + os.path.join(ROOT, 'src'),
           '-I' + os.path.join(ROOT, 'host')] + cflags
    hm = headers(sdk)
    fw = [os.path.join(sdk, 'fw', n + '.c') for n in ('app', 'gfx', 'text', 'json', 'net', 'widgets', 'img_stb')]
    own = [s for s in own_sources(cfg) if not s.endswith('src/main.c')]
    if tests:   # тестам нужны только ядро и данные
        own = [s for s in own if ('/src/core/' in s or '/src/data/' in s) and os.path.isfile(s)]
    host = sorted(glob.glob(os.path.join(ROOT, 'host', '*.c')))
    # окно X11 (--window) — если есть libX11; без него — только сценарии и снимки
    x11 = subprocess.run(['pkg-config', '--exists', 'x11'], capture_output=True).returncode == 0
    if not x11:
        host = [h for h in host if not h.endswith('x11_window.c')]
        common = common + ['-DPTV_NO_X11=1']
    test_src = sorted(glob.glob(os.path.join(ROOT, 'tests', '*.c')))
    jobs, objs_fw, objs_own, objs_host, objs_test = [], [], [], [], []
    def add(srcs, lst, warn):
        for s in srcs:
            o = os.path.join(out, 'obj', os.path.relpath(s, os.path.dirname(ROOT) if s.startswith(sdk) else ROOT).replace('/', '_')[:-2] + '.o')
            jobs.append((common + warn + inc + ['-c', s, '-o', o], s, o, hm))
            lst.append(o)
    add(fw, objs_fw, ['-w'])
    add(own, objs_own, OWN_WARN + ['-Wno-format-truncation'])
    if not tests:
        add(host, objs_host, OWN_WARN + ['-Wno-format-truncation'])
    if tests:
        add(test_src, objs_test, OWN_WARN + ['-Wno-format-truncation'])
    compile_all(jobs, verbose)
    emb = embed_asm(cfg, sdk, out, True)
    ldl = (['-fsanitize=address,undefined'] if os.environ.get('ASAN') else []) + libs + ['-ldl', '-lpthread', '-lm']
    if tests:
        exe = os.path.join(out, 'perfecttv-tests')
        r = subprocess.run([cc] + objs_test + objs_own + objs_fw + [emb] + ['-o', exe] + ldl)
        if r.returncode:
            sys.exit('ошибка компоновки тестов')
        env = dict(os.environ, PTV_TEST_DATA=os.path.join(ROOT, 'tests', 'data'))
        r = subprocess.run([exe], env=env)
        sys.exit(r.returncode)
    exe = os.path.join(out, 'perfecttv-host')
    xl = subprocess.run(['pkg-config', '--libs', 'x11'], capture_output=True, text=True).stdout.split() if x11 else []
    r = subprocess.run([cc] + objs_own + objs_host + objs_fw + [emb] + ['-o', exe] + ldl + xl)
    if r.returncode:
        sys.exit('ошибка компоновки')
    print(os.path.relpath(exe, ROOT))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', action='store_true')
    ap.add_argument('--tests', action='store_true')
    ap.add_argument('--clean', action='store_true')
    ap.add_argument('-v', '--verbose', action='store_true')
    a = ap.parse_args()
    cfg = load_cfg()
    sdk = sdk_dir()
    if a.clean:
        shutil.rmtree(os.path.join(ROOT, 'build', 'host' if a.host or a.tests else 'arm'), ignore_errors=True)
    if a.host or a.tests:
        build_host(cfg, sdk, a.verbose, a.tests)
    else:
        build_arm(cfg, sdk, a.verbose)


if __name__ == '__main__':
    main()
