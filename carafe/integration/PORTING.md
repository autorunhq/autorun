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
   `autorunhq/autorun-horizon-dlls`. Carafe starts from that repository's set and
   replaces a few files in it. Its 25 FEX patches, 22 DXVK patches, four
   patches to Wine's own DLLs (`runtime/patches/wine-pe`) and dxwrapper can
   only reach a console through a DLL repository of this fork's own, or by
   shipping those few DLLs inside the card archive. That is stage 5 and needs
   a decision first.
5. **Licences.** Carafe's overlay (`runtime/overlay`) and its Autorun patches
   are LGPL-2.1-or-later like Wine; its FEX patches are MIT and its DXVK
   patches Zlib, as the files they change. Everything else in Carafe,
   including `runtime/orchestrator`, `runtime/dshow` and `runtime/media`, is
   under Carafe's own GPL-3.0-or-later. The runtime already links
   GPL-3.0-or-later code (LSFG-VK), so the orchestrator can be linked in, but
   its files keep that licence, say so at their top and are named in the
   credits of `README.md` and `documentation/technical.md`. Where a few lines
   of data sufficed (the DirectShow filters), they were derived from Wine's own
   sources instead and stay LGPL. The settings menu's texts in fourteen
   languages (stage 7) come from Carafe's app and are GPL-3.0-or-later too:
   they are one file, `horizon-wine/tools/carafe-menu-texts.json`, which says
   so in its first lines, and the menu's catalog built from it carries them.

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
| 7 | The menu's translations (Carafe has 15 languages), the remaining graphics hunks in `winnx_drv.c`, `winnx_opengl.c` and `win32u/vulkan.c`, read one by one | **Done.** The translations are in for the settings both menus have; the graphics hunks were read and none is missing here |

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

Added later: `tests/check_media_parser.py` plays files through the parser on
the build machine. `media_unix.c` is compiled as it is and linked with FFmpeg
7.1.5 configured with the lines of `cmake/Media.cmake` and nothing more, so a
file plays there only if the console's build has its demuxer and decoders. A
second FFmpeg build writes 31 files (every demuxer of the list, 31 of its 43
decoders) and decodes each one itself, and `tests/media_parser.c` calls the
parser through both call tables the way `winegstreamer.dll` does and holds
what comes out against that: every picture and every sample, each buffer's
time and length, what follows a move in the file (to a time, to a key frame,
past the end), a stop position, one stream alone, a stream switched off,
every picture of each of the thirteen picture formats, three sound formats,
compressed packets byte for byte, times counted from zero, reads of 1000
bytes, files cut short, files that are no media, a stream with no decoder,
and a disconnect while a read is not answered. 558 runs, under the address
and undefined-behaviour sanitizers, in the `build` workflow's `media` job.
60 errors were put into the parser one at a time: 59 made it fail, and one
changes nothing a file here can show (BGRA pictures without their fourth
byte set: no file here is see-through, and FFmpeg writes 255 there either
way).

It found four faults in what stage 1 took, all fixed with it:

1. AIFF and AU files opened with no stream to play. Carafe's list has their
   two demuxers but no decoder for big-endian samples, which is what they
   hold. `Media.cmake` has one more decoder line (`pcm_s8`, `pcm_s16be`,
   `pcm_s24be`, `pcm_s32be`, `pcm_f32be`, `pcm_f64be`).
2. The last picture of a bare MPEG video file came out without a time: the
   decoder holds it back until the end, where no packet is left to take a
   time from. It now starts where the picture before it ended.
3. Compressed MPEG packets came out of order among the streams. MPEG writes a
   time once for each packet of the file, not for each picture, and a packet
   without one was handed out after every packet that had one: pictures of
   0.86 s after sound of 2.5 s. A packet now has its place by the time it is
   decoded at.
4. After a move in an MPEG file, which has no list of its pictures, the first
   compressed packets were the end of a picture, then pictures that need an
   earlier one, and a frame of sound with the end of another before it. The
   pictures now start at one that needs no other, from its first byte, and
   the frame of sound is cut to itself. (No file could be told to put a key
   picture right behind the place a move lands in, so that cut is checked on
   bytes made in the test.)

What the check cannot say: twelve decoders are not played because FFmpeg has
no encoder for them (H.264, WMV3, VC-1, WMA Pro and Lossless, MS-MPEG4 v1,
Indeo 3 to 5, MP1, MP3, GSM); a decoder of the list that is neither played
nor among those twelve fails the check, so the list cannot grow unnoticed.
FFmpeg is plain C there and has its ARM64 routines on the console. And
nothing of Windows is in it: neither quartz, nor the Windows Media reader,
nor a program.

The decoder beside it. `media_unix.c` had one half before the port: the
decoder behind Media Foundation's H.264 and AAC decoders, this fork's own
code. Stage 1 put the parser into the same file, so that half was checked the
same way: `tests/check_media_decoder.py`, 358 runs in the same job, with the
same two FFmpeg builds. Five AAC files that hold a tone go through it packet
by packet. Every sample is the one the other build decodes, as float and as
16 bits, with the channels as they are or mixed, into buffers of any size,
with any queue length, through both call tables, from frames with a header
and without, and from packets that hold two frames. Every buffer's time is
held against the tone itself: a sample is a point of the sine at the time its
buffer says, so one sample out of place shows. That found three faults, none
of them the port's, all fixed with it:

1. A packet the decoder could not read stayed first in the queue. Every read
   after it failed, and once the queue behind it was full every packet was
   refused: one damaged packet ended the sound for good. It is now left out;
   what it held is lost and the packets after it play at their times (checked
   with five kinds of damage, at the start, in the middle and at the end, up
   to twenty packets in a row).
2. A stream whose channels or rate change on the way was refused from there
   on. The output stays what was asked for: the rest of the old sound comes
   out, and the new sound is converted to the same output.
3. Sound played at another rate than the file's was stamped up to one sample
   of the file off (23 millionths of a second at 44.1 kHz), since what the
   converter holds back was counted in whole samples.

`tests/media_decoder.c` was there before and run by nothing. On AAC in MP4 it
failed by its own fault (it went back to time zero, which in such a file is
the second packet, and compared that with the first pass); it is rewritten as
the test this check runs. 63 errors were put into the decoder one at a time:
61 made the check fail, and two change nothing AAC can show (the end not
passed on to FFmpeg's decoder, which holds no AAC frame back; a frame not
released when the decoder is emptied, which the next frame releases).

What that check cannot say: no picture goes through the H.264 half, for the
same lack of an encoder (it is created, refused wrong formats, and emptied;
`media_decoder file` plays a file brought from elsewhere). The AAC is the
plain kind; no HE-AAC. One line has no test: a frame the converter refuses is
dropped rather than offered again, and nothing here makes the converter
refuse one.

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

Added later: `tests/check_horizon_suspend.py` runs the server's suspend,
resume, thread context and terminate handlers and the stall report. It had
stopped building before this work (the handlers hold a reference the test's
thread did not have), and stages 2 and 6 put more into those functions that
it did not know. It builds again, is in the `build` workflow, and now also
checks that the event ring hears of every suspend, resume and terminate and
of each select that sits out a suspension; that the stall report copies a
thread's stack while the thread is paused and looks for return addresses in
it only after the thread runs again and the lock is free; and that a shared
server thread tells the pool before it waits for a thread outside the lock.
Nine errors put into `horizon.c` and `thread_profile.c` each made it fail.

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
with the same tests failing as before; the changed C files and the launcher
compiled for AArch64; the `build` workflow.

Added later: the core the orchestrator keeps for the busiest thread had no
check of the runtime's side (`wine_nx_thread_reserve_core` and the shader
threads' mask in `thread_profile.c`; `orchestrator_switch.c` only sees that it
is asked for). `tests/check_core_affinity.py`, which had also stopped
building, builds again, is in the `build` workflow, and checks it: shader
threads moved before and named after stay off the kept core and get it back,
their server threads follow, a thread with an affinity of its own is left
alone, the only core there is cannot be kept, and nothing changes when the
kernel does not answer. Eight errors put into the code each made it fail.

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

The default renderer was not touched: a program with no `d3d` line runs on
DXVK (`runtime.c`), and the settings menu's row that writes nothing reads
"DXVK". A game added while the launcher's "Give a new game DXVK" is off gets
`d3d=wine` written, which the menu shows as the choice it is.

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
| Carafe's four changes to Wine's own DLLs (`runtime/patches/wine-pe`: `wow64.dll`, both `ntdll.dll`, both `dinput.dll` and `dinput8.dll`) | This tree's Wine source has had them since the base commit, but a console runs the DLLs of `autorunhq/autorun-horizon-dlls`, which are built without them. Read in its files at `67e2303`: `wow64.dll` does not name `BTCpuSuspendLocalThread`, neither `ntdll.dll` holds `0x46555458`, and all four DirectInput DLLs still hold `nx_joystick_create_device`. So on a console `0126` and `0127` are never handed a wait to take out, a 32-bit thread is suspended without asking FEX, and DirectInput reads the pad through `joystick_nx.c`, not the HID gamepad of `0128` to `0131`. Nothing here builds or releases these seven DLLs yet |
| dxwrapper | Carafe builds `elishacloud/dxwrapper` with its five patches using MSVC and the Windows SDK, fetched by `mstorsjo/msvc-wine` under Microsoft's licence; nothing here builds it |
| Wine Mono | A ready archive, `wine-mono-11.3.0-arm64.tar.xz`, which `tools/wine_mono_payload.py` pins and stages; no build passes it to a card and the launcher has no download for it |
| The WinRT classes | No new DLL: registry lines for DLLs system32 already has. `classes.reg` comes from the DLL repository, so the runtime would have to add the lines itself |
| Patch `0006` | Its Windows half is rebuilt Wine DLLs; not needed on the fork's own paths, see the row for it below |

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
- Leaving a program, as read in the code:
  - Pool threads are registered with the runtime as server threads (`'s'`),
    and `wine_nx_threads_program()` counts every registered thread but the
    presenter, so the quit waits for them like for any other thread; if one
    did not stop within five seconds the quit would be called off and the
    program would go on (`wine_nx_quit_point`, `runtime.c`).
  - An idle pool thread looks for a quit every quarter of a second
    (`horizon_cond_wait_quit`) and stops with the pool's lock released. One
    inside a request waits in the same three places a per-client thread waits
    in (`horizon_server_sleep_locked`, `horizon_server_wait_thread_locked`,
    `horizon_sync_sleep_locked`); every call of the three is followed by
    `horizon_server_quit_check_locked`, which stops the thread with the
    objects' lock released.
  - A thread that stopped ends itself and leaves its handle in
    `quit_joinable[]`, which `return_to_launcher` joins; a pool thread that
    retired earlier is on `horizon_server_zombies`, joined by the next thread
    that ends and at the latest by `horizon_release_thread_stacks()`.
  - The pool's counters are not put back, and need not be: `main` runs one
    program and returns, the loader maps the runtime's file again
    (`envSetNextLoad`; `hbl/source/main.c` reads it from the card) and libnx
    clears its `.bss` before `main` (`__nx_dynamic`). When the loader cannot
    take the process back, the application closes instead.

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

### Stage 7: the menu in the console's language, and the graphics hunks: done

The settings menu over a running game is Carafe's, and its catalog
(`/carafe/menu.txt`, built by `horizon-wine/tools/make-carafe-menu.py` and
embedded in the runtime) has room for fifteen languages: Russian, English,
Simplified Chinese, Japanese, Korean, Traditional Chinese, French, Canadian
French, German, Italian, Spanish, Latin American Spanish, Dutch, Portuguese
and Brazilian Portuguese. The menu's own words (buttons, messages) were in
all of them already. The catalog was written in English only, so every
setting, value and page showed in English whatever the console's language.

What was done:

- `carafe/integration/tools/menu_texts.py` reads Carafe's list of settings
  (`app/crates/carafe-core/src/settings_catalog.rs`) and its translation files
  (`app/ui/src/shared/i18n/locales/*.json`) from a checkout of Carafe and
  writes `horizon-wine/tools/carafe-menu-texts.json`: 92 settings and 286
  texts, each in the fifteen languages, keyed by what a setting is here (the
  settings file, the key, and a value's text in that file). The file names
  the Carafe commit it was made from; the `build` workflow fetches that
  commit and fails when the file is not what the tool writes from it.
- `make-carafe-menu.py` puts those texts beside Autorun's English. The
  settings, their order, their pages, their values and their defaults are
  untouched, and so is every English text but two (below).

How a text of Carafe's is matched to a row here:

| Row | Matched by | Rows that got the fourteen languages |
|---|---|---|
| A setting's name and hint | the same key in the same settings file | 91 of 141 |
| A value | the same text in the settings file (DXVK's strings without their quotes) | 297 of 546, the next two rows included |
| DXVK's "Default" and the frame limit's "Off" (rows that write nothing) | Carafe's row that writes nothing: "Default", "No limit" | |
| Other rows that write nothing (`vsync`, `upscaling`, `upscaling-sharpness`, `dxvk-hud`, the three `lsfg` settings, `large-pages`, every FEX option) | Carafe's label for the value such a row stands for, which the generator names for each setting (`unset=`; for FEX the default in `fex_options.h`) | |
| Sections, the two groups, the 24 controller outputs | the English is word for word the same; the generator stops if it is not | 4, 2, 24 |
| Pages | by hand, see below | 8 of 8 |
| Presets | the same three presets | 3 names, 2 hints |
| "What to try" suggestions | the same kind of failure and the same setting | 5 of 11 |
| Keys | the mouse's 13 actions, `0x100` to `0x10c` in both | 13 of 120 |

Three things about the values were not obvious and are held by the check:

- A row that writes nothing here often stands for a value Carafe writes.
  "Enabled" of `vsync` is Carafe's `1`; "Off (Bilinear)" of `upscaling` is
  Carafe's `off`, although Carafe's own default there is FSR.
- Carafe names `FEX_DISABLEL2CACHE` "Second jump cache (L2)", so its `1` is
  "Off" and its `0` is "On", the reverse of the English row "Disable L2
  lookup cache". Name and values are taken together, so each language is
  consistent in itself.
- The English "Default (False / Auto)" of a DXVK option also says what the
  default is. The other languages have Carafe's word alone: with the note
  added, the longest rows of four languages (fr-CA, es, es-419, pt) no
  longer fitted beside the setting's name.

Pages. Autorun sorts DXVK's options into five pages by the option's name,
Carafe into nine by hand, so a page here holds settings from several of
Carafe's. Each page has the title of the Carafe page most of its settings
come from:

| Page here | Carafe's title |
|---|---|
| Memory | Memory (`dxvk-memory`) |
| Frames & latency | Frames (`dxvk-frames`) |
| Shaders & quality | Shaders & Quality (`dxvk-shaders` and `dxvk-quality`, joined) |
| Display & GPU | Display modes (`dxvk-display`) |
| Compatibility | Fixes (`dxvk-fixes`) |
| Basic | General (`fex-general`, the same seven options) |
| Ordering & precision | Memory ordering (`fex-memory`, the same six options) |
| Cache & topology | Compatibility and caches (`fex-compat`) |

What stays English in every language, and why:

- The 49 settings Carafe's menu does not have: the four rows of the next
  part, `button-layout`,
  `mouse-speed`, `picture`, `d3d`, `dxvk-source`, `cpu`, `four-cores`,
  `sync`, `lazy-file-views`, 26 DXVK options and 10 FEX options.
- `dxvk.shaderCompilationMethod`. Carafe's hint says what "Default" does in
  Carafe's DXVK build (`async`); here the option is DXVK-Sarek's too, where
  the default is `dyasync`.
- "Global setting" of `windows`, `verbose` and `profile`. Carafe has
  "Automatic" for the first and nothing for the others, and neither means
  "as the launcher's settings say".
- The hint of the Fast preset. Carafe's Fast also turns the L1 cache's
  sizing off and its hint says so; the preset here does not, and was left as
  it is.
- Labels that are numbers or names (`30 fps`, `FSR 1.0`, `1000`, `mtrack`),
  six suggestions Carafe does not make, and the keyboard's key names.

English reads as before. Two "Default" notes were wrong for options that
two DXVK builds read with different defaults and now name both:
`dxvk.shaderCompilationMethod` "Default (dyasync / async)" and
`d3d9.deviceLocalConstantBuffers` "Default (False / Auto)". A console set to
a language outside the fifteen gets English.

Differences between the two menus that were seen and left alone: Carafe's
dead zones also offer 2 % and 8 %; its upscaling defaults to FSR; its Fast
preset turns the L1 cache's sizing off; its nine DXVK pages.

The catalog grew from 55 KB to 279 KB, in the runtime's file and in memory
while the menu holds it (the reader takes up to 1 MiB). The built runtime now
carries GPL-3.0-or-later texts (point 5 above).

Checked on the host: `tests/check_menu_languages.py` builds the catalog and
has the menu's own reader (`carafe_settings.c`, through
`tests/menu_languages.c`) show it under each of the fifteen console
languages. What the reader shows is the catalog's text for that language,
text for text; in English nothing is Carafe's wording; in the other
languages every text is Autorun's English or one of Carafe's texts for that
language; each matched setting has Carafe's name and hint, each value
Carafe's label for the value it stands for, and a settings file that holds
that value shows that row (the reader's own `valueIndex`). Fourteen mistakes
made in the generator on purpose (a wrong value behind a "-" row, the
languages shifted by one, a text taken for the excluded setting, quotes not
stripped, and so on) each made it fail. With the fonts the menu embeds
(M PLUS 1p, 16 px), the widest row of name and value is 603 of the list's
640 pixels in the twelve languages those fonts cover (measured once with
FreeType, not part of the check).

Not checked, because it needs a console: how each script looks there
(Chinese and Korean come from the console's own fonts, which were not
measured), whether a long hint leaves room for every value on the card at
the right, and the menu itself in any language but English.
`tests/controls_menu.c`, when given the built catalog, stops at its
registry editor part (line 716); it did so before this work too, with the
catalog as it was, and is not run by the `build` workflow. Its count of
settings was brought up to date (141).

The menu and Carafe's builds. Stages 4 to 6 gave the launcher new rows for a
program; the menu over the running game writes the same file and did not
have them. It showed a program set to Carafe's DXVK as the bare word
`carafe`, with no way back to it once the row was moved, and it listed every
DXVK and FEX option whatever build the program ran on. Now:

| In the menu | What it does |
|---|---|
| **DXVK build** has **DXVK Carafe** | writes `dxvk-source=carafe` |
| **FEX build**: Autorun, Carafe (System, shown while the CPU translator is FEX) | writes `fex-build=carafe`; the version stays the launcher's choice (`fex-version`) |
| A DXVK option | shown while **DXVK build** is on a build of which some version reads the option: 66 options for official DXVK, 50 for Sarek, 64 for DXVK Async, 76 for Carafe's |
| The 12 FEX options only Carafe's build reads | shown while **FEX build** is on Carafe |
| **Shared server threads** | `server-pool=1`; greyed out unless thread synchronization is Horizon |
| **Thread orchestrator**, **Lock wait boost** | `orchestrator=1`, `lock-boost=1` |

All take effect at the next start, like the rows beside them. A build that
is not on the card is downloaded when the launcher next starts the program;
a start that does not go through the launcher runs on the stock FEX, or on
Wine's Direct3D, as it already did for a missing DXVK (`runtime.c`). The
menu does not know whether the program is 32-bit, so **FEX build** shows for
64-bit programs too, where the runtime keeps the stock FEX; the row's text
says it is for 32-bit programs. The menu lists options by build, not by the
installed version, which the launcher also looks at. With **DXVK + VKD3D**
chosen the launcher runs official DXVK in place of DXVK-Sarek, as it did
before (`launcher_settings_read`); the menu still lists Sarek's options then.
These rows have no text in Carafe and are English in every language.

`tests/check_menu_builds.py` asks the menu's reader which rows it shows for
each choice and holds that against the code the launcher and the runtime
use: `dxvk_option_supported` over every version a release can have, for
each of the 81 options and four builds; `nx_fex_option_carafe`; and the
launcher's reader of a program's file for what each new value turns on.
Fourteen mistakes made in the generator on purpose: twelve made it fail,
and two change nothing with today's options: the catalog made with either
of them is the same file byte for byte (both are parts of the rule for
DXVK-Sarek that no option needs at present).

The graphics hunks. Eight of Carafe's patches have lines in
`dlls/win32u/winnx_drv.c`, `winnx_opengl.c` or `vulkan.c` that are not in
the fork's files as written: 84 lines (`ledger.py --review -v` lists them).
Each was read against the fork's code. None is a change the fork lacks; in
every case the fork has the same thing in another form, and in five of them
it has more:

| Patch | Carafe's lines not found | What the fork has in their place |
|---|---|---|
| 0010 | 2, `vulkan.c` | the same call, through a weak symbol |
| 0012 | 1, `winnx_drv.c` | the flush of mouse motion, made only after something was sent. The same reaches the server: win32u keeps motion per thread and an empty flush sends nothing (`accum_mouse_motion`, `send_mouse_motion` in `input.c`) |
| 0014 | 6, `winnx_opengl.c` | the same counter with atomic loads and stores; a drawable that is not the screen swaps without it |
| 0042 | 43, `vulkan.c` | the menu's Vulkan pass, grown to draw the pointer too: a second texture and descriptor set, a viewport set per draw, and a fence per image so a command buffer is recorded again only after its last use ended |
| 0043 | 1, `winnx_opengl.c` | the viewport line, in a routine that takes any rectangle (it also draws the pointer) |
| 0097 | 3, `winnx_drv.c` | the same, with atomic loads and stores |
| 0107 | 20, `vulkan.c` | weak calls and an atomic `nx_acquired`; a present that is refused waits on all of the program's semaphores, not the first 16; and it gives the driver the driver's own fence. Carafe passes the program's fence handle, which in this Wine is a wrapper object (`vulkan_fence_from_handle`) |
| 0108 | 8, `winnx_drv.c` | weak calls; the test for a lit pixel stays inside the picture's rows |

Beyond those lines the fork's three files differ from Carafe's by the
fork's own later work (a window's picture scaled to the screen, the pointer
drawn over Vulkan and OpenGL, more log lines); nothing there comes from
Carafe. No file changed for this part. It is a reading of the code: none of
it was run, and what the 0107 fence does on a console is not known.

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
| 0006 | virtual 32bit address space | 23 | 43 % | in the fork's own form, for a game's 32-bit forwarder (`forwarder.c`): `relocate_user_shared_data` tries below 2 GB first, and the heap page at 0x7ffe0000 is taken out of the runtime's heap and made inaccessible (`horizon_hide_user_shared_data_address`), so the PE side's direct reads of it fault into horizon.c's redirect whether or not its DLLs read `wine_nx_user_shared_data`; `thread_data_limit_low` as the patch's |
| 0007 | low window 32bit guest range | 10 | 100 % | in the fork |
| 0008 | runtime screen until first frame | 29 | 96 % | in the fork; the notice before the program starts has one more condition (`wine_nx_watch_window_pictures`) |
| 0009 | winnx vulkan reserve screen | 2 | 100 % | in the fork |
| 0010 | vulkan take screen at swapchain | 3 | 33 % | in the fork, with the call made weak |
| 0011 | winnx opengl first frame | 22 | 100 % | in the fork |
| 0012 | winnx flush mouse motion | 4 | 75 % | in the fork; the flush is made only after a move or a button was sent, and an empty flush sends nothing, so the server sees the same |
| 0013 | audio source rate frames | 30 | 100 % | in the fork |
| 0014 | winnx opengl clear after loading screen | 29 | 79 % | in the fork, with atomic loads and stores |
| 0015 | win32u screen size of a window | 15 | 100 % | in the fork |
| 0016 | opengl present at window position | 31 | 100 % | in the fork |
| 0017 | winnx screen follows game window | 72 | 100 % | in the fork |
| 0018 | horizon mouse hit test in program coordinates | 39 | 94 % | in the fork; it falls back to the first active monitor instead of the first |
| 0019 | horizon thread budget and server requests | 91 | 96 % | taken (stage 2); its `[BUDGET]` line is now the one of `0023`, with `pool=` |
| 0020 | horizon thread creation waits for a place | 42 | 59 % | in the fork as horizon_thread_may_wait and HORIZON_THREAD_WAIT_*; the wait counters taken (stage 2) |
| 0021 | horizon handle fd on the calling thread | 4 | 100 % | in the fork |
| 0022 | server handle fd on the calling thread | 1 | 100 % | in the fork |
| 0023 | horizon server thread pool | 180 | 98 % | taken (stage 6), behind a program's setting; the pipe's count and writer's end are stored atomically |
| 0024 | runtime tls holders give their place | 31 | 87 % | in the fork; the lines not found were rewritten by later Carafe patches |
| 0025 | horizon thread creation takes a tls holders place | 6 | 33 % | in the fork as horizon_tls_give_place |
| 0026 | thread join an ended thread without waiting | 23 | 100 % | in the fork |
| 0027 | horizon thread creation at the limit skips the kernel wait | 41 | 95 % | in the fork, as `horizon_thread_may_wait` |
| 0028 | horizon thread creation waits longer for a place | 39 | 61 % | in the fork (20 s wait, 2 ms then 20 ms steps); the caller report taken (stage 2) |
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
| 0096 | media ffmpeg parser for directshow | 808 | 98 % | ported in stage 1 (parser, codecs, filter registration); thirteen lines differ since the host check: where a packet without a time has its place, MPEG after a move, the last picture's time |
| 0097 | winnx screen follows the window on its own thread | 8 | 62 % | in the fork, with atomic loads and stores |
| 0098 | win32u a thread counts its waits for a reply | 6 | 100 % | in the fork |
| 0099 | horizon server low level input hooks | 94 | 96 % | in the fork, in upstream's later form (wheel data only for wheel messages) |
| 0100 | win32u the cursor stays on the programs screen | 3 | 100 % | in the fork |
| 0101 | horizon server clipping moves the cursor inside | 12 | 75 % | in the fork, in upstream's wider form (also after a move); the automatic merge would have added it twice |
| 0104 | runtime the log is written out before the game closes | 6 | 100 % | in the fork |
| 0105 | runtime the runtimes own dxvk is the system direct3d | 10 | 0 % | not taken: Carafe's DXVK is a release source here, in its own folder in front of system32 |
| 0106 | runtime the screen follows what a window shows last | 35 | 100 % | in the fork |
| 0107 | vulkan a swapchain gives the screen to gdi drawing | 152 | 86 % | in the fork, with the calls made weak and `nx_acquired` atomic; a refused present waits on every semaphore and passes the driver's fence, not the program's handle |
| 0108 | winnx a picture the program draws in a window is reported | 25 | 64 % | in the fork, with the calls made weak and the pixel test kept inside the picture |
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
