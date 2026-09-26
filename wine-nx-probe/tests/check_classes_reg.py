#!/usr/bin/env python3
"""The COM classes the staged DLLs serve (tools/make-classes-reg.py), read by
the registry server before a program asks for one.

Nothing here runs a DLL's DllRegisterServer, which is what writes these on
Windows, so a program that asked for a class got REGDB_E_CLASSNOTREG and used
the null pointer it did not check: Fallout New Vegas creating its filter graph
stopped exactly there."""
from pathlib import Path
import importlib.util
import re
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('mk', root / 'wine-nx-probe/tools/make-classes-reg.py')
mk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mk)

# The server reads it before system.reg, so a program's own writes still win.
server = (root / 'dlls/ntdll/unix/horizon_registry_server.h').read_text()
assert (server.index('horizon_registry_load_hive( machine, "config/classes.reg" )') <
        server.index('horizon_registry_load_hive( machine, HORIZON_REGISTRY_SUBDIR "system.reg" )'))

# quartz serves the filter graph, which is the class Fallout asks for.
classes = dict((uuid, name) for uuid, _, name in mk.classes_of('quartz'))
assert classes.get('e436ebb3-524f-11ce-9f53-0020af0ba770') == 'FilterGraph'
classes = dict((uuid, name) for uuid, _, name in mk.classes_of('wbemprox'))
assert classes.get('4590f811-1d3a-11d0-891f-00aa004b2e24') == 'WbemLocator'
classes = dict((uuid, name) for uuid, _, name in mk.classes_of('gameux'))
assert classes.get('9a5ea990-3034-4d6f-9128-01f3c61022bc') == 'GameExplorer'
classes = dict((uuid, name) for uuid, _, name in mk.classes_of('netprofm'))
assert classes.get('dcb00c01-570f-4a9b-8d69-199fdba5723b') == 'NetworkListManager'
classes = dict((uuid, name) for uuid, _, name in mk.classes_of('msctf'))
assert classes.get('33c53a50-f456-4884-b049-85fd643ecfed') == 'TF_InputProcessorProfiles'
classes = dict((uuid, name) for uuid, _, name in mk.classes_of('explorerframe'))
assert classes.get('56fdf344-fd6d-11d0-958a-006097c9a090') == 'TaskbarList'
classes = dict((uuid, name) for uuid, _, name in mk.classes_of('msxml3'))
assert classes.get('f5078f32-c551-11d3-89b9-0000f81fe221') == 'DOMDocument30'
assert classes.get('88d969c0-f192-11d4-a65f-0040963251e5') == 'DOMDocument40'
assert classes.get('88d96a05-f192-11d4-a65f-0040963251e5') == 'DOMDocument60'
classes = dict((uuid, name) for uuid, _, name in mk.classes_of('xaudio2_5'))
assert classes == {
    '4c9b6dde-6809-46e6-a278-9b6a97588670': 'XAudio2',
    '2139e6da-c341-4774-9ac3-b4e026347f64': 'AudioVolumeMeter',
    'd06df0d0-8518-441e-822f-5451d5c595b8': 'AudioReverb',
}
classes = dict((uuid, name) for uuid, _, name in mk.classes_of('xaudio2_0'))
assert classes.get('fac23f48-31f5-45a8-b49b-5225d61401aa') == 'XAudio2'
assert classes.get('6f6ea3a9-2cf5-41cf-91c1-2170b1540063') == 'AudioReverb'
classes = dict((uuid, name) for uuid, _, name in mk.classes_of('xaudio2_7'))
assert classes.get('5a508685-a254-4fba-9b82-9a24b00306af') == 'XAudio2'
# and a DLL that cannot serve a class is not asked to.
assert mk.classes_of('kernel32') == []
assert mk.classes_of('not-a-dll') == []

with tempfile.TemporaryDirectory() as tmp:
    stage = Path(tmp)
    count = mk.write(stage, ['quartz', 'devenum', 'combase', 'kernel32', 'wbemprox'])
    text = (stage / 'config/classes.reg').read_text()
    assert text.startswith('WINE REGISTRY Version 2\n')
    assert count > 20
    # The shape the registry parser reads: a key line, then its values.
    entry = ('[Software\\\\Classes\\\\CLSID\\\\{e436ebb3-524f-11ce-9f53-0020af0ba770}\\\\InprocServer32]\n'
             '@="quartz.dll"\n"ThreadingModel"="Both"')
    assert entry in text, text[:400]
    entry = ('[Software\\\\Classes\\\\CLSID\\\\{4590f811-1d3a-11d0-891f-00aa004b2e24}\\\\InprocServer32]\n'
             '@="wbemprox.dll"\n"ThreadingModel"="Both"')
    assert entry in text, text[:400]
    # Every line is one the parser knows: a key, a value, or a comment.
    for line in text.splitlines():
        assert not line or line[0] in '[@";#' or line == 'WINE REGISTRY Version 2', line
    # No class named twice, whichever DLL claimed it first.
    uuids = re.findall(r'CLSID\\\\\{([0-9a-f-]{36})\}', text)
    assert len(uuids) == len(set(uuids)) == count

print(f'classes.reg: the filter graph among {count} classes, in the shape the server reads')
