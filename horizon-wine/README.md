# horizon-wine

The Switch side of Autorun: the Horizon program that Wine runs inside. Wine
itself stays in the repository root, with its Horizon changes in place
(`dlls/ntdll/unix/horizon*`, `dlls/win32u/winnx_*`, `dlls/winebox64*`). This
folder builds the NRO around it: startup, the launcher, the display compositor,
the x86 and AMD64 CPU engines, audio, input and USB storage, and everything
needed to build, test and package it.

The Windows DLLs are not built or shipped here. They live in the DLL
repository, [autorun-horizon-dlls](https://github.com/autorunhq/autorun-horizon-dlls),
the `horizon-dlls/` submodule. The launcher downloads them to the card, and a
program starts only once the ones it needs are there and match this runtime's
interfaces (`runtime-interfaces.json`).

Per-game fixes live in the game fixes repository,
[autorun-game-fixes](https://github.com/autorunhq/autorun-game-fixes), the
`game-fixes/` submodule. Examples are the registry keys an installer would have
written, a settings file a game's own launcher would make, or controls. The
launcher (`source/launcher_fixes.c`, `source/game_fixes.c`) recognises a game by
its executable and the files beside it. Before the game starts, it offers the
fix and says when the game may not start without it. Then it downloads and
checks the fix, applies it, and runs the fix's setup program ahead of the game
through `run-next.txt`. The archive ships the index as
`game-fixes/bundled.json`, so a card that is offline still knows which games
have a fix. The same index carries every game's settings profile (its settings
screen, settings file and launch arguments, read by `source/game_settings.c`):
the runtime has none of its own. Patches to a game's code as it loads
(`source/*_startup.h`) and Left 4 Dead's environment stay compiled. `tests/check-game-fixes.sh` tests all of this against the
submodule.

For how it all works, how to build it and what to run to test it, see
[documentation/technical.md](../documentation/technical.md). For players, see
the [README](../README.md).

## Runtimes

One source tree builds each runtime. The CMake options pick which:

| Runtime | Runs | Options |
|---|---|---|
| x86 (`nx-wow64-dynarec-*`) | 32-bit games through WoW64 and Box64, with OpenGL and Vulkan from mesa-switch | `WINE_NX_BOX64_DYNAREC`, `WINE_NX_MESA_SWITCH_DIR` |
| AMD64 (`nx-amd64-fex-*`) | 32- and 64-bit games through FEX (WoW64 and ARM64EC) | `WINE_NX_AMD64`, `WINE_NX_FEX` |
| AMD64 with nxvk (`wine-nx-runtime-nxvk.nro`) | The same, with nxvk's Mesa instead of mesa-switch: NVK for Vulkan, Zink for OpenGL, no nvc0 and no frame generation | `WINE_NX_NXVK_DIR` (`build.sh --nxvk`, `build-nxvk.sh`) |

A game's Graphics driver setting picks mesa-switch with OpenGL through nvc0 or
Zink, or nxvk. Two Mesas cannot be linked into one program, so nxvk is a second
runtime on the card: a game set to it is queued in `run-next.txt` and that NRO
is started for it, and the other way round (`source/runtime.c`,
`runtime_switch_graphics_runtime`). When the game ends, the nxvk runtime starts
the card's runtime for the launcher. Both run in the one process the loader
keeps, so whatever one leaves running goes on into the next: each closes USB
and its threads first, and a runtime that finds pages left lent inside its heap
keeps them out of it. The nxvk runtime writes `logs/autorun_runtime_nxvk.log`,
which the card's runtime does not write over.

NVK's image compression is off in the nxvk runtime and for Zink on mesa-switch
(`NVK_DEBUG=no_compression`): each compressed image is an allocation of its own
with compression tags of the GPU's, Zink makes one for every texture, and in
Doom 3 they ran out at about 2000. `nvk-compression` in `settings.json` turns it
back on.

The AMD64 runtime runs 32-bit programs too, so a card needs only it; the x86
one is optional. `tools/package-autorun.py` puts the runtime, Autorun's setup
programs and default settings into the release zip,
`build-autorun/autorun-NNN.zip`. The build number is `WINE_NX_RUNTIME_BUILD` in
`source/runtime.c`.

## Quick build

From the repository root, with the requirements in
[technical.md](../documentation/technical.md#building). The Switch build runs in
the [switch-dev](https://github.com/autorunhq/switch-dev) image named in
`switch-dev.txt`, which has libnx, mesa-switch, LSFG-VK and libusbhsfs built in.

```sh
sh horizon-wine/build-amd64-components.sh       # AMD64 runtime and the Wine tree (see technical.md for its environment)
python3 horizon-wine/tools/package-autorun.py   # autorun-NNN.zip

# Optional: the x86-only runtime, shipped instead with package-autorun.py --x86
sh horizon-wine/build-x86.sh
```

The build folders (`build-*`, `toolchains/`, `vendor/`) are not tracked.

## Layout

| Path | Content |
|---|---|
| `source/` | The runtime: `runtime.c` (startup), `launcher*.c` (the SDL launcher and its screens), `compositor*.c`, the Box64 and FEX engines, audio, XInput, USB storage, the forwarder installer, `horizon_dlls*.c` (the DLL manager), `game_fixes.c` (game fixes) and `autorun_update.c` (self-update) |
| `tests/` | Host tests of the runtime, the launcher and the Horizon server pieces |
| `tools/` | The packagers and their check, Autorun's components setup (`autorun_setup.c`), the Box64 bootstrap and `runtime_features.py`, which gives the features the DLL repository checks against |
| `cmake/` | The devkitA64 toolchain file, the Box64 core build and LSFG-VK |
| `fex/` | The Horizon patch for FEX and the ABI the runtime shares with it; FEX's DLLs are built in the DLL repository |
| `mesosphere/` | Patches for the Atmosphère/HOC boot payloads |
| `hbl/` | nx-hbloader, for game forwarders |
| `assets/` | Launcher art and icons |
| `box64-shims/`, `switch-shims/`, `syntax/` | Headers that let Box64 and Wine's code build against libnx |
| `build-*.sh`, `check-*.sh` | Build and test entry points |

## Launcher updates

Settings > System > Check for update reads the latest stable release of
`danfromtico/autorun`. A check at boot only shows a notification; installing
always asks first, and a development build newer than the release is not
downgraded.

The launcher checks the release zip's SHA-256, then extracts `switch/wine` over
the card. Configuration, games, saves, artwork and downloaded graphics versions
are kept; the runtime, fonts, NLS data and the NRO are replaced. An interrupted
install is rolled back before Wine loads, and backups stay in
`switch/wine/updates` until the new launcher opens. If the NRO no longer starts,
open `switch/wine/updates/previous.nro` from the Homebrew Menu.

The Windows DLLs update separately: Settings > System > Windows DLLs compares the card
with the DLL repository's manifest and downloads only what changed.

## History

This folder was `wine-nx-probe`, from when it was a probe that loaded Wine's
unix side on Horizon. Some CMake targets (`wine-nx-probe`, `wine-nx-pe-smoke`,
the ntdll smoke tests) and the `wine-nx-*` file names are from that time. The
per-build notes that used to be here are in the Git history.
