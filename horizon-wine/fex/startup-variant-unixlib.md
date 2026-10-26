# FEX `-opt` startup failure: static unixlib lookup

## Finding

The original Horizon static unixlib table in `dlls/ntdll/unix/virtual.c`
registered only `libwow64fex.dll` and `libarm64ecfex.dll`, matched exactly
(case-insensitively). Renaming the PE files for a variant does not change
FEX's unixlib ABI, but it **does** change the loader's `BaseDllName`, which
ntdll uses to find the static unixlib.

FEX's `Source/Windows/Common/FEXUnixLib.cpp` tries two methods:

| Method | Requested identity | Original result |
| --- | --- | --- |
| `MemoryWineLoadUnixLibByName` (1002) | `libwow64fex` / `libarm64ecfex`, **without `.dll`** | No match, including for stock |
| `MemoryWineUnixFuncs` (1000), with `&__ImageBase` | Loader entry's `BaseDllName` | Stock `.dll` matches; `-opt.dll` does not |

The fallback uses the native loader list, not the PE export-directory name.
The latter is used only in the separate x86/WoW64 unixlib path. The FEX CPU
DLL is a **native** module even when it translates x86.

## Why this fits the reported status and timing

The working `horizon.patch` contains this startup chain:

```
Wow64LdrpInitialize
  process_init
    BTCpuProcessInit
      InitCRTProcess
        HorizonInitialize
          UnixLib::Init    (both lookups miss for -opt)
        NtTerminateProcess(..., STATUS_DLL_INIT_FAILED)
```

`Source/Windows/Common/CRT/CRT.cpp` explicitly terminates with
`STATUS_DLL_INIT_FAILED` (`0xc0000142`) if `HorizonInitialize()` fails.
This runs **before** constructors, logging initialization, the config-file
access and all guest execution. `DllMainCRTStartup` itself returns true, so
successful native DLL load/attach breadcrumbs do not contradict this.

This is a source-level explanation of the reported failure, not a claim of a
successful Switch retest. The unixlib QUERY/exception-attachment step can
still fail for other reasons once name resolution succeeds.

The pointer breadcrumbs for `BTCpuProcessInit`, `BTCpuThreadInit` and
`BTCpuSimulate` in game-branch diagnostic commit `09c547e` precede their
`GET_PTR` assignments. Zero there does not establish a missing export; the
before/after-call breadcrumbs are the useful ones.

## Fix and regression coverage

FEX has a separate, feature-guarded static table matched by canonical stem.
The stem, stock `.dll` name and suffixed variant names resolve to the same
function table. Other unixlibs retain their exact-name rules. The returned
static module handle is stable and recognized by unload. Malformed byte
lengths and empty/invalid suffixes do not match.

Run on an ordinary Linux host:

```sh
CC=cc python3 horizon-wine/tests/check_fex_unixlib.py
```

This compiles the **actual tables and resolver/unload/module-list functions
extracted from `virtual.c`**, using Wine's real types. Only unixlib
implementations and the current TEB are stubbed. It checks stock and variant
names for both CPU-module architectures, the by-name and native module-base
paths, case folding, bounded non-NUL-terminated names, stable/unloadable
handles, unchanged non-FEX rules, and existing initializer failures. All four
combinations of FEX on/off and the other optional modules on/off run with
ASan/UBSan. `horizon_unixlib_names.c` also tests the matcher itself.

For a regression check, `--source /path/to/old/virtual.c` runs the same cases
against the old resolver. The original passes stock module lookup but fails
on the `-opt` loader name.

## What must be rebuilt and tested on hardware

`virtual.c` is compiled into `wine-virtual-real` and linked into the runtime
NRO. Rebuild and deploy the **runtime**, not just `ntdll.dll` or the `-opt`
CPU DLLs. The existing stock-twin `-opt` DLLs can remain unchanged for this
test; do not mix in a new FEX version or CPU tuning while isolating startup.

The game branch owns the working per-game selector and build workflow. Take
the FEX unixlib fix commits from `arena/21bcd47b-autorun` into that branch,
then rebuild its runtime through the existing workflow. No game-specific
changes or writes to that branch are needed from the FEX agent.

On the Switch, compare stock FEX and FEX optimized with the same title:

1. Confirm `[CPU]` names the intended module in each run.
2. With `-opt`, check that `[WOW64-DIAG] BTCpuProcessInit call` is followed by
   config-file activity and `BTCpuProcessInit returned`, then `cpu_simulate`.
3. Verify that startup reaches the game and no longer exits at the initial
   `0xc0000142` window. Recheck stock FEX as the control.
4. If startup still fails, keep `stderr.txt` and the runtime log from the new
   build. Missing static FEX mappings are now named in the Wine error log;
   distinguish that from a later QUERY/exception-attachment failure.

The incomplete FEX-2610 fallback memory layer is a separate problem and is
not repaired by this name-lookup change.
