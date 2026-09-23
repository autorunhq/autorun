#!/usr/bin/env python3
"""Check architecture and direct dependency closure of the WoW64 test package."""
from pathlib import Path
import hashlib
import re
import subprocess
import json
import sys

root = Path(__file__).resolve().parents[2]
stage = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "wine-nx-probe/build-switch-wow64/sd-card/switch/wine"
readobj = root / "wine-nx-probe/toolchains/llvm-mingw-20260505-ucrt-macos-universal/bin/llvm-readobj"

def inspect(path, option):
    return subprocess.check_output([str(readobj), option, str(path)], text=True)

for directory, arch in (("system32", "aarch64"), ("syswow64", "i386")):
    path = stage / "drive_c/windows" / directory
    modules = {p.name.lower(): p for p in path.iterdir() if p.suffix.lower() in (".dll", ".drv")}
    assert modules, f"No modules in {path}"
    for name, module in modules.items():
        info = inspect(module, "--coff-imports")
        assert f"Arch: {arch}\n" in info, f"Wrong architecture: {module}"
        # Load-time imports only; delay-loaded DLLs are resolved on first use.
        deps = re.findall(r"^Import \{\n  Name: (.+)$", info, re.M)
        missing = [dep for dep in deps if dep.lower() not in modules]
        assert not missing, f"Missing load-time dependency for {module}: {missing}"

wow64 = stage / "drive_c/windows/system32/wow64.dll"
assert "Name: __wine_switch_cpu_backend" in inspect(wow64, "--coff-exports"), "Missing Switch CPU selection export"
ntdll_exports = inspect(stage / "drive_c/windows/system32/ntdll.dll", "--coff-exports")
for hook in ("pWow64SuspendLocalThread", "pWow64PrepareForException"):
    # The bootstrap bypasses init_wow64() and fills these in the PE ntdll.
    assert f"Name: {hook}\n" in ntdll_exports, f"ntdll.dll lacks the WoW64 bootstrap hook {hook}"
schema = stage / "drive_c/windows/system32/apisetschema.dll"
# The runtime maps it at startup; without it no api-ms-win-* import resolves.
assert schema.exists() and "Name: .apiset " in inspect(schema, "--sections"), f"Missing API set schema: {schema}"
cpu = stage / "drive_c/windows/system32/winebox64.dll"
exports = set(re.findall(r"^  Name: (.+)$", inspect(cpu, "--coff-exports"), re.M))
required = {"BTCpuProcessInit", "BTCpuThreadInit", "BTCpuGetBopCode", "BTCpuSimulate",
            "BTCpuGetContext", "BTCpuSetContext", "BTCpuResetToConsistentState",
            "BTCpuSuspendLocalThread", "BTCpuIsProcessorFeaturePresent", "BTCpuUpdateProcessorInformation",
            "__wine_get_unix_opcode"}
assert required <= exports, f"Missing CPU exports: {required - exports}"
smoke = stage / "drive_c/pe32-smoke.exe"
info = inspect(smoke, "--coff-imports")
assert "Arch: i386\n" in info and "Symbol: NtQuerySystemTime" in info and "Symbol: NtTerminateProcess" in info
assert "Type: HIGHLOW" in inspect(smoke, "--coff-basereloc"), "Smoke must be relocatable"
functional = stage / "drive_c/pe32-functional.exe"
info = inspect(functional, "--coff-imports")
assert "Arch: i386\n" in info
for symbol in ("CreateFileW", "ReadFile", "WriteFile", "HeapAlloc", "HeapReAlloc", "TlsAlloc", "TlsSetValue", "TlsGetValue", "NtDisplayString", "NtTerminateProcess"):
    assert f"Symbol: {symbol} " in info, f"Missing functional test import: {symbol}"
assert "Type: HIGHLOW" in inspect(functional, "--coff-basereloc")
assert all(name.lower() in {"kernel32.dll", "ntdll.dll"} for name in re.findall(r"^  Name: (.+)$", info, re.M))
thread_info = inspect(stage / "drive_c/pe32-threads.exe", "--coff-imports")
assert "Arch: i386\n" in thread_info
for symbol in ("CreateThread", "CreateEventW", "SetEvent", "WaitForSingleObject", "CreateMutexW", "ReleaseMutex", "TlsGetValue"):
    assert f"Symbol: {symbol} " in thread_info, f"Missing thread test import: {symbol}"
assert "Type: HIGHLOW" in inspect(stage / "drive_c/pe32-threads.exe", "--coff-basereloc")
lifecycle = stage / "drive_c/pe32-lifecycle.exe"
lifecycle_info = inspect(lifecycle, "--coff-imports")
assert "Arch: i386\n" in lifecycle_info
for symbol in ("CreateThread", "ExitThread", "GetExitCodeThread", "WaitForMultipleObjects", "OpenThread",
               "ResumeThread", "SuspendThread", "InitOnceExecuteOnce", "SleepConditionVariableSRW",
               "EnterCriticalSection", "CreateSemaphoreW", "DuplicateHandle", "GetThreadTimes"):
    assert f"Symbol: {symbol} " in lifecycle_info, f"Missing lifecycle test import: {symbol}"
assert all(name.lower() in {"kernel32.dll", "ntdll.dll"}
           for name in re.findall(r"^  Name: (.+)$", lifecycle_info, re.M))
assert "Type: HIGHLOW" in inspect(lifecycle, "--coff-basereloc")
tls = inspect(lifecycle, "--coff-tls-directory")
assert "AddressOfCallBacks: 0x0" not in tls and "StartAddressOfRawData" in tls, "Lifecycle test needs static TLS"
timers = stage / "drive_c/pe32-timers.exe"
timers_info = inspect(timers, "--coff-imports")
assert "Arch: i386\n" in timers_info and "Type: HIGHLOW" in inspect(timers, "--coff-basereloc")
for symbol in ("SetTimer", "KillTimer", "PeekMessageW", "DispatchMessageW", "MsgWaitForMultipleObjects", "CreateWindowExW"):
    assert f"Symbol: {symbol} " in timers_info, f"Missing timer test import: {symbol}"
assert {name.lower() for name in re.findall(r"^  Name: (.+)$", timers_info, re.M)} == {"user32.dll", "kernel32.dll", "ntdll.dll"}
messages = stage / "drive_c/pe32-messages.exe"
messages_info = inspect(messages, "--coff-imports")
assert "Arch: i386\n" in messages_info and "Type: HIGHLOW" in inspect(messages, "--coff-basereloc")
for symbol in ("SendMessageTimeoutW", "SendNotifyMessageW", "SendMessageCallbackW", "ReplyMessage",
               "MsgWaitForMultipleObjects", "GetQueueStatus", "PostThreadMessageW", "OpenClipboard",
               "SetClipboardData", "GetClipboardData", "RegisterClipboardFormatW", "RegisterWindowMessageW"):
    assert f"Symbol: {symbol} " in messages_info, f"Missing message test import: {symbol}"
sevenzip = stage / "drive_c/7zr.exe"
info = inspect(sevenzip, "--coff-imports")
assert "Arch: i386\n" in info and "Type: HIGHLOW" in inspect(sevenzip, "--coff-basereloc"), "7zr must be relocatable"
deps = re.findall(r"^Import \{\n  Name: (.+)$", info, re.M)
syswow64 = {p.name.lower() for p in (stage / "drive_c/windows/syswow64").glob("*.dll")}
# The components setup the runtime runs before the first program on a card.
components = stage / "drive_c/windows/autorun-setup.exe"
if components.is_file():
    info = inspect(components, "--coff-imports")
    assert "Arch: i386\n" in info
    for symbol in ("OleInitialize", "LoadLibraryExW", "RegSetValueExW", "NtDisplayString"):
        assert f"Symbol: {symbol} " in info, f"Missing components setup import: {symbol}"
    deps = re.findall(r"^Import \{\n  Name: (.+)$", info, re.M)
    assert deps and all(dep.lower() in syswow64 for dep in deps), deps
assert all(dep.lower() in syswow64 for dep in deps), f"7zr load-time imports not staged: {deps}"
assert (stage / "wine-nx-runtime.nro").read_bytes()[16:20] == b"NRO0"
target = (stage / "target.txt").read_text().strip()
if target == "sdmc:/switch/wine/drive_c/notepad.exe":
    info = inspect(stage / "drive_c/notepad.exe", "--coff-imports")
    assert "Arch: i386\n" in info
    deps = re.findall(r"^Import \{\n  Name: (.+)$", info, re.M)
    assert deps and all(dep.lower() in syswow64 for dep in deps), deps
    assert (stage / "args.txt").read_text().strip() == "C:\\notepad.exe C:\\notepad-test.txt"
    assert (stage / "drive_c/notepad-test.txt").is_file()
    for folder in ("drive_c/windows/fonts", "share/wine/fonts"):
        assert list((stage / folder).glob("*.ttf")), folder
elif target == "sdmc:/switch/wine/drive_c/pe32-audio.exe":
    audio = stage / "drive_c/pe32-audio.exe"
    info = inspect(audio, "--coff-imports")
    assert "Arch: i386\n" in info and "Type: HIGHLOW" in inspect(audio, "--coff-basereloc")
    for symbol in ("waveOutOpen", "waveOutWrite", "waveOutReset", "waveOutClose", "NtDisplayString"):
        assert f"Symbol: {symbol} " in info, symbol
    deps = re.findall(r"^Import \{\n  Name: (.+)$", info, re.M)
    assert all(dep.lower() in syswow64 for dep in deps), deps
    assert {"winmm.dll", "mmdevapi.dll", "avrt.dll"} <= syswow64
    driver = stage / "drive_c/windows/syswow64/winenxaudio.drv"
    assert "Name: WineNXAudioDriver" in inspect(driver, "--coff-exports")
    assert b"winenxaudio.drv\0" in driver.read_bytes(), "Static unixlib lookup requires the module name"
elif target == "sdmc:/switch/wine/drive_c/pe32-opengl.exe":
    opengl = stage / "drive_c/pe32-opengl.exe"
    info = inspect(opengl, "--coff-imports")
    assert "Arch: i386\n" in info and "Type: HIGHLOW" in inspect(opengl, "--coff-basereloc")
    for symbol in ("wglCreateContext", "wglMakeCurrent", "glClear", "glReadPixels", "SetPixelFormat",
                   "SwapBuffers", "NtDisplayString"):
        assert f"Symbol: {symbol} " in info, symbol
    deps = re.findall(r"^Import \{\n  Name: (.+)$", info, re.M)
    assert all(dep.lower() in syswow64 for dep in deps), deps
elif target == "sdmc:/switch/wine/drive_c/openttd/openttd.exe":
    game = stage / "drive_c/openttd"
    info = inspect(game / "openttd.exe", "--coff-imports")
    assert "Arch: i386\n" in info
    deps = re.findall(r"^Import \{\n  Name: (.+)$", info, re.M)
    assert deps and all(dep.lower() in syswow64 for dep in deps), deps
    assert list((game / "baseset").rglob("opengfx.obg")), "OpenGFX is missing"
    assert (game / "openttd.args.txt").read_text().startswith("-v "), "OpenTTD needs a video driver"
elif target == "sdmc:/switch/wine/drive_c/pe32-d3d9.exe":
    d3d9 = stage / "drive_c/pe32-d3d9.exe"
    info = inspect(d3d9, "--coff-imports")
    assert "Arch: i386\n" in info and "Type: HIGHLOW" in inspect(d3d9, "--coff-basereloc")
    for symbol in ("Direct3DCreate9", "CreateWindowExW", "NtDisplayString"):
        assert f"Symbol: {symbol} " in info, f"Missing Direct3D test import: {symbol}"
    deps = re.findall(r"^Import \{\n  Name: (.+)$", info, re.M)
    assert deps and all(dep.lower() in syswow64 for dep in deps), deps
    # d3d9 hands the work to wined3d, which draws with opengl32.
    assert {"wined3d.dll", "opengl32.dll"} <= syswow64
elif target == "sdmc:/switch/wine/drive_c/quake3/quake3e.exe":
    game = stage / "drive_c/quake3"
    info = inspect(game / "quake3e.exe", "--coff-imports")
    assert "Arch: i386\n" in info
    deps = re.findall(r"^Import \{\n  Name: (.+)$", info, re.M)
    assert deps and all(dep.lower() in syswow64 for dep in deps), deps
    # The engine loads these itself, so they are not in its import table.
    assert "opengl32.dll" in syswow64
    assert (game / "quake3e.args.txt").read_text().startswith("+set "), "Quake3e needs its command line"
elif target == "sdmc:/switch/wine/drive_c/WarCraft III Setup/war3-setup.exe":
    setup = stage / "drive_c/WarCraft III Setup/war3-setup.exe"
    info = inspect(setup, "--coff-imports")
    assert "Arch: i386\n" in info and "Type: HIGHLOW" in inspect(setup, "--coff-basereloc")
    for symbol in ("RegSetValueExW", "RegDeleteValueW", "NtDisplayString"):
        assert f"Symbol: {symbol} " in info, f"Missing setup import: {symbol}"
    deps = re.findall(r"^Import \{\n  Name: (.+)$", info, re.M)
    assert deps and all(dep.lower() in syswow64 for dep in deps), deps
    # What the components setup registers (autorun-setup.exe, staged with the
    # full package), and what WarCraft III's movies load through it.
    assert {"quartz.dll", "devenum.dll", "msacm32.dll", "ddraw.dll", "dsound.dll", "d3d9.dll"} <= syswow64
    for name in ("l3codeca.acm", "msacm32.drv"):
        assert (stage / "drive_c/windows/syswow64" / name).is_file(), f"{name} not staged"
    assert (stage / "drive_c/WarCraft III").is_dir()
else:
    assert target == "sdmc:/switch/wine/drive_c/7zr.exe"
    assert (stage / "args.txt").read_text().strip().lower() == "c:\\7zr.exe b 1 -mmt2 -md18"
assert (stage / "drive_c/7zr-rename.7z").read_bytes() == (stage / "drive_c/7zr-tree.7z").read_bytes(), \
    "The rename run starts from a copy of the tree archive"
assert not (stage / "drive_c/7zr-rename.7z.tmp").exists()
assert not (stage / "drive_c/no-such-archive.7z").exists(), "The error-path command needs a missing archive"
assert hashlib.sha256((stage / "drive_c/7zr-tree.7z").read_bytes()).hexdigest() == \
    "e477719f40d14d1d34127a14b3e9b657c596032ca3d6bd99dd3f3fc051bc8524", "Staged tree archive differs from the sample"
assert (stage / "drive_c/7zr-sample.7z").read_bytes()[:6] == b"7z\xbc\xaf\x27\x1c"
# The folder tree "7zr a" archives (and "7zr x" restores): the generator's files, byte for byte.
sys.path.insert(0, str(root / "wine-nx-probe/tools"))
tree_module = __import__("make-7zr-tree")
tree_root = stage / "drive_c/7zr-tree"
staged = sorted(p.relative_to(stage / "drive_c").as_posix() for p in tree_root.rglob("*") if p.is_file())
assert staged == sorted(name for name, _, _ in tree_module.TREE), f"Unexpected 7zr tree: {staged}"
assert sorted(p.name for p in tree_root.rglob("*") if p.is_dir()) == ["data", "text"]
for name, data, _ in tree_module.TREE:
    assert (stage / "drive_c" / name).read_bytes() == data, f"7zr tree file differs: {name}"
assert not (stage / "drive_c/wine-nx-tree.7z").exists(), "Staging an archive would replace the one 7zr made on the Switch"
assert not (stage / "drive_c/7zr-out").exists(), "7zr x must create its output folders itself"
# One settings file, where a dozen loose toggles were, and the keys beside it.
# The stages this one is built from are earlier steps and still carry the files.
if (stage / "config/settings.json").exists():
    settings = json.loads((stage / "config/settings.json").read_text())
    assert settings["run-the-chosen-program"] is True
    assert settings["windows-through-opengl"] is True and settings["core-balancing"] is True
    assert (stage / "config/keys.txt").exists()
    for gone in ("run-entry.txt", "verbose.txt", "profile.txt", "framebuffer.txt", "no-balance.txt",
                 "keys.txt"):
        assert not (stage / gone).exists(), f"{gone} is a setting now, not a file"
else:
    assert (stage / "run-entry.txt").read_text().strip() == "1"
assert (stage / "share/wine/nls/locale.nls").exists()
print("WoW64 package: architectures, dependency closure, CPU exports, relocatable PE32, NRO and launch files passed")
