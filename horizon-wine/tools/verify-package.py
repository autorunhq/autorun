#!/usr/bin/env python3
"""Check an Autorun package with the DLLs it runs with: the runtime, Autorun's
setup programs and settings, and what they need from the DLL repository.

    verify-package.py STAGE [--dlls DIR]

Autorun ships no Windows modules; horizon-dlls does, laid out as on the card
(DIR, default horizon-dlls/switch/wine). A file is looked for in the stage
first and then there, which is how the two meet on a card."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import platform
import re
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument("stage", type=Path)
parser.add_argument("--dlls", type=Path, default=root / "horizon-dlls/switch/wine")
args = parser.parse_args()
stage, dlls = args.stage.resolve(), args.dlls.resolve()
# The toolchain build.sh fetches: its release name is per-platform, and the
# build exports WINE_NX_LLVM_MINGW with the one it chose. Anything already
# fetched will do as a last resort.
llvm_version = "20260505"
machine = "aarch64" if platform.machine() in ("aarch64", "arm64") else "x86_64"
name = ("llvm-mingw-%s-ucrt-macos-universal" % llvm_version if platform.system() == "Darwin"
        else "llvm-mingw-%s-ucrt-ubuntu-22.04-%s" % (llvm_version, machine))
toolchain = (Path(os.environ["WINE_NX_LLVM_MINGW"]) / "bin" if os.environ.get("WINE_NX_LLVM_MINGW")
             else root / "horizon-wine/toolchains" / name / "bin")
if not (toolchain / "llvm-readobj").is_file():
    toolchain = next((p / "bin" for p in sorted((root / "horizon-wine/toolchains").glob("llvm-mingw-*"))
                      if (p / "bin" / "llvm-readobj").is_file()), toolchain)
assert (toolchain / "llvm-readobj").is_file(), \
    f"No llvm-mingw toolchain in {root / 'horizon-wine/toolchains'}; run build.sh, or set WINE_NX_LLVM_MINGW."
readobj = toolchain / "llvm-readobj"

def inspect(path, option):
    return subprocess.check_output([str(readobj), option, str(path)], text=True)

def card(path):
    """Where a file the card needs comes from: the stage, else the DLL repository."""
    return stage / path if (stage / path).exists() else dlls / path

assert (dlls / "horizon-dlls/manifest.json").is_file(), f"{dlls} is not the DLL repository's card tree"
# The Windows modules are the DLL repository's; the package brings none of its own.
for folder in ("drive_c/windows/system32", "drive_c/windows/syswow64", "drive_c/dxvk", "drive_c/dxvk64",
               "drive_c/vkd3d64"):
    if (stage / folder).is_dir():
        # One exception: the tuned FEX variants, which are the FEX tree's own
        # build and are fetched from its fex-opt release (horizon-wine/fex/
        # variants-build.md). They have no DLL repository release of their own.
        variant = re.compile(r"lib(arm64ec|wow64)fex-.+\.dll$")
        own = [p.name for p in (stage / folder).iterdir()
               if p.suffix.lower() in (".dll", ".drv", ".acm", ".ax", ".ocx", ".sys", ".cpl", ".tlb")
               and not variant.match(p.name)]
        assert not own, f"{folder} in the package holds modules the DLL repository provides: {own[:8]}"

wow64 = card("drive_c/windows/system32/wow64.dll")
assert "Name: __wine_switch_cpu_dll" in inspect(wow64, "--coff-exports"), "Missing Switch CPU selection export"
ntdll_exports = inspect(card("drive_c/windows/system32/ntdll.dll"), "--coff-exports")
for hook in ("pWow64PrepareForException",):
    # The bootstrap bypasses init_wow64() and fills this in the PE ntdll.
    assert f"Name: {hook}\n" in ntdll_exports, f"ntdll.dll lacks the WoW64 bootstrap hook {hook}"
schema = card("drive_c/windows/system32/apisetschema.dll")
# The runtime maps it at startup; without it no api-ms-win-* import resolves.
assert schema.exists() and "Name: .apiset " in inspect(schema, "--sections"), f"Missing API set schema: {schema}"
cpu = card("drive_c/windows/system32/winebox64.dll")
exports = set(re.findall(r"^  Name: (.+)$", inspect(cpu, "--coff-exports"), re.M))
required = {"BTCpuProcessInit", "BTCpuThreadInit", "BTCpuGetBopCode", "BTCpuSimulate",
            "BTCpuGetContext", "BTCpuSetContext", "BTCpuResetToConsistentState",
            "BTCpuSuspendLocalThread", "BTCpuIsProcessorFeaturePresent", "BTCpuUpdateProcessorInformation",
            "__wine_get_unix_opcode"}
assert required <= exports, f"Missing CPU exports: {required - exports}"
syswow64 = {p.name.lower() for p in (dlls / "drive_c/windows/syswow64").iterdir()}

# Autorun's own programs: relocatable i386, importing only what the DLL
# repository has, with the calls each is there to make.
setups = {
    # What the runtime runs before the first program on a card.
    "drive_c/windows/autorun-setup.exe": ("OleInitialize", "LoadLibraryExW", "RegSetValueExW", "NtDisplayString"),
}
for path, symbols in setups.items():
    info = inspect(stage / path, "--coff-imports")
    assert "Arch: i386\n" in info and "Type: HIGHLOW" in inspect(stage / path, "--coff-basereloc"), path
    for symbol in symbols:
        assert f"Symbol: {symbol} " in info, f"{path} does not import {symbol}"
    deps = re.findall(r"^Import \{\n  Name: (.+)$", info, re.M)
    assert deps and all(dep.lower() in syswow64 for dep in deps), f"{path} imports {deps}"
# What the components setup registers, and what WarCraft III's movies load through it.
assert {"quartz.dll", "devenum.dll", "msacm32.dll", "ddraw.dll", "dsound.dll", "d3d9.dll"} <= syswow64
for name in ("l3codeca.acm", "msacm32.drv"):
    assert card(f"drive_c/windows/syswow64/{name}").is_file(), f"{name} is not in the DLL repository"
for folder in ("drive_c/WarCraft III", "drive_c/The Sims 2"):
    assert (stage / folder).is_dir(), f"{folder} is where the player copies the game"

assert (stage / "wine-nx-runtime.nro").read_bytes()[16:20] == b"NRO0"
# target.txt only preselects a program, and the package has none to preselect.
if (stage / "target.txt").exists():
    target = (stage / "target.txt").read_text().strip()
    assert target.startswith("sdmc:/switch/wine/drive_c/") and (stage / target[len("sdmc:/switch/wine/"):]).is_file(), \
        f"target.txt names {target}, which the package does not hold"
assert not (stage / "args.txt").exists(), "args.txt would apply to whatever program it names"
# The game fixes the launcher knows of offline; the fixes themselves are
# downloaded, except any whose files the archive also holds under game-fixes/,
# which then apply with no network (the packer's game-fixes-extra/).
fixes = json.loads((stage / "game-fixes/bundled.json").read_text())
assert fixes["schema"] == 1 and fixes["serial"] >= 1 and fixes["fixes"], "game-fixes/bundled.json is not an index"
assert not (stage / "game-fixes/index.json").exists(), "index.json is the card's download; the archive ships bundled.json"
for fix in fixes["fixes"]:
    folder = stage / "game-fixes" / fix["id"]
    if not folder.is_dir():
        continue
    for file in fix["files"]:
        held = folder / file["name"]
        assert held.is_file(), f"{held} is named in the index but not in the archive"
        assert held.stat().st_size == file["size"], f"{held} is not the size the index says"
        assert hashlib.sha256(held.read_bytes()).hexdigest() == file["sha256"], f"{held} is not the file the index says"

# One settings file, where a dozen loose toggles were, and the keys beside it.
settings = json.loads((stage / "config/settings.json").read_text())
assert settings["run-the-chosen-program"] is True
assert settings["windows-through-opengl"] is True and settings["core-balancing"] is True
assert (stage / "config/keys.txt").exists()
for gone in ("run-entry.txt", "verbose.txt", "profile.txt", "framebuffer.txt", "no-balance.txt", "keys.txt"):
    assert not (stage / gone).exists(), f"{gone} is a setting now, not a file"
assert (stage / "share/wine/nls/locale.nls").exists()
for folder in ("drive_c/windows/fonts", "share/wine/fonts"):
    assert list((stage / folder).glob("*.ttf")), folder
print("Autorun package: no modules of its own, setups and CPU exports against the DLL repository, NRO and settings passed")
