#!/usr/bin/env python3
"""Carafe's texts for the settings menu, in its fifteen languages, as data.

Carafe writes its console menu's catalog from two things in its repository:
the list of settings (app/crates/carafe-core/src/settings_catalog.rs) and the
app's translation files (app/ui/src/shared/i18n/locales/*.json). This reads
both from a checkout of Carafe and writes what Autorun's menu can use to
horizon-wine/tools/carafe-menu-texts.json, keyed by what a setting is here:
the file and key it is stored under and the value's text in that file.
horizon-wine/tools/make-carafe-menu.py puts the texts into the menu's catalog.

    menu_texts.py CARAFE_CHECKOUT            write the file
    menu_texts.py CARAFE_CHECKOUT --check    change nothing; fail when it differs

The texts are Carafe's (GPL-3.0-or-later), as the file says in its first lines.
"""
import json
import re
import subprocess
import sys
from pathlib import Path

LANGUAGES = ['ru', 'en', 'zh-Hans', 'ja', 'ko', 'zh-Hant', 'fr', 'fr-CA', 'de', 'it', 'es', 'es-419', 'nl',
             'pt', 'pt-BR']
OUTPUT = Path(__file__).resolve().parents[3] / 'horizon-wine/tools/carafe-menu-texts.json'


class Setting:
    def __init__(self, id, file, key, apply, values, default):
        self.id, self.file, self.key, self.values, self.default = id, file, key, values, default
        self.page = None

    def typed(self):
        return self

    def on(self, setting, values):
        return self

    def apis(self, apis):
        return self


def function_body(source, name):
    start = re.search(r'^fn %s\([^)]*\)[^{]*\{\n' % re.escape(name), source, re.M)
    if not start:
        sys.exit('no fn %s in settings_catalog.rs' % name)
    return source[start.end():source.index('\n}\n', start.end())]


def python_of(rust):
    """A piece of the catalog's Rust, which is calls and lists, as a Python expression."""
    text = re.sub(r'^\s*use [^;]*;\n', '', rust, flags=re.M)
    text = text.replace('vec![', '[').replace('Vec::new()', '[]')
    text = text.replace('Setting::new(', 'Setting(')
    text = text.replace('SettingFile::Program', 'Program').replace('Apply::Restart', 'Restart')
    text = re.sub(r'&?format!\("((?:[^"\\]|\\.)*)"\)', r'f"\1"', text)
    text = re.sub(r'&(\w+)\.replace\(', r'\1.replace(', text)
    text = re.sub(r'&(?=[\[A-Z])', '', text)
    # [..].iter().map(|&n| expression).collect()
    text = re.sub(r'(\[[^\[\]]*\]|\b\w+)\s*\.iter\(\)\s*\.map\(\|&(\w+)\|\s*(.*?)\)\s*\.collect\(\)',
                  r'[\3 for \2 in \1]', text, flags=re.S)
    # { let mut values = A; values.extend(B); values }
    text = re.sub(r'\{?\s*let mut (\w+) = (.*?);\s*\1\.extend\((.*?)\);\s*(?:\1\s*\}?\s*$|\1\s*\})',
                  r'(\2 + \3)', text, flags=re.S)
    return re.sub(r'\bis\(', 'is_(', text.strip())


def read_catalog(carafe):
    """Carafe's settings: each with its file, key and values, a value being (text in the file or None, label)."""
    source = (carafe / 'app/crates/carafe-core/src/settings_catalog.rs').read_text(encoding='utf-8')
    env = {'Setting': Setting, 'Program': 'program', 'Keys': 'keys', 'Now': 'now', 'Restart': 'restart',
           'D3D9': 0, 'D3D11': 0, 'DXGI': 0}
    env['key'] = lambda key: ('key', key)
    env['text'] = lambda text: ('text', text)
    env['unit'] = lambda key, value: ('unit', key, value)
    env['word'] = lambda name: ('key', 'catalog.values.' + name)
    env['absent'] = lambda label: (None, label)
    env['is_'] = lambda text, label: (text, label)
    env['page'] = lambda id, settings: (id, settings)
    env['preset'] = lambda id, values: (id, values)
    env['fex'] = lambda id, key, values, default: Setting(id, 'program', key, 'restart', values, default)
    env['dxvk'] = lambda name, values: Setting(name.replace('.', '-'), 'program', name, 'restart',
                                              [(None, ('key', 'catalog.values.default'))] + values, 0)

    def run(expression):
        return eval(expression, env)

    def constant(name):
        return run(python_of(re.search(r'^const %s: [^=]*= (\[.*?\]);' % name, source, re.M | re.S).group(1)))

    env['DEADZONES'], env['MEMORY'] = constant('DEADZONES'), constant('MEMORY')
    for name, arguments in (('plain', 'texts'), ('units', 'unit_key, texts'), ('quoted', 'texts'),
                            ('off_on', ''), ('on_off', ''), ('dxvk_bool', ''), ('dxvk_tri', ''),
                            ('dxvk_threads', ''), ('dxvk_memory', ''), ('dxvk_latency', ''),
                            ('dxvk_anisotropy', ''), ('dxvk_lod', ''), ('dxvk_refresh', '')):
        env[name] = run('lambda %s: %s' % (arguments, python_of(function_body(source, name))))

    settings = []
    for name in ('controls', 'experimental'):
        settings += run(python_of(function_body(source, name)))
    graphics = function_body(source, 'graphics')
    limits = re.search(r'let mut frame_limits = (.*?);\s*frame_limits\.extend\((.*?)\);\n', graphics, re.S)
    env['frame_limits'] = run(python_of(limits.group(1)) + ' + ' + python_of(limits.group(2)))
    settings += run(python_of(graphics[limits.end():]))
    for name in ('dxvk_pages', 'fex_pages'):
        for id, members in run(python_of(function_body(source, name))):
            for setting in members:
                setting.page = id
            settings += members
    rows = re.search(r'entries\.extend\(rows\((.*?)\)\);', function_body(source, 'system'), re.S)
    settings += run(python_of(rows.group(1)))
    presets = dict(run(python_of(function_body(source, 'fex_presets'))))
    suggestions = [(kind.lower(), setting, int(value)) for kind, setting, value in
                   re.findall(r'suggestion\((\w+), "([^"]+)", (\d+)\)', function_body(source, 'suggestions'))]
    return settings, presets, suggestions, constant('MOUSE_ACTIONS')


def stored(text):
    """A value's text as Autorun's settings file holds it: DXVK's strings without their quotes."""
    return text[1:-1] if len(text) >= 2 and text[0] == text[-1] == '"' else text


def main():
    arguments = [a for a in sys.argv[1:] if a != '--check']
    check = len(arguments) != len(sys.argv[1:])
    if len(arguments) != 1:
        sys.exit(__doc__)
    carafe = Path(arguments[0])
    locales = {language: json.loads((carafe / 'app/ui/src/shared/i18n/locales' / (language + '.json'))
                                    .read_text(encoding='utf-8')) for language in LANGUAGES}
    settings, presets, suggestions, mouse = read_catalog(carafe)
    by_id = {setting.id: setting for setting in settings}
    if len(by_id) != len(settings):
        sys.exit('a setting is in the catalog twice')
    texts = {}

    def take(key):
        """Notes the fifteen texts of a translation key; every language must have one that fits a line."""
        found = []
        for language in LANGUAGES:
            node = locales[language]
            for part in key.split('.'):
                node = node.get(part) if isinstance(node, dict) else None
            if not isinstance(node, str) or not node.strip() or re.search(r'[\t\r\n]', node):
                sys.exit('no usable %s text for %s' % (language, key))
            found.append(node)
        texts[key] = found
        return key

    def label(spec):
        """A value's label: a key, a key with the number that goes into it, or None for a text no language changes."""
        if spec[0] == 'text':
            return None
        return take(spec[1]) if spec[0] == 'key' else [take(spec[1]), spec[2]]

    out_settings = {}
    for setting in settings:
        entry = {'id': setting.id, 'name': take('catalog.settings.%s.name' % setting.id),
                 'hint': take('catalog.settings.%s.hint' % setting.id), 'values': {}}
        if setting.page:
            entry['page'] = setting.page
        for text, spec in setting.values:
            if text is None:
                entry['absent'] = label(spec)
            elif label(spec) is not None:
                entry['values'][stored(text)] = label(spec)
        entry['default'] = setting.values[setting.default][0]
        if entry['default'] is not None:
            entry['default'] = stored(entry['default'])
        out_settings['%s %s' % (setting.file, setting.key)] = entry

    def name_of(id):
        return '%s %s' % (by_id[id].file, by_id[id].key)

    def value_of(id, index):
        text = by_id[id].values[index][0]
        return None if text is None else stored(text)

    english = locales['en']['catalog']
    data = {
        'from': 'Carafe (github.com/carafe-nx/carafe): app/ui/src/shared/i18n/locales and '
                'app/crates/carafe-core/src/settings_catalog.rs. Written by '
                'carafe/integration/tools/menu_texts.py; do not edit by hand.',
        'commit': subprocess.run(['git', '-C', str(carafe), 'rev-parse', 'HEAD'], check=True,
                                 stdout=subprocess.PIPE, text=True).stdout.strip(),
        'license': 'GPL-3.0-or-later, as Carafe is (its README and LICENSE).',
        'languages': LANGUAGES,
        'default': take('catalog.values.default'),
        'sections': {id: take('catalog.sections.' + id) for id in english['sections']},
        'groups': {id: take('catalog.groups.' + id) for id in english['groups']},
        'pages': {id: take('catalog.pages.' + id) for id in english['pages']},
        'presets': {id: {'name': take('catalog.presets.%s.name' % id), 'hint': take('catalog.presets.%s.hint' % id),
                         'sets': {name_of(setting): value_of(setting, index) for setting, index in values}}
                    for id, values in presets.items()},
        'suggest': [{'kind': kind, 'setting': name_of(setting), 'value': value_of(setting, index),
                     'why': take('catalog.suggest.%s.%s' % (kind, setting))} for kind, setting, index in suggestions],
        'pads': [take('pad.t%d' % number) for number in range(1, 25)],
        'mouse': [take('mouse.' + action) for action in mouse],
        'settings': out_settings,
        'texts': dict(sorted(texts.items())),
    }
    lines = ['{']
    for index, (key, value) in enumerate(data.items()):
        comma = '' if index == len(data) - 1 else ','
        if key in ('settings', 'texts', 'presets'):
            lines.append(' %s: {' % json.dumps(key))
            lines += ['  %s: %s%s' % (json.dumps(k, ensure_ascii=False), json.dumps(v, ensure_ascii=False),
                                      '' if i == len(value) - 1 else ',') for i, (k, v) in enumerate(value.items())]
            lines.append(' }' + comma)
        else:
            lines.append(' %s: %s%s' % (json.dumps(key), json.dumps(value, ensure_ascii=False), comma))
    written = '\n'.join(lines + ['}']) + '\n'
    json.loads(written)
    if check:
        if not OUTPUT.exists() or OUTPUT.read_text(encoding='utf-8') != written:
            sys.exit('%s is not what this Carafe checkout gives' % OUTPUT)
        print('%s is up to date' % OUTPUT.name)
        return
    OUTPUT.write_text(written, encoding='utf-8')
    print('%s: %d settings, %d texts in %d languages' % (OUTPUT.name, len(out_settings), len(texts), len(LANGUAGES)))


if __name__ == '__main__':
    main()
