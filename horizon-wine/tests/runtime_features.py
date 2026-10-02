import importlib.util
from pathlib import Path

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("runtime_features", root / "tools/runtime_features.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
native, wow64 = module.static_unix_libs()
for name in ("wmadmod.dll", "winenxaudio.drv", "ws2_32.dll", "winedmo.dll", "winegstreamer.dll"):
    assert name in native and name in wow64, name
    assert f"unixlib:{name}" in module.features()
    assert f"unixlib32:{name}" in module.features()
assert module.interfaces()["wmadmod.dll"] in module.features()
print("Native and WoW64 DLL feature discovery passed")
