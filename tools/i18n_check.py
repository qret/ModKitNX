#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ModKitNX — проверка переводов (resources/i18n). Как переводить — TRANSLATING.md.

    py -3 tools/i18n_check.py              проверить все языки
    py -3 tools/i18n_check.py --self-test  негативная проба: испорченные в памяти копии должны быть пойманы

Что проверяется:
  1. languages.json: у каждого языка есть code, name, english, choose и папка resources/i18n/<code>/;
  2. в каждой папке те же файлы, что в en-US (основной язык, запасной путь);
  3. все ключи en-US есть в каждом языке, лишних ключей нет, значения — непустые строки;
  4. плейсхолдеры {0}, {1}… в переводе те же, что в en-US;
  5. файлы — UTF-8 без BOM, без CR;
  6. ключи, которые код берёт по имени (tr("…"), trf("…"), textOf("app/…"), ключи ядра plan/…, warn/…,
     коды ошибок Error("…") и fail("…")), есть в en-US; ключи других файлов — trk("<файл>/<ключ>") (update.json,
     theme.json…) — есть в en-US/<файл>.json.
Код выхода: 0 — всё в порядке, 1 — найдены ошибки (список выше), 2 — не найдены файлы.
Ничего не пишет и не удаляет.
"""
import copy
import json
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
I18N = os.path.join(ROOT, 'resources', 'i18n')
BASE = 'en-US'
PH = re.compile(r'\{(\d+)\}')


def flatten(d, prefix=''):
    out = {}
    for k, v in d.items():
        key = prefix + k
        if isinstance(v, dict):
            out.update(flatten(v, key + '/'))
        else:
            out[key] = v
    return out


def load_tree():
    """{'languages': [...], 'files': {code: {file: (raw_bytes, dict | None, error)}}}"""
    with open(os.path.join(I18N, 'languages.json'), 'rb') as f:
        langs_raw = f.read()
    langs = json.loads(langs_raw.decode('utf-8'))
    files = {}
    codes = [l.get('code', '') for l in langs.get('languages', [])]
    for code in sorted(set(codes + [BASE])):
        d = os.path.join(I18N, code)
        files[code] = {}
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if not name.endswith('.json'):
                continue
            with open(os.path.join(d, name), 'rb') as f:
                raw = f.read()
            try:
                files[code][name] = (raw, json.loads(raw.decode('utf-8')), None)
            except Exception as e:  # битый JSON — тоже находка
                files[code][name] = (raw, None, str(e))
    return {'languages': langs, 'languages_raw': langs_raw, 'files': files}


def is_prefix(text, end):
    """Литерал — начало составного ключа ("fixes/name_" + id) или префикс для rfind — не проверяется."""
    tail = text[end:end + 12].lstrip()
    return tail.startswith('+') or re.match(r',\s*0\)', tail) is not None


def code_keys():
    """Ключи, которые код берёт по имени: ({app_key}, {core_key}, {"<файл>/<ключ>"} — trk/trkf/trkv)."""
    app, core, other = set(), set(), set()
    dirs = ['ui', 'source', 'preview', 'core/src', 'platform/src']
    for d in dirs:
        for base, _, names in os.walk(os.path.join(ROOT, d)):
            for n in names:
                if not n.endswith(('.cpp', '.hpp', '.h')):
                    continue
                text = open(os.path.join(base, n), encoding='utf-8').read()
                for m in re.finditer(r'\btr[fv]?\(\s*"([a-z0-9_]+/[a-z0-9_/]+)"', text):
                    if not is_prefix(text, m.end()):
                        app.add(m.group(1))
                for m in re.finditer(r'\btrk[fv]?\(\s*"([a-z0-9_]+/[a-z0-9_/]+)"', text):
                    if not is_prefix(text, m.end()):
                        other.add(m.group(1))
                for m in re.finditer(r'"app/([a-z0-9_]+/[a-z0-9_/]+)"', text):
                    if not is_prefix(text, m.end()):
                        app.add(m.group(1))
                for m in re.finditer(r'translateIds\([^;]*?\{([^}]*)\}', text, re.S):
                    for k in re.findall(r'"([a-z0-9_]+/[a-z0-9_/]+)"', m.group(1)):
                        app.add(k)
                for m in re.finditer(r'setFrameTitle\(\s*this,\s*"([a-z0-9_/]+)"', text):
                    app.add(m.group(1))
                if d in ('core/src', 'preview', 'source'):
                    for m in re.finditer(r'"((?:plan|warn|skip|done|skipped|gate|logs)/[a-z_]+)"', text):
                        if not is_prefix(text, m.end()):
                            core.add(m.group(1))
                if d == 'core/src':
                    for m in re.finditer(r'\bError\("([A-Z0-9_]+)"', text):
                        core.add('err/' + m.group(1))
                if d == 'platform/src':
                    for m in re.finditer(r'\bfail\("([A-Z0-9_]+)"', text):
                        core.add('err/' + m.group(1))
    return app, core, other


def check(tree, used=None):
    errs = []
    langs = tree['languages'].get('languages', [])
    if tree['languages'].get('default', BASE) != BASE:
        errs.append('languages.json: "default" должен быть %s' % BASE)
    seen = set()
    for l in langs:
        code = l.get('code', '')
        for f in ('code', 'name', 'english', 'choose'):
            if not isinstance(l.get(f), str) or not l.get(f).strip():
                errs.append('languages.json: у языка %r нет поля %r' % (code, f))
        if code in seen:
            errs.append('languages.json: язык %s указан дважды' % code)
        seen.add(code)
        if code and not tree['files'].get(code):
            errs.append('languages.json: нет папки resources/i18n/%s/ (или в ней нет .json)' % code)
    if BASE not in seen:
        errs.append('languages.json: нет основного языка %s' % BASE)
    raw = tree.get('languages_raw', b'')
    if raw.startswith(b'\xef\xbb\xbf') or b'\r' in raw:
        errs.append('languages.json: нужен UTF-8 без BOM и LF')

    base = tree['files'].get(BASE, {})
    if not base:
        errs.append('нет папки %s' % BASE)
        return errs
    base_flat = {}
    for name, (raw, data, err) in base.items():
        if err:
            errs.append('%s/%s: JSON не разбирается: %s' % (BASE, name, err))
            continue
        base_flat[name] = flatten(data)
    for code in sorted(seen | {BASE}):
        files = tree['files'].get(code, {})
        for name, (raw, data, err) in files.items():
            where = '%s/%s' % (code, name)
            if raw.startswith(b'\xef\xbb\xbf'):
                errs.append('%s: BOM в начале файла (нужен UTF-8 без BOM)' % where)
            if b'\r' in raw:
                errs.append('%s: окончания строк CRLF (нужен LF)' % where)
            if err:
                if code != BASE:
                    errs.append('%s: JSON не разбирается: %s' % (where, err))
                continue
            if name not in base_flat:
                errs.append('%s: лишний файл (в %s такого нет)' % (where, BASE))
                continue
            flat = flatten(data)
            ref = base_flat[name]
            for k in ref:
                if k not in flat:
                    errs.append('%s: нет ключа %s' % (where, k))
            for k, v in flat.items():
                if k not in ref:
                    errs.append('%s: лишний ключ %s (в %s его нет)' % (where, k, BASE))
                    continue
                if not isinstance(v, str) or not v.strip():
                    errs.append('%s: %s — значение должно быть непустой строкой' % (where, k))
                    continue
                if isinstance(ref[k], str):
                    a, b = sorted(set(PH.findall(ref[k]))), sorted(set(PH.findall(v)))
                    if a != b:
                        errs.append('%s: %s — плейсхолдеры {%s}, а в %s {%s}' %
                                    (where, k, ','.join(b), BASE, ','.join(a)))
                # Фигурные скобки, кроме {N}, ломают подстановку.
                rest = PH.sub('', v)
                if '{' in rest or '}' in rest:
                    errs.append('%s: %s — лишняя фигурная скобка (допустимо только {0}, {1}…)' % (where, k))
        for name in base_flat:
            if code != BASE and name not in files:
                errs.append('%s: нет файла %s (скопируйте из %s)' % (code, name, BASE))
    if used is not None:
        app_used, core_used = used[0], used[1]
        other_used = used[2] if len(used) > 2 else set()
        app_ref = base_flat.get('app.json', {})
        core_ref = base_flat.get('core.json', {})
        for k in sorted(app_used):
            if k not in app_ref:
                errs.append('код берёт ключ app/%s, а в %s/app.json его нет' % (k, BASE))
        for k in sorted(core_used):
            if k not in core_ref:
                errs.append('код берёт ключ core/%s, а в %s/core.json его нет' % (k, BASE))
        for k in sorted(other_used):
            fname, _, rest = k.partition('/')
            ref = base_flat.get(fname + '.json')
            if ref is None:
                errs.append('код берёт ключ %s, а файла %s/%s.json нет' % (k, BASE, fname))
            elif rest not in ref:
                errs.append('код берёт ключ %s, а в %s/%s.json его нет' % (k, BASE, fname))
    return errs


def self_test(tree):
    """Негативная проба: каждая порча (в памяти) должна дать находку."""
    cases = []

    def spoil(desc, fn, expect):
        t = copy.deepcopy(tree)
        fn(t)
        found = check(t)
        ok = any(expect in e for e in found)
        cases.append((desc, ok, found[:2]))

    def data(t, code, name):
        raw, d, err = t['files'][code][name]
        return d

    def set_data(t, code, name, d, raw=None):
        old = t['files'][code][name]
        t['files'][code][name] = (raw if raw is not None else old[0], d, None)

    def drop_key(t):
        d = data(t, 'uk', 'app.json')
        del d['games']['title']

    def extra_key(t):
        d = data(t, 'ru', 'core.json')
        d['plan']['no_such_key'] = 'лишний'

    def wrong_ph(t):
        d = data(t, 'uk', 'core.json')
        d['plan']['slot_free'] = 'Модуль займе місце {1}'

    def lost_ph(t):
        d = data(t, 'ru', 'app.json')
        d['games']['count'] = 'Игр много'

    def empty_val(t):
        d = data(t, 'uk', 'app.json')
        d['hint']['back'] = ''

    def stray_brace(t):
        d = data(t, 'ru', 'app.json')
        d['common']['back'] = 'Назад {'

    def missing_file(t):
        del t['files']['uk']['core.json']

    def bom(t):
        raw, d, err = t['files']['ru']['app.json']
        t['files']['ru']['app.json'] = (b'\xef\xbb\xbf' + raw, d, None)

    def no_folder(t):
        t['languages']['languages'].append({'code': 'xx', 'name': 'Xx', 'english': 'Xx', 'choose': 'Xx'})
        t['files']['xx'] = {}

    def no_name(t):
        t['languages']['languages'][1].pop('name', None)

    def no_english(t):
        t['languages']['languages'][2].pop('english', None)

    spoil('нет ключа в uk/app.json', drop_key, 'нет ключа games/title')
    spoil('лишний ключ в ru/core.json', extra_key, 'лишний ключ plan/no_such_key')
    spoil('чужой плейсхолдер в uk/core.json', wrong_ph, 'plan/slot_free — плейсхолдеры')
    spoil('потерян плейсхолдер в ru/app.json', lost_ph, 'games/count — плейсхолдеры')
    spoil('пустое значение', empty_val, 'hint/back — значение должно быть непустой строкой')
    spoil('лишняя скобка', stray_brace, 'лишняя фигурная скобка')
    spoil('нет файла core.json в uk', missing_file, 'uk: нет файла core.json')
    spoil('BOM', bom, 'BOM в начале файла')
    spoil('язык без папки', no_folder, 'нет папки resources/i18n/xx/')
    spoil('язык без самоназвания', no_name, "нет поля 'name'")
    spoil('язык без имени по-английски', no_english, "нет поля 'english'")

    # Ключ, который код берёт, а в en-US его нет (и настоящий список ключей кода не пуст).
    app_used, core_used, other_used = code_keys()
    found = check(tree, (app_used | {'games/no_such_key'}, core_used | {'err/FAIL_NO_SUCH'},
                         other_used | {'update/no_such_key', 'nosuchfile/key'}))
    cases.append(('код берёт несуществующие ключи', any('app/games/no_such_key' in e for e in found) and
                  any('core/err/FAIL_NO_SUCH' in e for e in found) and len(app_used) > 100 and len(core_used) > 100,
                  found[:2]))
    cases.append(('код берёт несуществующий ключ другого файла (trk)',
                  any('update/no_such_key' in e for e in found) and any('nosuchfile.json' in e for e in found) and
                  len(other_used) > 10, found[:2]))

    clean = check(tree, (app_used, core_used, other_used))
    caught = 0
    for desc, ok, found in cases:
        print('%s  %s%s' % ('поймано ' if ok else 'ПРОПУЩЕНО', desc, '' if ok else '  ' + repr(found)))
        caught += 1 if ok else 0
    print('негативная проба: поймано %d из %d порч' % (caught, len(cases)))
    if clean:
        print('исходные переводы не чисты — сначала обычная проверка (%d находок)' % len(clean))
    return 0 if caught == len(cases) and not clean else 1


def main():
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding='utf-8')
        except Exception:
            pass
    if not os.path.isfile(os.path.join(I18N, 'languages.json')):
        print('нет файла resources/i18n/languages.json')
        return 2
    tree = load_tree()
    if '--self-test' in sys.argv:
        return self_test(tree)
    errs = check(tree, code_keys())
    for e in errs:
        print('ОШИБКА  ' + e)
    langs = [l.get('code') for l in tree['languages'].get('languages', [])]
    nkeys = sum(len(flatten(d)) for raw, d, err in tree['files'].get(BASE, {}).values() if d)
    if errs:
        print('i18n: %d ошибок (языки: %s)' % (len(errs), ', '.join(langs)))
        return 1
    print('i18n: OK — языки %s, ключей в %s: %d' % (', '.join(langs), BASE, nkeys))
    return 0


if __name__ == '__main__':
    sys.exit(main())
