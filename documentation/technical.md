# Autorun: how it works

Autorun (previously Wine-NX) is Wine for the Nintendo Switch. A homebrew NRO
runs Windows programs on Horizon through libnx: 32-bit x86 programs through
Wine's WoW64, x86-64 programs through Wine's ARM64EC loader, both with Box64
as the CPU backend, and ARM64 programs natively. The Wine server, memory
management, exceptions and the display and input drivers are reimplemented for
Horizon and run inside the same process.

The project was renamed; files and code identifiers still say `wine-nx`
(`wine-nx-runtime.nro`, `WINE_NX_RUNTIME_BUILD`). The runtime's log is
`logs/autorun_runtime.log`.

For players, see the [README](../README.md).

## Status

Verified on hardware:

| Program | State |
|---|---|
| 7-Zip `7zr.exe` | Test, add, extract, rename and the benchmark all complete. |
| Wine Notepad (x86) | Menus, dialogs, fonts and the controller cursor work. |
| OpenTTD 15.3 (x86) | Runs at up to 60 fps with OpenGL, with sound effects. |
| Quake III Arena (Quake3e, x86) | The demo runs at about 37 fps at 1280x720 with OpenGL. |
| WarCraft III (x86) | In game at 24-34 fps with its OpenGL renderer, menus at about 50 fps; the intro movie plays through DirectShow. |
| Need for Speed Underground 2 | In game with sound and the controller (NFSU-XtendedInput). With wined3d, races run at roughly 20-45 fps at default detail with the CPU overclocked. With DXVK, races load and run. |
| Halo: Combat Evolved | Plays at about 30 fps under a 32-bit (no alias) forwarder. |
| Left 4 Dead 2 | Reaches the main menu. |
| Direct3D 9 test | Draws and reads every frame back: about 55 fps through wined3d, 58 fps through DXVK. |
| Vulkan test | Instance, device, memory mapped into a 32-bit program, a surface and a swapchain; 180 frames at about 61 fps. |
| Win64 validation | AMD64 CPU, threads, audio, Vulkan and DXVK Direct3D 11 tests pass. |

In progress: The Sims 2 Legacy Collection starts, talks to its launcher
emulation over LSX and loads towards its title screen, then closes. The
Ultimate Collection has not been tested.

The OpenTTD, Quake III and WarCraft III figures were measured with the Mesa 20.1
runtime; the full package now ships the Mesa 26 one.

## What is in place

- **x86 and x86-64 execution.** `dlls/winebox64` is the WoW64 CPU DLL, while
  `dlls/winebox64ec` connects AMD64 Wine code to ARM64EC. Both use Box64's
  ARM64 dynarec with separate writable and executable code mappings, as Horizon
  requires, including Box64's call/return optimization. System calls and unix
  calls from x86 code are dispatched without leaving the emulator loop; their
  result goes in Eax as wow64cpu puts it. A fault in x86 code (access
  violation, division by zero, INT3, invalid opcode) is raised into the
  program's own exception handlers with the faulting instruction's registers.
- **Wine on Horizon.** An in-process Wine server
  (`dlls/ntdll/unix/horizon*.c|h`) covers files (with Windows sharing modes),
  directories, sync objects, threads and user APCs, the registry
  (`registry/system.reg` and `registry/user.reg` on the card, over the classes
  the DLL repository ships in `horizon-dlls/classes.reg`; hives an earlier build left in
  `switch/wine` itself are moved there on the first start), message
  queues, timers, the clipboard, raw input, object directories and sections,
  including sections with no file whose views share their pages.
- **Sockets.** Overlapped Winsock on I/O completion ports: pending
  `WSARecv`/`WSASend`, `AcceptEx`, `ConnectEx`, cancellation, and the
  completion rules of wineserver's `async_set_result`. Horizon has only IPv4:
  an AF_INET6 socket is IPv4 underneath, reaching `::`, `::ffff:a.b.c.d` and
  `::1`.
- **Graphics.** Windows are drawn as layers of one OpenGL compositor, or
  straight to the framebuffer. The runtime links Mesa 26 from
  [mesa-switch](https://github.com/danfromtico/mesa-switch): OpenGL through
  nvc0 and Vulkan through NVK, with Wine's winevulkan on top. Direct3D runs
  through wined3d on OpenGL, or through architecture-specific DXVK payloads on
  Vulkan for programs set to use it; frames of another size than the screen's
  (800x600 or 640x480 in full
  screen) are scaled to it, keeping the aspect ratio. The earlier runtime on
  Mesa 20.1's nouveau driver, patched for pinned 32-bit buffers
  (`horizon-wine/mesa`), can still be built.
- **Audio.** `winenxaudio.drv` plays through audout, for mmdevapi and DirectSound.
- **Input.** The touchscreen, the controller as a mouse or as a keyboard with
  per-game mappings, and XInput, which sees player 1 as an Xbox 360 controller.
  Mouse-driven games get a recommended layout when they are recognised
  (`source/games/controls.h`): Septerra Core and Harbinger play with the right
  stick moving the mouse, ZL clicking and the right stick's click right-clicking,
  with the keys each game reads beside them. The layout sits between the global
  controls and the player's own per-game changes, so clearing a row in the
  editor brings it back.
- **On-screen keyboard.** `source/osk.c` is a keyboard drawn over the program
  that sends virtual keys one at a time, with scan codes, through the display
  driver (Minus + right stick, or `NtUserShowSoftwareKeyboard` / a text field
  taking focus). Horizon's keyboard applet only returns a finished string, so
  fields a game draws itself could not be edited. Its picture is copied into
  the Vulkan swapchain image before present (`dlls/win32u/vulkan.c`), blitted
  into the back buffer before `eglSwapBuffers` (`winnx_opengl.c`), or drawn as
  the compositor's top layer; the controller is its while it is up.
- **Remapping while a program runs.** Minus and the left stick click open the
  controls overlay over the game (`source/controls_overlay.c`): the same rows
  the launcher's controls editor has, each opened into the key or mouse action
  it should make -- picked from a grid in keyboard/mouse mode, captured from
  the controller in controller mode. Every change is written to the program's
  own controls at once, so it can be tried the moment the overlay closes (B).
  X clears a row, Y puts the game's layout back under it.
- **Memory.** Fixed-base games such as NFSU2 need a 32-bit address space:
  launch the NRO through a forwarder made with "32-bit, no alias", which the
  launcher can install itself and which also raises the memory limit to 2 GiB.
  AMD64 programs use the main 39-bit application forwarder. The same NRO
  selects the execution path from the program's PE header.
  Wine reserves the program's low address space early, keeps its allocations
  clear of the memory Horizon hands to libnx, and leaves 32-bit programs'
  Vulkan memory in the driver's own mappings.
- **Launcher.** An SDL2 launcher with a Home and a Library view, per-game
  options, a controls editor, SteamGridDB artwork, and forwarder installation
  (`horizon-wine/source/launcher*.c`, `forwarder.c`). Add Game begins with an
  SD Card / USB picker; mounted USB volumes can then be browsed directly.
- **Diagnostics.** A thread and core report, a sampling profiler, fatal fault
  reports that name the x86 instruction behind translated code, and a map of
  the address space when a program runs out of it.

## Files on the card

Everything lives in `sdmc:/switch/wine`: the runtime `wine-nx-runtime.nro`,
Wine's files, and `drive_c` with the programs. C: is `drive_c`, Z: is the
card's root, and mounted USB volumes are D: through H:.

The Windows modules -- `drive_c/windows/system32` and `syswow64`, and the
bundled DXVK and VKD3D-Proton in `drive_c/dxvk`, `dxvk64` and `vkd3d64` -- come
from the DLL repository, not from a release. The launcher reads the
repository's manifest when it starts, and Settings > System > Windows DLLs
downloads what is new or changed (`source/launcher_dlls.c`, over
`source/horizon_dlls.c`): each file into a `.part` beside where it goes, checked
against the manifest's size and SHA-256, then put in place. Starting a game on
a card without them offers them first. The card keeps the manifest it
installed from in `horizon-dlls/manifest.json`, and `horizon-dlls/classes.reg`,
the COM classes its DLLs serve, which the registry loads before `system.reg`. A
file the runtime cannot run (a feature it does not report,
`tools/runtime_features.py`) is left as it is, and one the repository dropped
is removed unless it was changed since. A copy of the repository's `switch`
folder made by hand is an installation like any other. Verify reads every file
on the card and downloads again any that differs.

Runtime settings are one JSON object in `config/settings.json`. The launcher
writes most of them; the rest are for testing. Earlier builds kept each as a
file of its own in the `wine` folder, and those are moved into the JSON file
the first time a newer build starts.

| Key | Default | Meaning (the file it replaced) |
|---|---|---|
| `verbose-log` | false | Wine's traces in the log (`verbose.txt`) |
| `profiler` | false | Thread, server and sampling reports (`profile.txt`) |
| `windows-through-opengl` | true | Windows drawn by the compositor, not the framebuffer (`framebuffer.txt`, inverted) |
| `reopen-the-launcher-on-exit` | false | Start the launcher again when a program ends (`reload-launcher.txt`) |
| `core-balancing` | true | Move busy threads between cores (`no-balance.txt`, inverted) |
| `display-devices` | true | Report display devices (`no-display-devices.txt`, inverted) |
| `gl-pinned-buffers-cached` | true | Mesa 20.1 runtime: cached pinned buffers (`gl-uncached.txt`, inverted) |
| `gl-clean-before-submit` | true | Mesa 20.1 runtime (`gl-noclean.txt`, inverted) |
| `run-the-chosen-program` | false | Start `target.txt` directly, without the launcher (`run-entry.txt`) |
| `hand-the-process-back-anyway` | false | Return through the Homebrew loader on exit (`loader-anyway.txt`) |
| `vulkan-probe` | false | Vulkan self-test at startup (`vulkan-probe.txt`) |

The controls every program uses are in `config/keys.txt`. A game Autorun
recognises gets its recommended layout over them (`source/games/controls.h`),
and the program's own controls over that. A program's own files sit next to
its executable, named after it:

| File | Content |
|---|---|
| `NAME.wine-nx.txt` | Title, hidden from the library, verbose traces, profiler, `windows` (compositor or framebuffer), `d3d=dxvk`, address space, own controls |
| `NAME.args.txt` | Its command-line arguments |
| `NAME.keys.txt` | Its own controls, over `config/keys.txt` |
| `NAME.box64.txt` | Box64 code generation options, one `BOX64_DYNAREC_*=value` per line |

`d3d=dxvk` selects `C:\dxvk` for x86 programs and `C:\dxvk64` for AMD64
programs. Application-local graphics DLLs have priority. Existing
`d3d9=dxvk` settings remain readable. The launcher's look is kept in
`launcher.txt` and its library in `launcher-library-v2.ini`.

## Logs

Everything the runtime writes is in `switch/wine/logs`. Each run writes
`autorun_runtime.log`, and a copy named after the program: `NAME.log`, with
`_verbose` and `_profiler` added to the name when those were on, so a
diagnostic run keeps its own file. The program's standard handles are
`stdin.txt`, `stdout.txt` and `stderr.txt` there, and verbose runs add
`horizon-trace.log`. A log's first lines include `[BUILD]`, the runtime
version, worth
checking before reading anything else. `[PROGRESS]` lines report every 10
seconds: frames, OpenGL and system call rates, memory and translation counters.
Its reading figures tell a slow load apart from a busy one: `reads`/`read_mb`
are what the program asked for, `sd_reads`/`sd_mb` what the card was asked for
after the read cache, and `cache_mb` what that cache holds. The cache
(`source/sd_read_cache.h`) keeps up to eight pieces of each open file, and takes
between 32 and 192 MB depending on the heap a game leaves free; a write to a
file throws away what is held for it. A miss reads a whole 128 KB chunk; the
cache can also read ahead from 16 KB (`fill_min`), which on The Sims 2 halved
the bytes from the card but not the time. `code_mb` is the translated code the run
holds now over the code memory Horizon gave it, and `code_all_mb` every byte
ever translated: Horizon grants ten code memory objects in all, so translated
code that is never reused is what ends a long run in the interpreter
(`source/box64_code_arena.h`). On a 36- or 39-bit address space the code goes
above 4 GB, out of the program's way; on a 32-bit one it shares the runtime's
window and runs out near 150 MB. There `BOX64_DYNAREC_PURGE=1` in a game's
`NAME.box64.txt` lets blocks it has not entered for `BOX64_DYNAREC_PURGE_AGE`
translations give their room back, at a small cost on every block entry;
`purged=` counts the passes, blocks, megabytes and milliseconds. Freed code
waits two seconds before it is reused.
With the profiler on, `[THREADS]`, `[SERVER]` and `[PROF]` show where each busy
thread spends its time. DLLs that fail to load are logged (`[NXLDR]`, and
Wine's `err:` lines) even without verbose traces, and so are files a program
could not open (`[FS]`, with how it asked), the first overlapped socket
operations (`[ASYNC]`), user APCs (`[APC]`) and x86 faults (`[BOX64]`).

## Building

Requirements:

- Docker with the [switch-dev](https://github.com/autorunhq/switch-dev) image
  named in `horizon-wine/switch-dev.txt`, for the Switch build and the Box64
  tests. It is devkitA64 with a newer libnx, mesa-switch (Mesa 26: OpenGL and
  NVK Vulkan), LSFG-VK's Horizon port and libusbhsfs with UASP in portlibs,
  each from a pinned revision.
- LLVM-MinGW 20260505 in `horizon-wine/toolchains/`, fetched as the platform's
  release (`llvm-mingw-20260505-ucrt-macos-universal` on a Mac), and bison
  (Homebrew's), for Wine's PE modules.

Autorun builds the runtime; the Windows modules it runs are the DLL
repository's, [autorun-horizon-dlls](https://github.com/autorunhq/autorun-horizon-dlls),
a submodule at `horizon-dlls/`. That is all of system32 (ARM64X) and syswow64
(i386), FEX's CPU modules, the bundled DXVK and VKD3D-Proton and `classes.reg`,
one set for the x86 and AMD64 runtimes, laid out as on the card and grouped by
the part of Windows each file belongs to. No Autorun package carries any of it.

`sh build.sh` at the repository root does all of it, fetching what is missing
(`--help` lists its options). The steps it runs, from the repository root:

```sh
docker run --rm -v "$PWD:/work" -w /work -e DEVKITPRO=/opt/devkitpro-release "$(cat horizon-wine/switch-dev.txt)" \
    sh horizon-wine/build-boot-bundle.sh         # Atmosphere 1.11.2 and HOC boot payloads, once
WINE_NX_LLVM_MINGW="$PWD/horizon-wine/toolchains/llvm-mingw-20260505-ucrt-macos-universal" \
WINE_NX_FEX=1 WINE_NX_DXVK=1 WINE_NX_VKD3D=1 \
    sh horizon-wine/build-amd64-components.sh    # the AMD64 NRO, and the build-wine-amd64-pe tree
horizon-dlls/tools/build-dlls.py                  # the DLL repository: what changed since its manifest
python3 horizon-wine/tools/package-autorun.py    # the card: autorun-NNN.zip, with the AMD64 runtime
```

The runtime links mesa-switch, LSFG-VK and libusbhsfs from the image's portlibs;
a build records the image's revisions (`switch-dev.json` in the build folder)
in the package's `build-manifest.json`. `WINE_NX_MESA_SWITCH_DIR` links another
mesa-switch build instead. A change to Mesa, LSFG-VK's port or libusbhsfs's
UASP patches is made in switch-dev and comes with a new image.

`package-autorun.py` is what a card wants, with the DLL repository's `switch`
folder beside it: the runtime, Wine's NLS files and fonts, Autorun's setup
programs (the components setup, WarCraft III, The Sims 2, Guitar Hero III) and
the default settings, checked against that tree by `verify-package.py`. It
holds no test programs.

The AMD64 runtime runs 32-bit programs too, so it is the only one a card
needs. `build-x86.sh` builds the x86-only runtime (Box64, no FEX) against the
same Wine tree and Mesa, and `package-autorun.py --x86` ships it instead.

`build-dlls.py` builds in `horizon-wine/build-wine-amd64-pe` and rebuilds
only what changed since the commit its manifest names: a Wine module whose
sources, headers, import libraries or build tools changed (as Wine's make
knows them), and a file built from other sources whose recipe changed. After
changing `dlls/winebox64`, `horizon-wine/source/wow64_box64_bridge.c` (which
`winebox64` compiles in), `dlls/wow64` or anything else a module is built from,
commit it and run `build-dlls.py`; `--all` rebuilds everything, for a new
toolchain. Commit and push the DLL repository, then commit the new
`horizon-dlls` here.

The runtime's version is `WINE_NX_RUNTIME_BUILD` in
`horizon-wine/source/runtime.c`, and the archive is written to
`horizon-wine/build-autorun`.

## Tests

```sh
sh horizon-wine/check-runtime-console.sh      # host unit tests of runtime and server pieces, under ASan and UBSan
sh horizon-wine/check-box64-execution.sh      # Box64 interpreter and dynarec in an ARM64 container, plus their Switch build
sh horizon-wine/check-wow64-box64-bridge.sh   # the x86 system-call gate
sh horizon-wine/check-wow64-box64-unix.sh     # the native side of the CPU DLL
sh horizon-wine/check-amd64.sh                # AMD64, ARM64EC and both Box64 CPU modes
sh horizon-wine/check-audio.sh                # the audio driver
sh horizon-wine/tests/check-launcher-host.sh  # the launcher, headless, with scripted input (Homebrew's sdl2, sdl3, sdl2_ttf, libpng)
sh horizon-wine/tests/check-horizon-dlls.sh   # the DLL manager against the horizon-dlls checkout: install, resume, verify, update
sh horizon-wine/tests/check-game-fixes.sh     # game fixes against the game-fixes checkout: match, download, apply, record, update
for t in horizon-wine/tests/check_*.py; do python3 "$t"; done  # server pieces run against real host sockets and files
```

Box64 is pinned in `horizon-wine/vendor/box64` (fetched by
`tools/bootstrap-box64-core.sh`) and never edited:
`horizon-wine/cmake/Box64Core.cmake` builds patched copies of the files it
changes, and fails if the pinned text moves.

## Layout

| Path | Content |
|---|---|
| `dlls/ntdll/unix/horizon*` | Horizon server, memory, sections, sockets, exception handling |
| `dlls/win32u/winnx_drv.c`, `winnx_vulkan.c` | Display, input and Vulkan surface driver |
| `dlls/winebox64`, `dlls/winebox64ec` | x86 WoW64 and AMD64 ARM64EC CPU DLLs |
| `horizon-wine/source` | Runtime: startup, launcher, compositor, Box64 engine, profiler, audio and XInput backends, forwarder installer |
| `horizon-wine/tests` | Host, PE32 and PE32+ tests |
| `horizon-wine/tools` | Packagers and game setups |

## Limits

- Speed. Heavy Direct3D games are limited by translated x86 code on the game's
  main thread and by Wine's Direct3D layer, rather than by the GPU.
- Wine's first-run setup (wineboot) does not run. The DLL repository writes the COM
  classes its DLLs serve to `horizon-dlls/classes.reg`, and the runtime runs
  `C:\windows\autorun-setup.exe` (`tools/autorun_setup.c`) before the first
  program on a card: the MP3 decoder under Drivers32 and DllRegisterServer for
  the DirectShow and DMO DLLs, which lay out their own filter data. The
  program waits in `run-next.txt` and starts when the runtime starts again; the
  mark is `registry/components-1.done`, so a reset registry runs it again.
- A 32-bit address space leaves a program about 2 GiB of addresses and caps the
  whole process at 2 GiB of memory.
- AMD64 programs require the 39-bit application forwarder and remain
  experimental.
- One program at a time; one controller.
- Missing DLLs: the card only holds what earlier programs needed. The log names
  what is missing, and the overlay packager builds it.

## Credits

Autorun is built from these projects; each keeps its own copyright and license.

| Project | Authors | License | Used for |
|---|---|---|---|
| [Wine](https://www.winehq.org) | The Wine project authors | LGPL-2.1-or-later | The Windows API, loader, WoW64, and the Direct3D, OpenGL and Vulkan layers; this repository is a Wine fork |
| [Box64](https://github.com/ptitSeb/box64) | ptitSeb and contributors | MIT | x86 and x86-64 execution through its interpreter and ARM64 dynarec (`horizon-wine/vendor/box64`) |
| [DXVK](https://github.com/doitsujin/dxvk) | Philip Rebohle, Joshua Ashton, Robin Kertels, Jeffrey Ellison and contributors | zlib/libpng | Direct3D over Vulkan, for programs set to `d3d=dxvk` |
| [Mesa](https://mesa3d.org) | The Mesa authors | MIT (mostly) | OpenGL through nvc0 and Vulkan through NVK |
| [mesa-switch](https://github.com/danfromtico/mesa-switch) | danfromtico, NaGaa95 and contributors | Mesa's licenses | The Switch port of Mesa 26 (nvc0 and NVK) that the runtime links |
| Switch ports of Mesa 20.1 and libdrm_nouveau | fincs, Subv, Jules Blok | MIT | The earlier OpenGL path, from devkitPro's packages |
| [libnx](https://github.com/switchbrew/libnx) | switchbrew, libnx authors | ISC | The Horizon system library the runtime is written against |
| [devkitPro](https://devkitpro.org) | devkitPro | Per component | devkitA64 and the Switch builds of the libraries in this table |
| [SDL2 and SDL2_ttf](https://www.libsdl.org) | Sam Lantinga and contributors | zlib | The launcher's drawing, input and text |
| [FreeType](https://freetype.org) | The FreeType Project | FTL or GPL-2.0 | Font rendering in the launcher |
| [HarfBuzz](https://harfbuzz.github.io) | HarfBuzz authors | MIT | Text shaping in the launcher |
| [libpng](http://www.libpng.org), [zlib](https://zlib.net), [bzip2](https://sourceware.org/bzip2/) | Their authors | libpng, zlib, BSD-style | Program icons and compressed data |
| [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) | Martin Storsjö; LLVM and mingw-w64 authors | Apache-2.0 with LLVM exception, mingw-w64's licenses | Building Wine's and DXVK's Windows DLLs |
| [FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) | Advanced Micro Devices | MIT | The Upscaling setting's FSR 1.0 (EASU and RCAS, `tools/fsr1`), which isrmicha brought to the launcher |
| [dolphin-nx](https://github.com/NaGaa95/dolphin-nx) | NaGaa95 | GPL-2.0-or-later | The launcher's look (icon grid, program menu, settings and themes) follows its launcher; Autorun's launcher is its own code |

References that shaped the port without being part of the build:

- [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere): its kernel source is
  how Autorun learns what Horizon's memory calls allow.
- [tico-dolphin](https://github.com/ticohq/tico-dolphin): JIT and exception
  handling on Horizon.
- [WineBox64 NX](https://github.com/Ibnuard/winebox64_nx) by Ibnuard: a proof
  of concept that runs x86-64 Wine itself under Box64 on Horizon. Its Box64 and
  libnx integration was reference material for the execution core; Autorun
  instead runs Wine natively on ARM64, with Box64 as the WoW64 CPU.
- [sphaira](https://github.com/NaGaa95/sphaira) by ITotalJustice and NaGaa95:
  its forwarder code is what the launcher's forwarder installer is ported from.

## More

- [Build-by-build notes](../horizon-wine/README.md)
