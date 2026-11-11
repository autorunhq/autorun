#!/usr/bin/env python3
"""The settings menu in the console's fifteen languages.

tools/make-carafe-menu.py writes the menu's catalog: the settings and the
English are Autorun's, the other languages are Carafe's texts for the same
setting (tools/carafe-menu-texts.json). This builds the catalog, has the
menu's own reader (source/carafe_settings.c) show it under each console
language, and holds what comes out against Carafe's texts:

- the reader shows, text for text, what the catalog holds for the language;
- in English nothing is Carafe's wording: DXVK's options keep their names;
- in every other language a text is Autorun's English or one of Carafe's
  texts for that language, never a mix or a made-up one;
- a value is labelled with Carafe's word for the value it stands for, also
  where a row that writes nothing stands for a value Carafe writes, and where
  Carafe names an option the other way round (the L2 cache);
- what Carafe says that is not true here stays English.
"""
from pathlib import Path
import json
import os
import re
import subprocess
import sys
import tempfile

root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
wine = root / 'horizon-wine'
carafe = json.loads((wine / 'tools/carafe-menu-texts.json').read_text(encoding='utf-8'))
LANGUAGES = carafe['languages']
EN = LANGUAGES.index('en')
# The console's number for each language (libnx's SetLanguage, as the shim has it).
CONSOLE = {'en': 1, 'fr': 2, 'de': 3, 'it': 4, 'es': 5, 'zh-Hans': 15, 'ko': 7, 'nl': 8, 'pt': 9, 'ru': 10,
           'ja': 11, 'zh-Hant': 16, 'fr-CA': 13, 'es-419': 14, 'pt-BR': 17}
assert sorted(CONSOLE) == sorted(LANGUAGES) and len(LANGUAGES) == 15
assert 'GPL-3.0-or-later' in carafe['license'] and 'carafe' in carafe['from'].lower()

settings_c = (wine / 'source/carafe_settings.c').read_text()
codes = re.search(r'CODES\[CarafeLanguageCount\] = \{(.*?)\};', settings_c, re.S).group(1)
assert re.findall(r'"([^"]+)"', codes) == LANGUAGES, 'the reader and the texts list other languages'
limit = eval(re.search(r'#define MENU_LIMIT \(([\d *]+)\)', settings_c).group(1))

# How many text blocks each kind of line ends with, after how many plain fields.
SHAPE = {'section': (2, 1), 'group': (3, 1), 'page': (2, 1), 'setting': (8, 2), 'value': (2, 1),
         'preset': (2, 2), 'suggest': (4, 1), 'pad': (2, 1), 'key': (2, 1)}


def text(key, language, number=None):
    found = carafe['texts'][key][language]
    return found if number is None else found.replace('{{value}}', number)


def label(spec, language):
    return text(spec, language) if isinstance(spec, str) else text(spec[0], language, spec[1])


with tempfile.TemporaryDirectory() as temp:
    temp = Path(temp)
    menu = temp / 'menu.txt'
    subprocess.run([sys.executable, str(wine / 'tools/make-carafe-menu.py'), str(menu)], check=True)
    raw = menu.read_bytes()
    assert len(raw) < limit, 'the catalog is larger than the menu reads'
    lines = [line.split('\t') for line in raw.decode('utf-8').split('\n') if line]
    assert lines[0] == ['carafe-menu', '2'] and lines[1] == ['languages'] + LANGUAGES

    # The catalog as rows of (kind, identity, [texts by language]).
    rows, setting = [], None
    for fields in lines[2:]:
        kind = fields[0]
        if kind in ('end', 'set'):
            continue
        plain, blocks = SHAPE[kind]
        assert len(fields) == plain + 15 * blocks, fields[:3]
        assert all(part.strip() == part and '{{' not in part for part in fields[plain:]), fields[:3]
        texts = [fields[plain + 15 * block:plain + 15 * block + 15] for block in range(blocks)]
        assert all(block[EN] for block in texts), fields[:3]
        if kind == 'setting':
            setting = fields[3]
            rows.append((kind, (fields[2], fields[3], fields[1]), texts))
        elif kind == 'value':
            rows.append((kind, (setting, fields[1]), texts))
        elif kind == 'suggest':
            rows.append((kind, (fields[1], fields[2], fields[3]), texts))
        else:
            rows.append((kind, (fields[1],), texts))

    exe = temp / 'menu_languages'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-D_GNU_SOURCE', '-D__SWITCH__', '-Wall', '-Wextra',
                    '-Werror', '-g', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                    '-I', str(wine / 'source'), '-I', str(wine / 'tests/controls-shims'),
                    '-I', str(wine / 'tests/controls-shims/SDL2'), str(wine / 'tests/menu_languages.c'),
                    str(wine / 'source/input_profile.c'), str(wine / 'source/dxvk_options.c'), '-lm',
                    '-o', str(exe)], check=True)

    # The text a program's file would hold for what each "-" row stands for.
    UNWRITTEN = {'vsync': '1', 'upscaling': 'off', 'upscaling-sharpness': '40%', 'dxvk-hud': '0', 'lsfg': '0',
                 'lsfg-performance': '1', 'lsfg-flow': '0.25', 'large-pages': '0', 'frame-limit': '0'}

    def shown(language, probes=()):
        env = dict(os.environ, CONTROLS_TEST_LANGUAGE=str(CONSOLE[language]), ASAN_OPTIONS='detect_leaks=1')
        out = subprocess.run([str(exe), str(menu)] + list(probes), check=True, env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout.decode('utf-8')
        return [line.split('\t') for line in out.split('\n') if line]

    # What the reader shows is, text for text, the catalog's text for the language.
    fex_defaults = {}
    for index, language in enumerate(LANGUAGES):
        out = shown(language, ['%s=%s' % pair for pair in UNWRITTEN.items()] if language == 'en' else ())
        assert out[0] == ['language', str(index)], (language, out[0])
        seen = {}
        for fields in out[1:]:
            seen.setdefault(fields[0], []).append(fields[1:])
        for kind in SHAPE:
            mine = [texts for row_kind, _, texts in rows if row_kind == kind]
            theirs = seen[kind]
            assert len(mine) == len(theirs), (language, kind, len(mine), len(theirs))
            for texts, fields in zip(mine, theirs):
                want = [block[index] or block[EN] for block in texts]
                assert fields[-len(want):] == want, (language, kind, fields, want)
        if language == 'en':
            fex_defaults = dict(seen['fex'])
            for key, stored, row in seen['stored']:
                assert row == '-', 'a file holding %s=%s does not show the row that writes nothing' % (key, stored)

    by_kind = {}
    for kind, identity, texts in rows:
        by_kind.setdefault(kind, {})[identity] = texts
    settings = {(file, key): texts for (file, key, _), texts in by_kind['setting'].items()}
    ids = {(file, key): sid for (file, key, sid) in by_kind['setting']}
    values = by_kind['value']
    assert len(settings) == 141, len(settings)

    # English is Autorun's own: an option of DXVK is called what dxvk.conf calls it.
    dxvk = [key for (_, key) in settings if re.match(r'(dxvk|dxgi|d3d9|d3d11)\.', key)]
    assert len(dxvk) == 81 and all(settings[('program', key)][0][EN] == key for key in dxvk)
    assert values[('vsync', '-')][0][EN] == 'Enabled' and values[('FEX_TSOENABLED', '-')][0][EN] == '0 - Off (Default)'

    # Every text of another language is Autorun's English left as it is (an
    # empty field, which the reader shows in English) or one of Carafe's.
    for index, language in enumerate(LANGUAGES):
        if index == EN:
            continue
        known = {found[index] for found in carafe['texts'].values()}
        units = [re.compile(re.escape(found[index]).replace(re.escape('{{value}}'), r'-?[\d.]+') + r'\Z')
                 for found in carafe['texts'].values() if '{{value}}' in found[index]]
        for kind, identity, texts in rows:
            for block in texts:
                got = block[index]
                if not got or got in known or any(unit.match(got) for unit in units):
                    continue
                if kind == 'page' and all(part in known for part in got.split(' & ')):
                    continue
                raise AssertionError('%s %s: %r is not a text of Carafe' % (language, identity, got))

    # A setting Carafe has too carries Carafe's name and hint in all fourteen
    # languages; one it has not, and the one whose hint is not true here, none.
    english_only = {('program', 'dxvk.shaderCompilationMethod')}
    theirs = {tuple(name.split(' ', 1)): entry for name, entry in carafe['settings'].items()}
    assert len(theirs) == 92 and set(theirs) <= set(settings)
    translated = 0
    for name, (title, hint) in settings.items():
        entry = theirs.get(name) if name not in english_only else None
        for index in range(15):
            if index == EN:
                continue
            if entry is None:
                assert title[index] == hint[index] == '', (name, LANGUAGES[index])
            else:
                assert title[index] == text(entry['name'], index) and hint[index] == text(entry['hint'], index), name
        translated += entry is not None
    assert translated == 91, translated

    def expect(setting_key, stored, spec):
        """The value row shows Carafe's `spec` in every language but English, or nothing of Carafe's."""
        row = values[(setting_key, stored)][0]
        for index in range(15):
            if index != EN:
                want = '' if spec is None else label(spec, index)
                assert row[index] == want, (setting_key, stored, LANGUAGES[index], row[index], want)

    on, off = 'catalog.values.on', 'catalog.values.off'
    for setting_key, stored, spec in (
            # A row that writes nothing, standing for a value Carafe writes.
            ('vsync', '-', on), ('vsync', '0', off),
            ('upscaling', '-', 'graphics.upscalingOff'), ('upscaling', 'integer', 'graphics.upscalingInteger'),
            ('upscaling', 'fsr', None),
            ('upscaling-sharpness', '-', ['catalog.values.percent', '40']),
            ('upscaling-sharpness', '100%', ['catalog.values.percent', '100']),
            ('dxvk-hud', '-', 'graphics.hudOff'), ('dxvk-hud', 'fps', 'graphics.hudFps'),
            ('lsfg', '-', off), ('lsfg', '1', on), ('lsfg-performance', '-', on), ('lsfg-performance', '0', off),
            ('lsfg-flow', '-', 'catalog.values.flowQuarter'), ('lsfg-flow', '0.5', 'catalog.values.flowHalf'),
            ('large-pages', '-', off), ('large-pages', '1', on),
            ('frame-limit', '-', 'graphics.noLimit'), ('frame-limit', '30', None),
            ('keyboard-auto', '0', off), ('keyboard-auto', '1', on),
            ('input-mode', '1', 'input.controller'), ('input-mode', '2', 'input.keyboardMouse'),
            ('left-deadzone', '10', ['catalog.values.percent', '10']),
            # "Global setting" is not Carafe's "Automatic".
            ('windows', '-', None), ('windows', 'framebuffer', 'advanced.windowsFramebuffer'),
            ('verbose', '-', None), ('verbose', '1', on), ('profile', '-', None), ('profile', '0', off),
            # Carafe calls it the L2 cache, where 1 turns the cache off.
            ('FEX_DISABLEL2CACHE', '-', off), ('FEX_DISABLEL2CACHE', '0', on),
            ('FEX_TSOENABLED', '-', off), ('FEX_TSOENABLED', '1', on),
            ('FEX_X87REDUCEDPRECISION', '-', on), ('FEX_X87REDUCEDPRECISION', '0', off),
            ('FEX_SMCCHECKS', '-', 'catalog.values.none'), ('FEX_SMCCHECKS', 'mtrack', None),
            ('FEX_MAXINST', '-', None),
            ('dxvk.maxMemoryBudget', '0', 'catalog.values.auto'),
            ('dxvk.maxMemoryBudget', '128', ['catalog.values.mib', '128']),
            ('d3d9.floatEmulation', 'Strict', 'catalog.values.strict'),
            ('d3d11.samplerAnisotropy', '16', ['catalog.values.times', '16']),
            ('dxvk.shaderCompilationMethod', '-', None), ('dxvk.shaderCompilationMethod', '"none"', None)):
        expect(setting_key, stored, spec)
    assert theirs[('program', 'windows')]['values']['framebuffer'] == 'advanced.windowsFramebuffer'

    # The rule behind those: a row that writes what a value of Carafe's writes
    # (dxvk.conf's strings are in quotes here) carries Carafe's label for it.
    labelled = 0
    for (file, key), entry in theirs.items():
        if (file, key) in english_only:
            continue
        for stored, spec in entry['values'].items():
            for spelled in (stored, '"%s"' % stored):
                if (key, spelled) in values:
                    expect(key, spelled, spec)
                    labelled += 1
    assert labelled == 216, labelled
    expect('d3d11.cachedDynamicResources', '"v"', 'catalog.values.vertexBuffers')
    expect('d3d9.forceAspectRatio', '""', 'catalog.values.all')

    # Every option of FEX: the row that writes nothing is labelled as the
    # value FEX then uses, and a file holding that value shows that row.
    probes, checked = [], 0
    for key, default in fex_defaults.items():
        entry = theirs.get(('program', key))
        if entry is None:
            continue
        expect(key, '-', entry['values'].get(default))
        probes.append('%s=%s' % (key, default))
        checked += 1
    assert checked == 21, checked
    for key, stored, row in [fields[1:] for fields in shown('ja', probes) if fields[0] == 'stored']:
        assert row == '-', (key, stored, row)

    # DXVK's own default is Carafe's word for it. English also says what that
    # default is, for each DXVK build where the builds differ.
    expect('dxvk.maxMemoryBudget', '-', 'catalog.values.default')
    expect('d3d9.deviceLocalConstantBuffers', '-', 'catalog.values.default')
    assert carafe['default'] == 'catalog.values.default'
    assert values[('dxvk.maxMemoryBudget', '-')][0][EN] == 'Default (0 / Auto)'
    assert values[('d3d9.deviceLocalConstantBuffers', '-')][0][EN] == 'Default (False / Auto)'
    assert values[('dxvk.shaderCompilationMethod', '-')][0][EN] == 'Default (dyasync / async)'

    def row_is(kind, identity, block, spec):
        row = by_kind[kind][identity][block]
        for index in range(15):
            if index != EN:
                want = '' if spec is None else ' & '.join(text(key, index) for key in spec)
                assert row[index] == want, (kind, identity, LANGUAGES[index], row[index], want)

    for name in ('controls', 'graphics', 'system', 'experimental'):
        row_is('section', (name,), 0, ['catalog.sections.' + name])
    for name in ('dxvk', 'fex'):
        row_is('group', (name,), 0, ['catalog.groups.' + name])
    for page, names in (('memory', ['dxvk-memory']), ('frames', ['dxvk-frames']),
                        ('shaders', ['dxvk-shaders', 'dxvk-quality']), ('display', ['dxvk-display']),
                        ('compat', ['dxvk-fixes']), ('basic', ['fex-general']), ('precision', ['fex-memory']),
                        ('cache', ['fex-compat'])):
        row_is('page', (page,), 0, ['catalog.pages.' + name for name in names])
    for preset, hint in (('balanced', True), ('reliable', True), ('fast', False)):
        row_is('preset', (preset,), 0, ['catalog.presets.%s.name' % preset])
        # Carafe's Fast also stops sizing the L1 cache, and its hint says so.
        row_is('preset', (preset,), 1, ['catalog.presets.%s.hint' % preset] if hint else None)
    lines_of = {fields[1]: i for i, fields in enumerate(lines) if fields[0] == 'preset'}
    fast = [fields[1] for fields in lines[lines_of['fast'] + 1:lines_of['fast'] + 6] if fields[0] == 'set']
    assert 'fex-fex-dynamicl1cache' not in fast and 'fex-fex-maxinst' in fast

    # A suggestion carries Carafe's reason when Carafe suggests the same change.
    reasons = {(item['kind'], tuple(item['setting'].split(' ', 1))): item['why'] for item in carafe['suggest']}
    by_id = {sid: name for name, sid in ids.items()}
    found = 0
    for (kind, sid, _), (why,) in by_kind['suggest'].items():
        key = reasons.get((kind, by_id[sid]))
        found += key is not None
        for index in range(15):
            if index != EN:
                assert why[index] == (text(key, index) if key else ''), (kind, sid)
    assert found == 5 and len(by_kind['suggest']) == 11, found

    for number in range(1, 25):
        row_is('pad', ('0x%x' % number,), 0, [carafe['pads'][number - 1]])
        assert by_kind['pad'][('0x%x' % number,)][0][EN] == text(carafe['pads'][number - 1], EN)
    for code, (name,) in by_kind['key'].items():
        number = int(code[0], 16) - 0x100
        row_is('key', code, 0, [carafe['mouse'][number]] if 0 <= number < len(carafe['mouse']) else None)
    assert by_kind['key'][('0x100',)][0][EN] == 'Left click' and text(carafe['mouse'][0], EN) == 'Left mouse button'
    assert by_kind['key'][('0x10c',)][0][EN] == 'Mouse right' and text(carafe['mouse'][12], EN) == 'Cursor right'

print('settings menu: 141 settings, 91 of them and %d value rows in fifteen languages'
      % sum(1 for texts in values.values() if texts[0][0]))
