# Bringing Carafe's work into Autorun

The aim: this fork behaves like [Carafe](https://github.com/carafe-nx/carafe)
where Carafe is the better tuned one, and stays Autorun where the two differ in
kind. Games stay on the SD card and start from the launcher (or from a game
forwarder the launcher makes); nothing is packed into an NSP and nothing is
installed.

This file is the ledger of that work: what was compared, what has been taken,
what is still to take and in which order, and what is left out and why. It is
changed in the same commit as the code it describes.

Compared trees:

| | Commit | Date |
|---|---|---|
| This fork | `1781365` (upstream `autorunhq/autorun` `1ac9b6c` + 19 commits) | 2026-10-16 base |
| Carafe | `73ffb5a` (v0.5.6) | 2026-11-10 |
| Autorun as Carafe pins it | `636913ca` | 2026-10-03 |
| Carafe's 127 Autorun patches as a branch | `carafe-patches` `b785476` | on `636913ca` |

## What the comparison found

1. **Upstream Autorun has already taken much of Carafe's patch set, in its own
   words.** The fork's base is 46 upstream commits newer than Carafe's pin, and
   many patches arrive there renamed or widened: the thread wait is
   `horizon_thread_may_wait` and `HORIZON_THREAD_WAIT_*` instead of the
   `CARAFE_THREAD_*` macros, the alias region logs as `[HMAP]`, the file locks
   are upstream's own, the cursor clip also acts after a move.
2. **A textual merge is therefore not safe.** A trial `git merge` of
   `carafe-patches` gives 100 conflict hunks in 19 files, and its *clean* hunks
   are the dangerous ones: where both sides added the same thing a few lines
   apart, git adds it twice (the cursor clip block, the `HORIZON_STOP_*`
   defines, the futex exit block in `sync.c`, the device ioctl check). Every
   hunk is read and carried over by hand instead.
3. **What is really missing is a short list of features**, found by comparing
   the functions and macros each side defines rather than lines:
   - the hang and thread-budget diagnostics (`carafe_diag`, `[BUDGET]`,
     `[SERVED]`, the alert table, the stall report on request);
   - the server thread pool;
   - the FEX-aware profiler and the thread placement the orchestrator uses;
   - the FFmpeg parser for DirectShow and Windows Media (taken in stage 1);
   - the FEX thread counters area, Carafe's DXVK as the system Direct3D, and
     the options that only Carafe's FEX and DXVK builds read.
4. **FEX, DXVK and every other Windows module do not come from this
   repository.** The card archive holds none; the launcher downloads them from
   `autorunhq/autorun-horizon-dlls`. Carafe's 25 FEX patches, 22 DXVK patches
   and dxwrapper can only reach a console through a DLL repository of this
   fork's own, or by shipping those few DLLs inside the card archive. That is
   stage 5 and needs a decision first.
5. **Licences.** Carafe's overlay (`runtime/overlay`) and its Autorun patches
   are LGPL-2.1-or-later like Wine; its FEX patches are MIT and its DXVK
   patches Zlib, as the files they change. Everything else in Carafe,
   including `runtime/orchestrator`, `runtime/dshow` and `runtime/media`, is
   under Carafe's own GPL-3.0-or-later. The runtime already links
   GPL-3.0-or-later code (LSFG-VK), so the orchestrator can be linked in, but
   its files keep that licence, say so at their top and are named in the
   credits of `README.md` and `documentation/technical.md`. Where a few lines
   of data sufficed (the DirectShow filters), they were derived from Wine's own
   sources instead and stay LGPL.

## Stages

Each stage is one or a few commits, checked on the host and built by the
`build` workflow before the next one starts.

| Stage | What | State |
|---|---|---|
| 1 | Videos through DirectShow and the Windows Media reader; small log and profiler fixes | **done** |
| 2 | Diagnostics: `carafe_diag.c`, the event ring and connection table in `horizon.c`, `[BUDGET]` and `[SERVED]`, the alert table in `sync.c`, the stall report on request, `carafe_waits.c`, `carafe_mesa.c`, the fd queue report, FEX's counters | **done** |
| 3 | The profiler: x86 names of the code FEX runs, the busiest translated blocks, FEX time outside blocks, stages by call chain (`thread_profile.c`, +536 lines) | **done** |
| 4 | The orchestrator (`runtime/orchestrator`, GPL-3.0-or-later) with its host tests, `wine_nx_thread_place` and the side cores, `carafe_lock_boost.c`, `carafe_bench.c` | **done**, as two settings of a program, both off by default |
| 5 | Carafe's FEX and DXVK patches, dxwrapper, Wine Mono, WinRT classes, and with them `carafe_fex_stats.c`, the FEX and DXVK options and the five missing menu rows | FEX for 32-bit programs and DXVK: **done**, downloaded from this repository's releases. The rest is open, see below |
| 6 | The server thread pool (`0023`): the program's threads share a few server threads instead of having one each | **done**, as a setting of a program, off by default |
| 7 | The menu's translations (Carafe has 15 languages), the remaining graphics hunks in `winnx_drv.c`, `winnx_opengl.c` and `win32u/vulkan.c`, read one by one | |

### Stage 1, done

| Change | From | Files |
|---|---|---|
| A parser for `winegstreamer`: libavformat demuxes, libavcodec decodes, libswscale and libswresample convert. Every `wg_parser` call used to fail, so DirectShow and the Windows Media reader played nothing | patch `0096`, taken whole | `horizon-wine/source/media_unix.c` |
| The containers and codecs those files use: ASF, AVI, MPEG-PS, MP3, WAV, AIFF, AU; WMV1–3, VC-1, MS-MPEG4, MPEG-1/2/4, H.263, Cinepak, Video 1, RLE, Indeo 3–5; MP1–3, PCM, ADPCM, GSM | `runtime/media/configure.in` | `horizon-wine/cmake/Media.cmake` |
| The DirectShow splitter that plays through that parser, with the two categories and the quartz filters a filter graph connects it to, loaded into the registry at every start. `autorun-setup.exe` registers quartz's filters once per card but never winegstreamer's splitter; Carafe adds these eight keys to `classes.reg` when it packs a game, and here that file comes with the downloaded DLLs | `runtime/dshow/add-directshow-filters.py` (the same keys and bytes, generated from Wine's own tables) | `dlls/ntdll/unix/horizon_directshow_reg.h`, `horizon_registry_server.h`, `horizon-wine/tools/make-directshow-reg.py` |
| A builtin module that asks for unix calls and gets none is named in the log, the first 16 | patch `0091` | `dlls/ntdll/unix/virtual.c` |
| The ASLR region is traced once, not at every reservation | patch `0061` | `dlls/ntdll/unix/horizon.c` |
| The profiler's tables hold 4096 addresses instead of 512 | patch `0053` | `horizon-wine/source/thread_profile.h` |

Checked: `tests/registry_server.c` (the filters are there after a start, under
any case of the path, and after a save and a restart) with clang 18 `-Werror`
and with gcc under ASan and UBSan; `tests/check_directshow_reg.py` (the header
matches its generator and every `FilterData` reads back as quartz reads it);
the generated keys and bytes compared once with what Carafe's script writes,
which are the same; every codec, demuxer,
parser and filter name against FFmpeg 7.1.5 at the pinned revision;
`tests/thread_profile.c`; the host test suite against its count before the
change; the `build` workflow.

Not checked, because it needs a console: that a game's video plays.

What it rests on outside this repository: the Windows half is the downloaded
`winegstreamer.dll` (`autorunhq/autorun-horizon-dlls`, group `directx-media`),
whose `classes.reg` names it as the server of the splitter's class. The table of
unix calls in `media_unix.c` is indexed by the names in
`dlls/winegstreamer/unixlib.h`, which is the same file here as at Carafe's base,
so the two halves agree as long as that DLL was built from this header.

### Stages 2 and 3: diagnostics and the profiler, done

Nothing here changes what a game does; it changes what the log says about it.

| Change | From | Files |
|---|---|---|
| The server keeps its last 512 thread events (new, suspend, resume, terminate, a wait on a suspended thread) and a table of its connections; the stall report prints the newest 96 and where each connection stands | patch `0001`, applied | `dlls/ntdll/unix/horizon.c` |
| A handle taken from the fd queue that had to skip other owners' entries says so, and whether it found its own | the two report lines of `0002`; the owner matching was already here | `horizon.c` |
| A table of the alertable waits in progress, printed with the stall report | patch `0004`, applied; the hunk at the wait's two returns by hand, before the fork's own `carafe_futex_wait_left()` | `dlls/ntdll/unix/sync.c` |
| `[BUDGET]`: threads used and their peak, windows, server threads, the pool's threads with how many are idle and how many wait inside a request (stage 6), refusals, how many creations waited for a place, how many of those got none and the time waited. `[SERVED]`: the ten requests the server answered most often and the time spent in each, every 30 s | patch `0019`, applied; the counters of `0020` and `0028` by hand, because the wait itself is the fork's `horizon_thread_may_wait` loop | `horizon.c` |
| A creation that has waited 5 s writes where it was called from and asks the stall watch for every thread's place, instead of reading them itself | `0028`'s report, `0030` | `horizon.c`, `horizon-wine/source/runtime.c` |
| The stall report names the native callers of a Windows thread from its stack | patch `0029` | `horizon-wine/source/thread_profile.c` |
| With the profiler on: the x86 address of the code FEX runs, the busiest translated blocks written to `logs/jit-blocks.txt`, FEX's time outside blocks split by part, time counted by the calls near a thread's start, the block of a pc after a return point | patches `0050`, `0055`, `0058`, `0065`, `0076` | `thread_profile.c` |
| With the profiler on, FEX is asked for its counters and gets an area for them from the runtime's heap; Carafe's FEX also gets `FEX_GDBSYMBOLS=1` and Carafe's DXVK `CARAFE_D3D9_STATS=1` | patches `0063`, `0066`, `0067`, `0081` | `runtime.c`, `horizon-wine/source/fex_unix.c` |
| The reports themselves: frame rate, clocks and GPU load with every thread report; with the profiler on, each core's load, what each thread waited for by kind (`[WAITS]`), FEX's counters by thread (`[FEX]`), and once the A57 timings; where native threads are started from (`[NATIVE]`); the thread pools Mesa makes | `runtime/overlay`, taken whole | `horizon-wine/source/carafe_diag.c`, `carafe_bench.c`, `carafe_waits.c`, `carafe_fex_stats.c`, `carafe_lock_boost.c`, `carafe_mesa.c` |

How `thread_profile.c` was brought over: as a three-way merge of the fork's
file, Carafe's and their common base, because 11 of Carafe's patches change it
and the fork had changed the same places. Of the nine conflicts, the fork's side
was kept where it stops and holds threads (its 20 ms wait instead of an endless
one, `wine_nx_stop_state`) and where it collects the rows the tasks panel also
reads; Carafe's side everywhere else. The merge had also written one block
twice, which was removed. What is left of Carafe's file that is not in the
fork's is 13 lines, all of them those two places.

The merge also brings what the orchestrator calls (`wine_nx_thread_views`,
`wine_nx_thread_place`, `wine_nx_thread_reserve_core`,
`wine_nx_thread_side_cores`, patches `0064` and `0072`). Only the orchestrator
calls them (stage 4), and no core is reserved while it is off.

What differs from Carafe:

- `carafe_lock_boost.c` times a wait for a libnx mutex, and in Carafe also
  raises the waiting thread's priority by one. Here that second half is off
  (`carafe_lock_boost_enabled` starts at 0) unless a program's settings turn it
  on (stage 4).
- `[BUDGET]` had no `pool=` field until stage 6 brought the server thread pool.
- The stock FEX is asked for its counters too. Its record has the same size as
  Carafe's, with padding where Carafe's counts returns that miss the
  call-return stack, so that one number reads 0.

Checked: `tests/carafe_reports.c` (waits by kind and thread, the first sight of
a thread counting nothing, the lock wait's priority off and on and for a thread
at another priority, FEX's counters as differences between reports, a record
that would end outside the area, a record of another size), also with two
errors put into the code to see it fail; `tests/check_thread_creation_wait.py`,
which now also checks what a wait reports (counted once, as placed or lost,
with its time; one request for the threads' places; refusals) and a fifth case,
a place that frees during the wait; `tests/check_horizon_fd_owner.py` with the
two report lines; `tests/thread_profile.c`; the host suite, 82 passing before
and after; every changed and added C file compiled for AArch64 against libnx's
and newlib's headers; the `build` workflow.

Not checked, because it needs a console: any of these lines in a real log, the
clock and GPU services answering, and that FEX fills the area.

### Stage 4: the orchestrator, done

| Change | From | Files |
|---|---|---|
| The orchestrator. Every two seconds it reads each core's idle time and each thread's load. While no core is full it only watches and the runtime's balancer places threads. When one is, it lays the busy threads out itself: the busiest gets a core that the balancer is then kept away from, the others are spread by their load, and with **4-core support** on a thread may be sent to core 3 and back | `runtime/orchestrator`, taken whole; the link option as in Carafe's `carafe-overlay.cmake` | `horizon-wine/source/orchestrator.c`, `orchestrator_policy.c`, `orchestrator_policy.h`, `horizon-wine/CMakeLists.txt` (`--wrap=wine_nx_thread_balance`) |
| Its tests and the tool that replays the decisions of a log on the host | `runtime/orchestrator/test` | `horizon-wine/tests/orchestrator.c`, `horizon-wine/tools/orchestrator_replay.c` |
| Two switches in a program's settings, **Thread orchestrator** (`orchestrator=1`) and **Lock wait boost** (`lock-boost=1`), under Emulation | new here; Carafe has the first as a key of its own program file and the second always on | `horizon-wine/source/launcher_settings.h`, `launcher.c`, `runtime.c` |
| Carafe's code asks the runtime whether the profiler and the orchestrator are on, instead of reading the program's file itself | new here | `runtime.c` (`wine_nx_program_profiling`, `wine_nx_program_orchestrating`), `carafe_settings.c`, `carafe_diag.c` |

What differs from Carafe:

- Both are off unless a program's settings turn them on. Carafe's orchestrator
  is on wherever the file does not say `orchestrator=0`, and its lock wait
  always raises the priority. Neither has run on a console in this fork, and
  the fork's balancer and its stop-and-hold code differ from Carafe's, so a
  default has to wait for measurements.
- The orchestrator asks whether it is on at every period. Carafe's asks once;
  here the first period comes while the launcher is on the screen, before any
  program is chosen. Until a program's settings have been read the runtime
  answers "not yet", and the orchestrator reads and moves nothing.
- If the switch goes from on to off, the orchestrator gives its reserved core
  back to the balancer. A program's settings do not change while it runs, so
  nothing does this today.
- The orchestrator does not ask whether core balancing was turned off
  (`no-balance.txt`), as in Carafe: with balancing off and the orchestrator on,
  only the orchestrator moves threads.
- The **Lock wait boost** row is not shown in a build with
  `WINE_NX_SWAP_POC`, which wraps the same kernel call for itself.

This stage also corrects stage 2. `carafe_diag.c` asked Carafe's
`carafeMeasuring()` once and kept the answer. The log's thread makes its first
report 10 to 30 s after the runtime starts, which is while the launcher is on
the screen unless a program is picked sooner; no program is known then, so the
answer was "off" and stayed so, and the reports that need the profiler
(`[WAITS]`, `[FEX]`, each core's load, the A57 timings) were not written for
the program picked afterwards. It also read only the program's `profile=1`, not
`profile.txt`. Now it asks `runtime_profile` every time, and the reports and
the services they open (clocks, GPU load) wait until a program starts.

Checked: `tests/orchestrator.c`, Carafe's own test of the decisions, also under
the address and undefined-behaviour sanitizers; `tests/orchestrator_switch.c`,
the orchestrator with its policy on made-up cores and threads (before the
settings are read and with the switch off it calls only the balancer and reads
nothing; on, it says so once, measures, puts its own thread back where it was,
places the threads after the policy's two periods, reserves the busiest
thread's core, stops calling the balancer, and gives all of it back when turned
off), with five errors put into the orchestrator to see each one fail;
`tests/check_orchestrator_setting.py`, which compiles the runtime's two answers
with Carafe's two questions and checks the strings of the settings, with six
errors put into the code to see each one fail; `tests/launcher_settings.c` for
the two keys; the host suite, 83 passing (the 82 of before and the new check)
with the same 30 failing as before; the changed C files and the launcher
compiled for AArch64; the `build` workflow.

Not checked, because it needs a console: that the orchestrator's layouts make
any game faster here, that the kernel's idle counters read as the code expects,
how it behaves next to this fork's stop-and-hold code, and whether the lock
wait's priority helps or starves other threads.

### Stage 5: FEX and DXVK, done

Carafe builds two Windows modules differently from the DLL repository: FEX and
DXVK. Neither is in the card archive. The `carafe dlls` workflow builds them
from `carafe/dlls` and publishes each as a release of this repository, and the
launcher lists, downloads and installs those releases with the code that
already does it for DXVK, Sarek, GPLAsync and VKD3D-Proton.

| What | Where |
|---|---|
| Carafe's 25 FEX and 22 DXVK patches, the pins, the build scripts and the packer | `carafe/dlls/` |
| The build and the releases `fex-carafe-<version>` and `dxvk-carafe-<version>`, each holding `<tag>.tar.gz`; then the launcher's own code downloads and installs what was released | `.github/workflows/carafe-dlls.yml` |
| Two more release sources, in the repository named by `WINE_NX_CARAFE_DLLS_REPO`. Its releases are told apart by what their tags begin with. FEX's archive has one folder, and its DLL is an ARM64 one | `horizon-wine/source/dxvk_releases.c`, `.h` |
| **Game Settings, Graphics, DXVK version, DXVK Carafe**; `dxvk-source=carafe` in a program's settings; installed in `drive_c/dxvk-carafe` and `dxvk-carafe64` | `launcher_graphics.c`, `launcher_settings.h`, `launcher.c` |
| **Game Settings, Emulation, FEX build** (32-bit programs on FEX): Autorun or Carafe; `fex-build=carafe` and `fex-version=`; installed in `drive_c/fex-carafe/versions` | `launcher_graphics.c`, `launcher_settings.h`, `launcher.c` |
| The FEX a program is set to is the file in system32 when it starts, and the stock one is back when the launcher opens | `horizon-wine/source/fex_build.c`, `runtime.c`, `launcher.c` |
| The ten DXVK options only Carafe's build reads (patches `0060`, `0068`–`0071`, `0082`), shown and written for that source only | `horizon-wine/source/dxvk_options.c` |
| The twelve FEX options (patches `0051`, `0054`, `0056`, `0057`, `0059`, `0073`–`0075`, `0077`), the same rows and defaults as Carafe's, shown and passed only to Carafe's build | `horizon-wine/source/fex_options.h` |

How the FEX build gets used. `wow64.dll` loads its CPU module by one name from
one place, `system32\libwow64fex.dll`, and that file is the DLL repository's.
So when a 32-bit program starts on FEX, the runtime makes system32 hold the
build that program is set to: the first time a Carafe build goes in, the stock
DLL is copied to `drive_c/fex-carafe/stock-libwow64fex.dll`, and
`drive_c/fex-carafe/in-system32.txt` records which Carafe DLL stands in. When
the launcher opens, the stock DLL goes back before the Windows DLLs check runs,
so that check never finds a file it would want to replace. If a DLL update
replaces the file anyway, what is in system32 no longer matches the record and
is taken for the stock DLL, which it is.

The options. FEX has each of Carafe's nine switches off unless told otherwise,
and three more (`DiskCache`, `DiskCacheFileMapping`, `GDBSymbols`) exist in
stock FEX by name without the code behind them here. A program on the stock FEX
is therefore neither shown nor given any of the twelve. DXVK's
`dxvk.shaderCompilationMethod` is a second row beside Sarek's of the same name,
with Carafe's default (`async`); Sarek's keeps its own (`dyasync`).

Checked on the host: `tests/carafe_dlls.c` (the release list in the shape
GitHub gives for this repository, an archive packed as `package.py` packs one,
which versions count as installed, and every way system32 changes hands:
first use, another Carafe build, back to stock, a DLL update in between, a
removed release, no stock copy), `tests/dxvk_releases.c`,
`tests/dxvk_options.c`, `tests/fex_options.c`, `tests/launcher_settings.c`.
`launcher.c` and `launcher_graphics.c` compile against SDL's headers.
Checked by the workflows: both DLL sets build; `package.py` refuses a FEX that
imports anything but `ntdll.dll` and `wow64.dll` or lacks a `BTCpu` export that
`dlls/wow64/syscall.c` asks for; and `tests/carafe_dlls.c live` downloads and
installs the real releases with the launcher's code.

Not checked, because it needs a console: that a game runs on either build, the
menu rows on the screen, and what the options do to a game.

Still open in stage 5:

| What | Why not yet |
|---|---|
| FEX for 64-bit programs (`libarm64ecfex.dll`) | Carafe's patch set is built here for `wow64fex` only; the ARM64EC target was not tried |
| Carafe's DXVK as the system Direct3D (`0105`, `0133`) | Here it is a source like the others, in a folder put in front of system32, which is what this fork does for every DXVK |
| dxwrapper, Wine Mono, the WinRT classes, patch `0006` | Each needs Windows DLLs other than these two |

### Stage 6: the server thread pool, done

Wine's server runs inside the process here, and every thread of a program has
a thread of the server that answers its requests: two of the console's threads
for each Windows thread. Carafe's patch `0023` lets the program's threads
share a few server threads. A request pipe tells the pool when it holds a
request; a pool thread takes one request of one client at a time; two threads
stay ready (`HORIZON_SERVER_POOL_SPARE`); a thread that is about to wait inside
a request says so, and another is started when none is left free; the threads
above the two end when they come back and find the two idle.

| Change | From | Files |
|---|---|---|
| The pool, and `pool=workers/idle/blocked` in `[BUDGET]` | patch `0023`, applied with `patch` (21 hunks, none rejected) | `dlls/ntdll/unix/horizon.c` |
| The mark before a wait for another thread to change state | one line of `0121`, by hand | `horizon.c` (`horizon_server_wait_thread_locked`) |
| A switch in a program's settings, **Shared server threads** (`server-pool=1`), under Emulation | new here; Carafe has the pool on wherever Horizon synchronization is | `horizon_private.h`, `horizon-wine/source/launcher_settings.h`, `launcher.c`, `runtime.c` |
| A pipe's byte count and its writer's end are stored atomically | new here | `horizon.c` (`horizon_pipe_write_r`, `horizon_pipe_read_r`, `horizon_pipe_close_r`) |

What differs from Carafe:

- The pool is off unless a program's settings turn it on, and it needs
  **Synchronization** set to Horizon, as in Carafe, whose patch says only that
  standard synchronization keeps a thread per client. As read here, the reason
  is that with standard synchronization every wait is made inside the server
  and would hold a pool thread for as long as it lasts.
  `horizon_server_pool_enabled()` is
  `horizon_fast_sync_enabled && horizon_server_pool_wanted`; Carafe's is the
  first half only. The log says which it is: `[SYNC] server threads: ...`. It
  has not run on a console in this fork, so a default has to wait for that.
- Carafe's pool thread reads a pipe's count and whether its writer is gone
  without the pipe's lock, with an atomic load, while the pipe writes both
  with plain stores. The order of the two locks makes the outcome right either
  way (the writer tells the pool after it has stored, and the pool thread reads
  under the pool's lock), but the thread sanitizer reports it, so the stores
  are atomic here. The three counters `[BUDGET]` prints are read the same way
  by the log's thread and were left as they are: they only feed that line.

Why it was safe to take, as far as reading can tell:

- A pool thread that waits without the mark holds one of two threads, and two
  such waits would stop every request. So every wait in the server was
  compared between the two trees. Carafe's `horizon.c` has 24 places that
  wait, the fork's 25. The one more is the wait in `horizon_poll_pipe`; its
  callers that pass a time limit are on the client's side (`file.c`), and the
  server's only call passes 0. The marks are in the same three places as in
  Carafe: `horizon_server_sleep_locked`, `horizon_sync_sleep_locked` and
  `horizon_server_wait_thread_locked`.
- What a server thread knows about itself is the same in both trees: the
  connection's structure is identical, and so are the thread-local variables
  apart from the fork's `wine_nx_stop_word`, which `horizon.c` reads only in
  its exception handler (through `wine_nx_stop_state()`).
- Leaving a program: pool threads are registered with the runtime as server
  threads (`'s'`), so the quit sequence counts them. An idle one looks for a
  quit every quarter of a second (`horizon_cond_wait_quit`), one inside a
  request stops where the per-client threads stop, and each then ends and is
  joined like them. The next program starts from a freshly loaded runtime
  (`envSetNextLoad`), so the pool's counters do not carry over.

Tests: `tests/check_server_pool.py` (new, in the `build` workflow) builds the
pool's own functions from `horizon.c`, with `horizon_cond_wait_quit`, and runs
them with real threads under the address and the thread sanitizer: 9000
requests of six clients written from three threads are each served once, in
their client's order and never by two threads at once, by the same two
threads; two waits inside a request start a third thread and the other clients
are served meanwhile; a thread that cannot be started is not counted, its
request stays in the pipe and the next request starts one; the extra threads
end; clients that end are finished once, after their last request; at a
quit the idle threads end; and every thread the pool started has been joined
once, by the pool's own reaping or as the runtime joins the ones a quit ends. The idle wait has no time limit in the test, so a
thread that is not told of work shows as a request never served. Twelve
changes made to the pool's code on purpose (no wake-up, no requeue, no third
thread, no retiring, a plain store, and so on) each made it fail.
`tests/check_horizon_fast_sync.py` now also checks that a pool thread marks
itself once per request, and `tests/launcher_settings.c` the setting.

Not checked, because it needs a console: that a game runs with the pool, how
many threads it saves there, and whether requests wait longer for a thread
than they did for their own.

## Left out, and why

| Carafe part | Why it stays out |
|---|---|
| The desktop app (`app/`) and the NSP it builds | It is the packing and installing this fork does without |
| The loader (`runtime/loader`) | It starts the runtime inside an NSP; here `hbloader` and the forwarder do |
| The `sdmc:` device over RomFS and save data, and the save commits, in `carafe_overlay.c` | Games and their saves are plain files on the card |
| Patch `0006` (shared user data and thread data in a 32-bit host address space) | Not needed on the fork's own paths: the launcher refuses to start a program unless the process is 39-bit (`launcher.c` `address_space_fits`), the forwarders it installs are always 39-bit (`forwarder.c` `npdm_patch`), and fixed-base games get the low window there. A 32-bit forwarder made by another tool that starts a game directly is not refused for i386 programs (`runtime.c` only refuses AMD64), and `documentation/technical.md` still describes that route; `0006` would matter only there. Its PE half needs rebuilt Windows DLLs, so it waits for the stage 5 decision |

## The 127 patches

"Lines found" is a textual measure: of the lines a patch adds (12 characters
or longer), how many are in the fork's file today, ignoring spaces. It
understates what upstream rewrote and says nothing where a block moved; the
last column is the reading of the code. Every row has been read. For the
rows that are not at 100 %, each line not found was looked up in Carafe's
final file and in the fork: either a later Carafe patch rewrote it, or the
fork has it in its own form, which the last column names.

| Patch | Subject | Lines added | Lines found | State |
|---|---|---|---|---|
| 0001 | horizon hang diagnostics | 122 | 98 % | taken (stage 2); two of its lines were rewritten by `0023` (stage 6) |
| 0002 | horizon fd owner | 37 | 97 % | owner matching was in the fork; the two report lines taken (stage 2) |
| 0003 | horizon alias region | 31 | 90 % | in the fork; upstream renamed the log tag to [HMAP] and guards the overflow |
| 0004 | sync alert diagnostics | 54 | 100 % | taken (stage 2) |
| 0006 | virtual 32bit address space | 23 | 13 % | deferred: the fork's launcher and its own forwarders are 39-bit with the low window; only a third-party 32-bit forwarder would need it, and its PE half needs rebuilt DLLs (stage 5) |
| 0007 | low window 32bit guest range | 10 | 100 % | in the fork |
| 0008 | runtime screen until first frame | 29 | 96 % | in the fork; the notice before the program starts has one more condition (`wine_nx_watch_window_pictures`) |
| 0009 | winnx vulkan reserve screen | 2 | 100 % | in the fork |
| 0010 | vulkan take screen at swapchain | 3 | 33 % | in the fork, with the call made weak |
| 0011 | winnx opengl first frame | 22 | 100 % | in the fork |
| 0012 | winnx flush mouse motion | 4 | 75 % | in the fork; the motion is flushed only when the pointer moved |
| 0013 | audio source rate frames | 30 | 100 % | in the fork |
| 0014 | winnx opengl clear after loading screen | 29 | 79 % | in the fork, with atomic loads and stores |
| 0015 | win32u screen size of a window | 15 | 100 % | in the fork |
| 0016 | opengl present at window position | 31 | 100 % | in the fork |
| 0017 | winnx screen follows game window | 72 | 100 % | in the fork |
| 0018 | horizon mouse hit test in program coordinates | 39 | 94 % | in the fork; it falls back to the first active monitor instead of the first |
| 0019 | horizon thread budget and server requests | 91 | 96 % | taken (stage 2); its `[BUDGET]` line is now the one of `0023`, with `pool=` |
| 0020 | horizon thread creation waits for a place | 42 | 57 % | in the fork as horizon_thread_may_wait and HORIZON_THREAD_WAIT_*; the wait counters taken (stage 2) |
| 0021 | horizon handle fd on the calling thread | 4 | 100 % | in the fork |
| 0022 | server handle fd on the calling thread | 1 | 100 % | in the fork |
| 0023 | horizon server thread pool | 180 | 98 % | taken (stage 6), behind a program's setting; the pipe's count and writer's end are stored atomically |
| 0024 | runtime tls holders give their place | 31 | 87 % | in the fork; the lines not found were rewritten by later Carafe patches |
| 0025 | horizon thread creation takes a tls holders place | 6 | 33 % | in the fork as horizon_tls_give_place |
| 0026 | thread join an ended thread without waiting | 23 | 100 % | in the fork |
| 0027 | horizon thread creation at the limit skips the kernel wait | 41 | 95 % | in the fork, as `horizon_thread_may_wait` |
| 0028 | horizon thread creation waits longer for a place | 39 | 58 % | in the fork (20 s wait, 2 ms then 20 ms steps); the caller report taken (stage 2) |
| 0029 | runtime stall report names native callers | 36 | 94 % | taken (stage 2) |
| 0030 | runtime stall watch reports on request | 6 | 100 % | taken (stage 2) |
| 0031 | runtime threads carry a stop request | 32 | 87 % | in the fork; the lines not found were rewritten by later Carafe patches |
| 0032 | horizon suspend pauses threads and terminate ends them | 106 | 81 % | in the fork, in its own form: the stop request is read through `wine_nx_stop_state()`, not a thread-local word, and the runtime's calls are weak |
| 0033 | syscalls stop on request and wow64 reads other threads | 36 | 80 % | in the fork, in its own form: the stop request is read through `wine_nx_stop_state()` |
| 0034 | sleep stops on request | 6 | 0 % | in the fork, with the calls made weak |
| 0035 | runtime tls holders wait on an address | 14 | 100 % | in the fork |
| 0036 | runtime shows the error screen when the game fails | 7 | 85 % | in the fork |
| 0037 | unhandled exceptions are noted for the error screen | 3 | 100 % | in the fork |
| 0038 | horizon refused threads are noted for the error screen | 2 | 50 % | in the fork, with one more condition: only once a program's threads exist |
| 0039 | horizon a runtime fault ends the program for the error screen | 17 | 100 % | in the fork |
| 0041 | runtime settings menu buttons | 3 | 100 % | in the fork |
| 0042 | vulkan menu over the frame | 461 | 90 % | in the fork, grown since: one routine records the upload of the menu's and the pointer's picture, and the pointer has a descriptor set of its own |
| 0043 | winnx opengl menu over the frame | 155 | 99 % | in the fork, with the call made weak |
| 0044 | compositor gl quad blends | 0 | 100 % | in the fork |
| 0045 | compositor gl draws blended quads | 8 | 100 % | in the fork |
| 0046 | compositor menu layer blends | 3 | 100 % | in the fork |
| 0049 | registry tells carafe when loaded and can be changed | 77 | 100 % | in the fork |
| 0050 | profiler names the x86 code fex runs | 71 | 92 % | taken (stage 3); the lines not found were replaced by later patches |
| 0051 | fex options carry the disk cache | 8 | 100 % | done in stage 5: the rows are in `fex_options.h`, shown and passed only to Carafe's FEX build |
| 0052 | horizon server grants file locks | 15 | 80 % | in the fork: upstream has its own file locks (HORIZON_REQ_LOCK_FILE 53, UNLOCK_FILE 54) |
| 0053 | profiler tables hold more addresses | 3 | 100 % | ported in stage 1 |
| 0054 | fex options carry x87 conversion rounding | 4 | 100 % | done in stage 5: the rows are in `fex_options.h`, shown and passed only to Carafe's FEX build |
| 0055 | profiler writes the busiest translated blocks | 91 | 98 % | taken (stage 3) |
| 0056 | fex options carry x87 fast stack and single precision | 8 | 100 % | done in stage 5: the rows are in `fex_options.h`, shown and passed only to Carafe's FEX build |
| 0057 | fex options carry gdb symbols | 4 | 100 % | done in stage 5: the rows are in `fex_options.h`, shown and passed only to Carafe's FEX build |
| 0058 | profiler splits fex time outside blocks | 79 | 100 % | taken (stage 3) |
| 0059 | fex options carry the flag option | 4 | 100 % | done in stage 5: the rows are in `fex_options.h`, shown and passed only to Carafe's FEX build |
| 0060 | dxvk options carry the unchanged state path | 3 | 66 % | done in stage 5: the row is in `dxvk_options.c`, for the DXVK Carafe source only |
| 0061 | horizon aslr region traced once | 5 | 100 % | ported in stage 1 |
| 0062 | runtime vulkan surfaces share a reserved screen | 18 | 100 % | in the fork |
| 0063 | runtime profiler asks direct3d 9 for call counts | 5 | 80 % | taken (stage 3); the counts come from Carafe's DXVK |
| 0064 | runtime threads placed by the orchestrator | 68 | 95 % | taken (stages 3 and 4); the three lines not found were rewritten by `0072` |
| 0065 | profiler counts time by the calls near a threads start | 90 | 95 % | taken (stage 3); the lines not found were replaced by later patches |
| 0066 | fex gets an area for its thread counters | 24 | 100 % | taken (stage 2) |
| 0067 | runtime profiler asks fex for its counters | 4 | 100 % | taken (stage 2) |
| 0068 | dxvk options carry the front end thread | 3 | 33 % | done in stage 5: the row is in `dxvk_options.c`, for the DXVK Carafe source only |
| 0069 | dxvk options carry the effect thread | 3 | 66 % | done in stage 5: the row is in `dxvk_options.c`, for the DXVK Carafe source only |
| 0070 | dxvk options carry the effect pass merge | 3 | 66 % | done in stage 5: the row is in `dxvk_options.c`, for the DXVK Carafe source only |
| 0071 | dxvk options carry the managed buffer copies | 3 | 66 % | done in stage 5: the row is in `dxvk_options.c`, for the DXVK Carafe source only |
| 0072 | runtime orchestrator moves threads to and from core 3 | 10 | 100 % | taken (stages 3 and 4) |
| 0073 | fex options carry the call cache and the bridge return | 8 | 100 % | done in stage 5: the rows are in `fex_options.h`, shown and passed only to Carafe's FEX build |
| 0074 | fex options carry the jump tables | 4 | 100 % | done in stage 5: the rows are in `fex_options.h`, shown and passed only to Carafe's FEX build |
| 0075 | fex options carry compact returns | 4 | 100 % | done in stage 5: the rows are in `fex_options.h`, shown and passed only to Carafe's FEX build |
| 0076 | profiler finds the block of a pc after a return point | 18 | 100 % | taken (stage 3) |
| 0077 | fex options carry slim return points | 4 | 100 % | done in stage 5: the rows are in `fex_options.h`, shown and passed only to Carafe's FEX build |
| 0078 | virtual large views start on 2mb | 10 | 100 % | in the fork |
| 0079 | fex options turn jump tables on | 1 | 100 % | in the fork |
| 0080 | fex options turn compact and slim returns on | 2 | 100 % | in the fork |
| 0081 | runtime profiler turns fex gdb symbols on | 1 | 100 % | taken (stage 3); only Carafe's FEX is given the option |
| 0082 | dxvk options carry sarek choices | 24 | 79 % | done in stage 5 for the DXVK Carafe source: the two frame limiter rows, the two compiler rows, and `dxvk.shaderCompilationMethod` as a row of its own. Sarek's row keeps its default |
| 0085 | horizon server window hooks | 277 | 98 % | in the fork; taking a hook out of its chain is a function of its own |
| 0087 | wow64 context of another thread waits until it stops | 8 | 37 % | in the fork; `wow64_wait_stopped` returns a status |
| 0088 | win32u counts shown dialogs | 5 | 100 % | in the fork |
| 0089 | raised exceptions are noted in the log | 30 | 96 % | in the fork, with the call made weak |
| 0090 | horizon server shown window exposes its children | 11 | 100 % | in the fork |
| 0091 | modules without unix calls are noted in the log | 9 | 100 % | ported in stage 1 |
| 0092 | xinput notes the first read and press | 16 | 100 % | in the fork |
| 0094 | winnx game window repainted after the screen follows it | 1 | 100 % | in the fork |
| 0095 | ntdll system locale follows the console language | 9 | 66 % | in the fork, as `wine_nx_console_locale` (`runtime.c`): the same table of languages, except that British English gives `en-GB` here and `en-US` in Carafe |
| 0096 | media ffmpeg parser for directshow | 808 | 100 % | ported in stage 1 (parser, codecs, filter registration) |
| 0097 | winnx screen follows the window on its own thread | 8 | 62 % | in the fork, with atomic loads and stores |
| 0098 | win32u a thread counts its waits for a reply | 6 | 100 % | in the fork |
| 0099 | horizon server low level input hooks | 94 | 95 % | in the fork, in upstream's later form (wheel data only for wheel messages) |
| 0100 | win32u the cursor stays on the programs screen | 3 | 100 % | in the fork |
| 0101 | horizon server clipping moves the cursor inside | 12 | 75 % | in the fork, in upstream's wider form (also after a move); the automatic merge would have added it twice |
| 0104 | runtime the log is written out before the game closes | 6 | 100 % | in the fork |
| 0105 | runtime the runtimes own dxvk is the system direct3d | 10 | 0 % | not taken: Carafe's DXVK is a release source here, in its own folder in front of system32 |
| 0106 | runtime the screen follows what a window shows last | 35 | 100 % | in the fork |
| 0107 | vulkan a swapchain gives the screen to gdi drawing | 152 | 86 % | in the fork, with the calls made weak and `nx_acquired` atomic |
| 0108 | winnx a picture the program draws in a window is reported | 25 | 64 % | in the fork, with the calls made weak |
| 0111 | winnx a fullscreen window in a scaled mode is placed between the bars | 11 | 100 % | in the fork |
| 0112 | win32u a window surface knows when the program drew into it | 15 | 86 % | in the fork: the hunk is in `dibdrv/dc.c`, the call weak |
| 0113 | win32u the system paints backgrounds and frames as itself | 12 | 100 % | in the fork |
| 0114 | win32u a scaled surface passes on that the program drew | 9 | 66 % | in the fork, with the call made weak |
| 0115 | compositor a layer tells whether a flush draws over its picture | 17 | 88 % | in the fork |
| 0116 | win32u a thread waiting for input is reported | 5 | 60 % | in the fork, with the call made weak |
| 0117 | runtime a program starts with the hint explorer gives | 1 | 0 % | not taken: Autorun starts a program with the hint to maximise, on purpose, so that a desktop program fills the screen. Carafe starts it as Explorer does, for games that restore their window after Direct3D fitted it; a game that needs that here would need it as a setting |
| 0118 | ntdll a system call keeps its callers registers | 158 | 97 % | in the fork; the stop request is read through `wine_nx_stop_state()` |
| 0119 | runtime a suspended thread is held in the kernel | 116 | 99 % | in the fork; a held thread looks again every 20 ms instead of waiting without end |
| 0120 | ntdll the server turns registers into contexts | 9 | 100 % | in the fork |
| 0121 | horizon suspending a thread works as on windows | 91 | 85 % | in the fork, with the runtime's calls made weak; the line that belongs to the server thread pool taken (stage 6) |
| 0122 | runtime fex thread state offsets | 11 | 100 % | in the fork |
| 0123 | runtime wow64 registers come from fex state | 42 | 100 % | in the fork (with the profiler's merge, stage 3) |
| 0124 | runtime checks the fex state offsets | 4 | 100 % | in the fork |
| 0125 | horizon a thread let go in a wait looks again before it sleeps | 0 | 100 % | in the fork |
| 0126 | ntdll an ended thread takes its address wait out of the table | 47 | 93 % | in the fork; the stop request is read through `wine_nx_stop_state()` |
| 0127 | thread an ending thread takes its address wait out first | 4 | 100 % | in the fork |
| 0128 | horizon kernel devices open read and answer ioctls | 170 | 98 % | in the fork |
| 0129 | ntdll a read done at once fills the status block | 6 | 100 % | in the fork |
| 0130 | xinput the pad state is shared with the hid gamepad | 12 | 83 % | in the fork; the profile is read through `wine_nx_input_profile_get` |
| 0131 | runtime the input poll feeds the hid gamepad | 2 | 100 % | in the fork |
| 0132 | registry volatile keys for devices | 16 | 100 % | in the fork |
| 0133 | dxvk the build manifest names the system dxvk | 11 | 9 % | not taken: there is no system DXVK to name; an installed release carries its version in its folder |
| 0135 | runtime a window picture publication event | 20 | 90 % | in the fork |
| 0136 | horizon sections use separate backing blocks | 73 | 100 % | in the fork |
| 0136 | sync window picture publication notifies the runtime | 12 | 100 % | in the fork |
| 0137 | horizon anonymous mappings use smaller backing pieces | 17 | 94 % | in the fork; one more condition, for views read on demand |
| 0137 | vulkan consumes the primary publication after image submission | 9 | 100 % | in the fork |
| 0138 | horizon section views claim addresses before anchors | 15 | 100 % | in the fork |

## How this was measured

- `git merge-base` of the fork and `carafe-patches` is `636913ca`; the trial
  merge was `git -c merge.conflictstyle=diff3 merge --no-commit b785476` in a
  scratch worktree, never committed.
- Missing features: the functions, macros, types and file-scope variables each
  file defines at `b785476` that it defines neither at `636913ca` nor in the
  fork (224 names in 11 files before stage 1).
- The table's two numbers come from `carafe/integration/tools/ledger.py`, which
  reads the patches in `carafe/integration/patches-from-carafe`; they are
  measured again whenever a stage changes the files a patch touches
  (`tools/refresh_table.py`; the `build` workflow fails when a row is out of
  date). `ledger.py --review -v` lists, for a patch, the lines the next point
  is about.
- The reading of a patch that is not found whole: each line it adds that the
  fork's file lacks is looked up in Carafe's file at `b785476`. A line that is
  not there either was rewritten by a later Carafe patch. A line that is there
  is a difference, and the fork's code at that place was read to see whether it
  does the same in another form. Of the 127 patches, that left two differences
  kept on purpose (`0105`, `0117`) and two patches not needed here (`0006`,
  `0133`). The last one that was waiting, `0023`, came with stage 6.
- The host tests are `horizon-wine/tests/check_*.py`. The sandbox they ran in
  has gcc but neither clang nor the Switch toolchain, so a full build is the
  `build` workflow's, and tests that need clang, an ARM64 host or the
  submodules fail there for that reason before and after a change alike; what
  counts is that the set of passing tests does not shrink.
