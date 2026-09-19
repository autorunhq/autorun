# Autorun: how it works

Autorun (previously Wine-NX) is Wine for the Nintendo Switch. A homebrew NRO
runs Windows programs on Horizon through libnx: 32-bit x86 programs through
Wine's WoW64 with Box64 as the CPU backend, and ARM64 programs natively. The
Wine server, memory management, exceptions and the display and input drivers
are reimplemented for Horizon and run inside the same process.

The project was renamed; files, logs and code identifiers still say `wine-nx`
(`wine-nx-runtime.nro`, `wine-nx-runtime.log`, `WINE_NX_RUNTIME_BUILD`).

For players, see the [README](../README.md).

## Status

Verified on hardware:

| Program | State |
|---|---|
| 7-Zip `7zr.exe` | Test, add, extract, rename and the benchmark all complete. |
| Wine Notepad (x86) | Menus, dialogs, fonts and the controller cursor work. |
| OpenTTD 15.3 (x86) | Runs at up to 60 fps with OpenGL, with sound effects. |
| Quake III Arena (Quake3e, x86) | The demo runs at about 37 fps at 1280x720 with OpenGL. |
| WarCraft III (x86) | In game at 24–34 fps with its OpenGL renderer, menus at about 50 fps; the intro movie plays through DirectShow. |
| Need for Speed Underground 2 | In game with sound and the controller (NFSU-XtendedInput). With wined3d, races run at roughly 20–45 fps at default detail with the CPU overclocked. With DXVK, races load and run. |
| Halo: Combat Evolved | Plays at about 30 fps under a 32-bit (no alias) forwarder. |
| Left 4 Dead 2 | Reaches the main menu. |
| Direct3D 9 test | Draws and reads every frame back: about 55 fps through wined3d, 58 fps through DXVK. |
| Vulkan test | Instance, device, memory mapped into a 32-bit program, a surface and a swapchain; 180 frames at about 61 fps. |

In progress: The Sims 2 Legacy Collection starts, talks to its launcher
emulation over LSX and loads towards its title screen, then closes. The
Ultimate Collection has not been tested.

The OpenTTD, Quake III and WarCraft III figures were measured with the Mesa 20.1
runtime; the full package now ships the Mesa 26 one.

## What is in place

- **x86 execution.** `dlls/winebox64` is the WoW64 CPU DLL. It runs Box64's
  ARM64 dynarec with separate writable and executable code mappings, as Horizon
  requires, including Box64's call/return optimization. System calls and unix
  calls from x86 code are dispatched without leaving the emulator loop; their
  result goes in Eax as wow64cpu puts it. A fault in x86 code (access
  violation, division by zero, INT3, invalid opcode) is raised into the
  program's own exception handlers with the faulting instruction's registers.
- **Wine on Horizon.** An in-process Wine server
  (`dlls/ntdll/unix/horizon*.c|h`) covers files (with Windows sharing modes),
  directories, sync objects, threads and user APCs, the registry
  (`system.reg`/`user.reg` on the card, plus `config/classes.reg`), message
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
  nvc0 and Vulkan through NVK, with Wine's winevulkan on top. Direct3D 9 runs
  through wined3d on OpenGL, or through DXVK on Vulkan for programs set to use
  it; frames of another size than the screen's (800x600 or 640x480 in full
  screen) are scaled to it, keeping the aspect ratio. The earlier runtime on
  Mesa 20.1's nouveau driver, patched for pinned 32-bit buffers
  (`wine-nx-probe/mesa`), can still be built.
- **Audio.** `winenxaudio.drv` plays through audout, for mmdevapi and DirectSound.
- **Input.** The touchscreen, the controller as a mouse or as a keyboard with
  per-game mappings, and XInput, which sees player 1 as an Xbox 360 controller.
- **Memory.** Fixed-base games such as NFSU2 need a 32-bit address space:
  launch the NRO through a forwarder made with "32-bit, no alias", which the
  launcher can install itself and which also raises the memory limit to 2 GiB.
  Wine reserves the program's low address space early, keeps its allocations
  clear of the memory Horizon hands to libnx, and leaves 32-bit programs'
  Vulkan memory in the driver's own mappings.
- **Launcher.** An SDL2 launcher with a Home and a Library view, per-game
  options, a controls editor, SteamGridDB artwork, and forwarder installation
  (`wine-nx-probe/source/launcher*.c`, `forwarder.c`).
- **Diagnostics.** A thread and core report, a sampling profiler, fatal fault
  reports that name the x86 instruction behind translated code, and a map of
  the address space when a program runs out of it.

## Files on the card

Everything lives in `sdmc:/switch/wine`: the runtime `wine-nx-runtime.nro`,
Wine's files, and `drive_c` with the programs. C: is `drive_c` and Z: is the
card's root.

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

The controls every program uses are in `config/keys.txt`. A program's own
files sit next to its executable, named after it:

| File | Content |
|---|---|
| `NAME.wine-nx.txt` | Title, hidden from the library, verbose traces, profiler, `windows` (compositor or framebuffer), `d3d9=dxvk`, address space, own controls |
| `NAME.args.txt` | Its command-line arguments |
| `NAME.keys.txt` | Its own controls, over `config/keys.txt` |
| `NAME.box64.txt` | Box64 code generation options, one `BOX64_DYNAREC_*=value` per line |

`d3d9=dxvk` makes the program load DXVK's `d3d9.dll` from `C:\dxvk` instead of
Wine's. The launcher's look is kept in `launcher.txt` and its library in
`launcher-library-v2.ini`.

## Logs

Each run writes `wine-nx-runtime.log`, and a copy named after the program,
`game-NAME.log`. Its first lines include `[BUILD]`, the runtime version, worth
checking before reading anything else. `[PROGRESS]` lines report every 10
seconds: frames, OpenGL and system call rates, memory and translation counters.
With the profiler on, `[THREADS]`, `[SERVER]` and `[PROF]` show where each busy
thread spends its time. DLLs that fail to load are logged (`[NXLDR]`, and
Wine's `err:` lines) even without verbose traces, and so are files a program
could not open (`[FS]`, with how it asked), the first overlapped socket
operations (`[ASYNC]`), user APCs (`[APC]`) and x86 faults (`[BOX64]`).

## Building

Requirements:

- Docker with the `devkitpro/devkita64` image, for the Switch build and the Box64
  tests.
- LLVM-MinGW 20260505 in `wine-nx-probe/toolchains/llvm-mingw-20260505-ucrt-macos-universal`,
  and bison (Homebrew's), for Wine's PE modules.
- A Wine PE build tree, configured once:

```sh
mkdir -p wine-nx-probe/build-wine-wow64-pe && cd wine-nx-probe/build-wine-wow64-pe
PATH="$PWD/../toolchains/llvm-mingw-20260505-ucrt-macos-universal/bin:/opt/homebrew/opt/bison/bin:$PATH" \
    ../../configure --enable-archs=aarch64,i386 --enable-winebox64=aarch64
```

- For the Mesa 26 runtime, a [mesa-switch](https://github.com/danfromtico/mesa-switch)
  checkout in `~/mesa-switch` and the image its `Docker.rust` makes
  (`devkitpro-mesa-rust`). For DXVK, a [DXVK](https://github.com/doitsujin/dxvk)
  checkout in `~/dxvk`.

Then, from the repository root:

```sh
sh wine-nx-probe/build-wow64-dynarec.sh           # Wine modules, and the Mesa 20.1 runtime NRO
sh wine-nx-probe/build-mesa-switch.sh             # Mesa 26 (OpenGL and Vulkan) into build-mesa-switch/install
docker run --rm --platform linux/arm64 -v "$PWD:/work" -w /work devkitpro/devkita64 sh -ec '
    cmake -S wine-nx-probe -B wine-nx-probe/build-switch-wow64-mesa-switch -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/work/wine-nx-probe/cmake/switch-devkitA64.cmake \
        -DWINE_NX_PE_BUILD_DIR=/work/wine-nx-probe/build-wine-wow64-pe \
        -DWINE_NX_BOX64_DYNAREC=ON -DCMAKE_BUILD_TYPE=Release \
        -DWINE_NX_MESA_SWITCH_DIR=/work/wine-nx-probe/build-mesa-switch/install/opt/devkitpro/portlibs/switch/lib
    cmake --build wine-nx-probe/build-switch-wow64-mesa-switch --target wine-nx-runtime-nro'
python3 wine-nx-probe/tools/package-wow64-full.py # the whole SD-card payload as one zip
python3 wine-nx-probe/tools/package-wow64-dxvk.py # the Mesa 26 runtime, Vulkan and DXVK, over the full payload
```

The packagers copy the ARM64 PE modules (`winebox64.dll`, `wow64.dll`,
`ntdll.dll`, `win32u.dll`, `wow64win.dll`) from the PE build tree without
rebuilding them. After changing `dlls/winebox64`,
`wine-nx-probe/source/wow64_box64_bridge.c` (which `winebox64` compiles in) or
`dlls/wow64`, rebuild the module first:

```sh
PATH="$PWD/wine-nx-probe/toolchains/llvm-mingw-20260505-ucrt-macos-universal/bin:/opt/homebrew/opt/bison/bin:$PATH" \
    make -C wine-nx-probe/build-wine-wow64-pe dlls/winebox64/aarch64-windows/winebox64.dll
```

The runtime's version is `WINE_NX_RUNTIME_BUILD` in
`wine-nx-probe/source/runtime.c`, and archives are written to
`wine-nx-probe/build-switch-wow64-dynarec`. Other packagers in
`wine-nx-probe/tools` stage single programs over the full payload (OpenTTD,
Quake III's engine, WarCraft III's setup, the Direct3D 9, OpenGL and audio
tests). `package-wow64-dll-overlay.py --log wine-nx-runtime.log` builds the
i386 DLLs a run reported missing, with everything they import, as an overlay
zip.

## Tests

```sh
sh wine-nx-probe/check-runtime-console.sh      # host unit tests of runtime and server pieces, under ASan and UBSan
sh wine-nx-probe/check-box64-execution.sh      # Box64 interpreter and dynarec in an ARM64 container, plus their Switch build
sh wine-nx-probe/check-wow64-box64-bridge.sh   # the x86 system-call gate
sh wine-nx-probe/check-wow64-box64-unix.sh     # the native side of the CPU DLL
sh wine-nx-probe/check-audio.sh                # the audio driver
sh wine-nx-probe/tests/check-launcher-host.sh  # the launcher, headless, with scripted input (Homebrew's sdl2, sdl3, sdl2_ttf, libpng)
for t in wine-nx-probe/tests/check_*.py; do python3 "$t"; done  # server pieces run against real host sockets and files
```

`wine-nx-probe/tests/win32` holds small Windows programs the full package puts
on the card (`C:\APC Test`, `C:\Socket Test`): each does what a game does with
one piece of the machinery and reports the result in a message box, which the
log records. Run under desktop Wine first, they give the answers to expect.

Box64 is pinned in `wine-nx-probe/vendor/box64` (fetched by
`tools/bootstrap-box64-core.sh`) and never edited:
`wine-nx-probe/cmake/Box64Core.cmake` builds patched copies of the files it
changes, and fails if the pinned text moves.

## Layout

| Path | Content |
|---|---|
| `dlls/ntdll/unix/horizon*` | Horizon server, memory, sections, sockets, exception handling |
| `dlls/win32u/winnx_drv.c`, `winnx_vulkan.c` | Display, input and Vulkan surface driver |
| `dlls/winebox64` | WoW64 CPU DLL |
| `wine-nx-probe/source` | Runtime: startup, launcher, compositor, Box64 engine, profiler, audio and XInput backends, forwarder installer |
| `wine-nx-probe/tests` | Host and PE32 tests |
| `wine-nx-probe/tools` | Packagers and game setups |

## Limits

- Speed. Heavy Direct3D games are limited by translated x86 code on the game's
  main thread and by Wine's Direct3D layer, rather than by the GPU.
- Wine's first-run setup (wineboot) does not run. The package writes the COM
  classes every staged DLL serves to `config/classes.reg`; WarCraft III's setup
  program (`C:\WarCraft III Setup\war3-setup.exe`) registers DirectShow for its
  movies.
- A 32-bit address space leaves a program about 2 GiB of addresses and caps the
  whole process at 2 GiB of memory.
- 32-bit x86 and ARM64 programs only; x86-64 programs do not run.
- One program at a time; one controller.
- Missing DLLs: the card only holds what earlier programs needed. The log names
  what is missing, and the overlay packager builds it.

## Credits

Autorun is built from these projects; each keeps its own copyright and license.

| Project | Authors | License | Used for |
|---|---|---|---|
| [Wine](https://www.winehq.org) | The Wine project authors | LGPL-2.1-or-later | The Windows API, loader, WoW64, and the Direct3D, OpenGL and Vulkan layers; this repository is a Wine fork |
| [Box64](https://github.com/ptitSeb/box64) | ptitSeb and contributors | MIT | 32-bit x86 execution: its interpreter and ARM64 dynarec are the WoW64 CPU backend (`wine-nx-probe/vendor/box64`) |
| [DXVK](https://github.com/doitsujin/dxvk) | Philip Rebohle, Joshua Ashton, Robin Kertels, Jeffrey Ellison and contributors | zlib/libpng | Direct3D 9 over Vulkan, for programs set to `d3d9=dxvk` |
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
| [7-Zip](https://www.7-zip.org) | Igor Pavlov | LGPL-2.1 | `7zr.exe`, the benchmark and archive test program on the card |
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

- [WoW64 CPU interface](wow64-box64-interface.md)
- [x86 memory layout on Horizon](horizon-x86-memory.md)
- [Build-by-build notes](../wine-nx-probe/README.md)
