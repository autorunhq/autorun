# Wine-NX Probe And Runtime

`nx-wow64-dynarec-33` removes OpenTTD's second cursor. On build-32 hardware,
OpenTTD loaded almost instantly, but the runtime's arrow was drawn over the
cursor OpenTTD draws itself.
- Wine tells the display driver about cursor changes with `WM_WINE_SETCURSOR`,
  a hardware message the server queues when the visible cursor changes. The
  Horizon server does not queue it, and the Switch driver had no `SetCursor`
  callback, so the arrow was drawn in every frame.
- The driver now reads the cursor handle and show count from the shared input
  state (`get_shared_input`) each time it polls input. The arrow is hidden
  while the show count is negative, or when a program that had set a cursor
  sets none. Before any program sets a cursor, the arrow is shown.
- `wine_nx_cursor_show` stops drawing the arrow and presents the change;
  cursor-only frames are skipped while it is hidden. `SetCursor` is plugged in
  for a server that queues the message.
- `tests/check_pointer_events.py` covers no cursor yet, a class cursor,
  `SetCursor(NULL)`, `ShowCursor(FALSE)`, the callback and an unreadable state.

`nx-wow64-dynarec-32` stops re-checking translated x86 code on every jump.
Build 31 showed file access was not the cost:
- By 45 s OpenTTD had made 29,314 reads, 97% served from the new cache, with
  634 ms spent inside `NtReadFile`. Loading took as long as on build 30.
- `native_entries` tracked `syscalls` (70,383 and 70,661), so the remaining
  time was x86 execution between system calls.

The cause:
- The dynarec build forced every translated block to `always_test`. Box64 then
  points each block's jump table entry at its `jmpnext` stub instead of its
  code.
- So every jump, call or return between blocks went through `native_next`,
  `LinkNext` and `DBGetBlock`, which hashed the block's x86 bytes before running
  it. Every iteration of a loop spanning blocks, such as OpenTTD's sprite
  decoding, paid a C call and a hash.
- Box64 normally write-protects translated guest pages instead. This port does
  not, which is why every entry was checked.

The fix:
- Blocks link directly again. `cmake/Box64Core.cmake` no longer forces
  `always_test`. It records the largest block size and counts hash validations
  (`block_tests`).
- winebox64 exports `BTCpuNotifyMemoryFree`, `BTCpuNotifyUnmapViewOfSection`,
  `BTCpuNotifyMemoryProtect`, `BTCpuFlushInstructionCache2` and
  `BTCpuFlushInstructionCacheHeavy`.
- Through a new unix call (winebox64 ABI 3), they reach
  `wine_nx_box64_invalidate`. It frees the blocks in freed or unmapped memory,
  and marks those in re-protected or flushed memory so their next entry checks
  the hash. It walks the jump table from the largest block size before the
  range, skipping unused table levels.
- Code that a program changes without such a report keeps its old translation.
- `[PROGRESS]` adds `block_tests`.

Tests:
- In `tests/box64_execution.c`, a loop that calls into another block 5,000
  times makes fewer than 50 block validations.
- Code rewritten at a reused address and reported runs the new code, and so does
  code written after its blocks were freed. The existing tests now report the
  code they rewrite.
- `tests/wow64_box64_unix.c` checks the new call's version, size, address limit
  and arguments.

`nx-wow64-dynarec-31` caches reads from the SD card. On build 30, OpenTTD's
white screen lasted about 70 seconds, 40 of them loading graphics: 29,314 reads
at about 1,000 per second. OpenTTD reads each sprite with a seek and a 4 KB
read, and libnx sends every `read()` to the FS service as an `fsFileRead`
request. Horizon has no page cache or file mapping, and libnx has no file data
cache. Nintendo's SDK keeps one in its fs client library; Atmosphère's
reimplementation of that path is still a stub. Dolphin's disc readers cache
aligned blocks the same way.
- `source/sd_read_cache.h` keeps 8 aligned 128 KB chunks per file, 32 MB in
  all. A read inside a chunk is a copy, a miss reads the whole chunk in one
  request, and the least recently used chunk is replaced, as in Dolphin's
  `SectorReader`. Reads of 64 KB or more go straight to the file.
- `source/sd_cache.c` installs it in place of libnx's sdmc device before the
  runtime opens a file, keeping the device index that paths without a device
  name use. Wine's file calls, image mapping, NLS and fonts all go through it.
- Only read-only files are cached. Opening a path for writing, renaming,
  removing or truncating it stops caching of every open file with that path, and
  a file opened while a writer has its path open is not cached. Files opened
  before the cache was installed pass through. Data is copied to the caller
  after the cache lock is released, so a fault on the caller's buffer cannot
  happen while it is held.
- `[PROGRESS]` adds `read_ms` (time inside `NtReadFile`), `sd_reads` and
  `sd_ms` (requests to the FS service and their time), `cache_hits` and
  `syscalls`. Load time left over, beyond `read_ms`, is x86 execution,
  translation and other system calls.
- `tests/sd_read_cache.c` covers OpenTTD's read pattern, the end of a file,
  least recently used replacement against 20,000 random reads, failed
  requests, the memory limit and open-file rules.
- `source/sd_write_buffer.h` keeps a window of each file being written, up
  to 1 MB, with the bytes written into it and the file position the program
  sees; writes of 64 KB or more go straight out. Writes and seeks inside the
  window stay there, and sending it out writes only what changed since the
  last time and keeps it. Dolphin's Switch port puts a 16 KB stdio buffer on
  its NAND files for the same reason, but stdio sends it out on every seek.
  `sd_cache.c` holds writes only for a file open once and not for appending,
  drops the window before a read, open, rename, delete, seek from the end or
  close of that file, adds it to the size stat and fstat report, and sends
  what changed every 200 ms from the log flusher. F.E.A.R. writes its 262 KB
  checkpoint a value at a time, going back to fill in each length, which
  stalled the start of every level for a minute; `tests/sd_write_buffer.c`
  turns that pattern's 52,500 requests into one per 200 ms send.
- `source/sd_stat_cache.h` keeps what stat said about 512 paths, and that a
  path is not there. libnx answers a stat with up to five requests, and
  F.E.A.R. asks for PunkBuster's missing `pb/pbcl.dll` 340 times a second,
  each one a lookup of every directory on the way. `sd_cache.c` forgets a path
  created, written, truncated or deleted, and everything on a rename or a
  directory removed. `[PROGRESS]` shows `sd_writes`, `sd_write_ms`,
  `writes_held`, `sd_stats` (asked of the card), `stat_hits` and why held
  writes went out (`flushes=` a write elsewhere / the file read or opened /
  a seek from its end / closed / the 200 ms send).
Hardware results are pending.

`nx-wow64-dynarec-30` fixes OpenTTD's white screen and idle hang.
Build 29 hardware showed the same end as build 28: five
`NtQueryPerformanceCounter` calls, then an `NtDelayExecution` that never
returned, even with `svcSleepThread`. The sleep was not the problem; its length
was.
- On the Switch, `monotonic_counter()` fell back to `gettimeofday()` minus
  `server_start_time`. The Horizon server never reports that start time, so
  `QueryPerformanceCounter` counted 100 ns ticks since 1601, about 1.3e17.
- MSVC's `steady_clock::now()` returns that counter times 100 at Wine's 10 MHz
  frequency, which overflows to a negative time.
- OpenTTD's loop compares `now >= next_draw_tick` against time points that start
  at 0. That was never true, so it never drew, and it slept until the next tick,
  about 158 years away, clamped to a `Sleep` of about 49 days.
- The counter now counts from boot, like Windows, using `horizon_interrupt_time()`
  (`armGetSystemTick`), which also drives the shared data `InterruptTime`.
- `tests/check_qpc_base.py` compiles `monotonic_counter()` as the Switch build at
  3 days of uptime in 2026 and checks MSVC's arithmetic and OpenTTD's first draw
  tick. `--baseline` restores the old counter and fails.

On hardware, build 30 shows OpenTTD's title screen. Its `[PROGRESS]` lines put
the white screen at about 70 seconds:
- 5 s: the window is created (163 reads).
- 5–45 s: graphics loading, 29,314 reads at about 1,000 per second, with three
  bursts of short-lived CRT worker threads.
- 55–75 s: the first frames (17 by 75 s).
- 85–95 s: 182 frames, while dynarec entries rose from 107,399 to 203,330 as
  drawing code was translated for the first time.
- 145–155 s: about 43 frames per second.
Next targets: fewer SD-card requests for OpenTTD's one-seek-per-sprite reads, and
translation time on first execution.

`nx-wow64-dynarec-29` replaced the select()-based `Sleep()` (below), but the
hang remained; the entry's cause was wrong. The change stays: sleeping should not
depend on the BSD socket service. The original entry follows.

`nx-wow64-dynarec-29` fixes `Sleep()` never returning, which left OpenTTD on a
white screen with an idle CPU. In a longer build-28 verbose run, OpenTTD
finished loading:
- It read the base graphics, the title game and the AI folders, then started
  and joined 17 worker threads.
- Its game loop called `NtQueryPerformanceCounter`, then `NtDelayExecution`, and
  that call never logged `done`. Nothing else ran.

Wine's `NtDelayExecution` sleeps with `select( 0, NULL, NULL, NULL, &tv )`. On
libnx that is `bsdPoll` with no descriptors, a request to the BSD socket
service, which has only a few sessions. On the Switch it now calls
`svcSleepThread`, rechecking the time at least hourly, as the winebox64_nx
runtime does for Wine's descriptor-less `pselect6`. The built
`NtDelayExecution` calls `svcSleepThread` and no longer calls `select`. Notepad
never slept this way; it waits in message waits.

Build 29 also reports progress without verbose logs. An earlier build-28 run,
with verbose logs on, was stopped after about 105 seconds (21 reports, 5 s
apart) while still loading:
- It read the base graphics (up to 6,141 reads in one interval), then the title
  game `opntitle.dat`, scanned `ai`, and was reloading GRF sprites with one seek
  per read.
- Only two requests failed: an AFD ioctl from network setup (0x120354) and a
  console ioctl. Nothing was blocked.
- Build 27 moved every periodic report behind verbose logs, so a run without
  them shows nothing while the screen is white. Every 10 seconds, when something
  changed, the runtime now logs `[PROGRESS] <seconds>s reads=<n> frames=<n>
  native_entries=<n>`. Reads are completed `NtReadFile` calls, and frames are
  frames queued to the display. Reads that stop with frames rising means
  drawing; neither rising means waiting or computing.

`nx-wow64-dynarec-28` fixes the cursor not moving at all in OpenTTD on build 27.
Build 27's display driver started a background thread that polled the controller
and sent the input with `NtUserSendHardwareInput`. That thread is a plain libnx
pthread without a TEB. At the first stick movement, the server call dereferenced
the NULL TEB (`[EXC] ... far=0x384`, in `server_call_unlocked`, `ldr w19,
[x0, #900]` after `NtCurrentTeb`), and the thread was parked before it presented
the moved cursor. `ProcessEvents` skipped polling while that thread was
marked as started, so nothing read the controller again.
- The background thread now only polls and presents, with no Wine calls. The
  cursor keeps moving while a program is too busy to pump messages, such as
  OpenTTD loading its sprites.
- `ProcessEvents` always polls as well and sends the input from a Wine thread.
  The runtime remembers every button pressed or released between two calls
  (`pointer_buttons` in `pointer_cursor.h`, `wine_nx_pointer_take`). A click
  made while the program was busy is delivered as down, then up.
- Tests: `tests/pointer_cursor.c` covers the button edges between takes, and
  `tests/check_pointer_events.py` runs the driver's event translation,
  including clicks between calls. The fixture had stopped compiling in build 27.

The same build-27 run, with verbose logs, was still loading GRF files after
more than 80 seconds: 154 opens and 9,248 reads, each syscall written to the
SD card twice. Judge OpenTTD's loading time with verbose logs off.

`nx-wow64-dynarec-21` fixes positioned I/O moving the ordinary file cursor.
Build 20 hardware shows no invalid-frame unwind errors and confirms native
stack rebinding. Its language reads return 4096 bytes, but their first words
match byte 23 of the original language files rather than the LANG header.
Wine's is_device_placeholder() reads 23 bytes with pread(), and the Horizon
shim implemented that with seek/read without restoring position. pread and
pwrite now save/restore position under a shared mutex; ordinary concurrent
read/write on the same descriptor still need caller coordination.
`tests/check_positioned_io.py` checks the actual shims: the 23-byte probe then
ordinary header read, nonzero cursors, EOF, short/zero reads and error paths.
Its --baseline mode reproduces the skipped header. Hardware confirmation of
OpenTTD advancing past language initialization is pending in build 21.

`nx-wow64-dynarec-20` addresses the native unwind stack mismatch seen in
OpenTTD build 19. ARM64 PE entry points and callbacks run on libnx's thread
stack, but the native TIB still described the separately allocated Wine stack.
Before PE entry/callbacks, native bounds now come from threadGetSelf()'s
stack_mirror/stack_sz, only if they contain the current native frame. Guest
stack bounds, CPU-reserved storage and allocation ownership are unchanged.
`tests/check_native_stack.py` covers these invariants and invalid ranges;
hardware confirmation of RtlUnwindEx remains pending.

The language failure remains under investigation. All 66 staged language packs
and openttd.exe match the original 15.3 archive byte-for-byte; their header
version is 0x2ad109ab. Build-19 logs show successful file opens and reads but
do not expose returned byte counts or data. The first logged native unwind
failure follows the language scan during GUI calls, so causality is unproven.
Build 20 logs the first 256 native read completions as NXREAD in verbose mode,
including length and first eight bytes, and enables OpenTTD misc debug level 3.

`nx-wow64-dynarec-19` gets OpenTTD past DLL initialization, based on what
build 18 logged on hardware. All imports loaded, and then:
```
err:module:find_forwarded_export module not found for forward 'cryptbase.SystemFunction036' used by L"C:\\windows\\system32\\advapi32.dll"
err:opengl:DllMain Failed to load unixlib, status 0xc0000008
err:module:loader_init "OPENGL32.dll" failed to initialize, aborting
```
- opengl32's WoW64 table from build 17 was found, but its first call,
  `process_attach`, returned `STATUS_INVALID_HANDLE`. The x86 unix call gate
  (`wine_nx_call_ntdll_wow64`) accepted only ntdll's table. It now also calls
  the static tables of 32-bit DLLs through `wine_nx_call_static_wow64_unix`, each
  below its size, and still refuses any other handle, since the guest supplies
  it. `tests/check_wow64_unix_tables.py` compiles both functions against small
  tables; it fails on the build-18 gate at opengl32's `process_attach`.
- advapi32 forwards `SystemFunction036` (RtlGenRandom) to cryptbase, which was
  not staged, so the CRT's `rand_s` and bcrypt's `BCryptGenRandom` would reach a
  stub that raises an exception. The OpenTTD packager now also stages the DLLs
  that imported functions are forwarded to. For OpenTTD this adds cryptbase.dll,
  28 DLLs in all.

`nx-wow64-dynarec-18` makes Wine's error messages from Windows-side code reach
the log. On build-17 hardware, OpenTTD exited during x86 process startup with
`0xc0000135` (`STATUS_DLL_NOT_FOUND`) after 19 dynarec entries. Every static
import of `openttd.exe` resolves among the DLLs on the SD card, and the loader
names the missing library in an `err:` line, but no such line was logged:
- The Windows-side ntdlls read their debug channels from the page after the
  WoW64 PEB, which upstream's `dbg_init` fills. The runtime never called it,
  so every channel there was off, errors included. The runtime now writes the
  default entry: errors, plus fixmes with verbose traces. `dbg_init` itself
  would move the unix-side debug buffers into TEBs, which the runtime's own
  threads lack.
- x86 debug output goes through `wow64_wine_dbg_write`, which wrote to unix
  fd 2, where nothing reads on the Switch. It now goes to the runtime log like
  the 64-bit output.
- A program in a folder got `C:\openttd` as its current directory. It now ends
  in a backslash, as `RtlSetCurrentDirectory_U` stores it; relative paths are
  appended to it directly.

`nx-wow64-dynarec-17` prepares 32-bit OpenTTD 15.3. It uses the official
`openttd-15.3-windows-win32.zip` (SHA-256 3f092edc…0e74, matching
cdn.openttd.org) with OpenGFX 8.0. On build-16 hardware, `pe32-messages.exe` and
`pe32-timers.exe` passed all their groups.
- OpenTTD's i386 import closure is 27 DLLs. ws2_32 and opengl32 fail
  `DllMain` without a unixlib table.
  - The Switch's static unixlib tables only served 64-bit callers and matched
    modules through the 64-bit loader list, where 32-bit DLLs do not appear.
  - WoW64 lookups now match the DLL name in the module's export directory.
  - `ws2_32_unix_stub.c` adds 32-bit tables that let ws2_32 and opengl32
    load. opengl32 answers process and thread attach and detach with success.
    Every other call reports `STATUS_NOT_IMPLEMENTED`.
  - `tests/check_wow64_unix_tables.py` keeps the table sizes equal to Wine's
    enums (5 and 3102 entries), since unix calls index tables without bounds
    checks.
- A program's own arguments can live beside it (`openttd.args.txt`), ahead of
  `args.txt`. The launcher shows them. Paths with spaces are quoted in the
  command line.
- `tools/package-wow64-openttd.py` extends the Notepad package:
  - It checks both input hashes, installs the game in `C:\openttd` with OpenGFX
    in `baseset`, and writes `openttd.cfg`: the sprite font, and the survey
    declined so the first start opens no modal question.
  - Arguments select GDI video without a drawing thread, null sound and music,
    and a 1280x720 window.
  - It stages the import closure and writes
    `build-switch-wow64-dynarec/wine-nx-openttd-dynarec-17.zip`.
  - Under host Wine 11, the staged copy started with these arguments reached
    the main menu over the title game; a screenshot from `autoexec.scr`
    confirmed it. At runtime it loads only the import closure plus uxtheme,
    which user32 treats as optional.

`nx-wow64-dynarec-16` fixes the one group `pe32-messages.exe` failed on build-15
hardware. The other nine passed: SendMessage to another thread,
SendMessageTimeout, SendNotifyMessage, nested sends, ReplyMessage,
SendMessageCallback, GetQueueStatus, thread quit and the clipboard.
`MsgWaitForMultipleObjects` on an empty queue returned `WAIT_OBJECT_0`
immediately instead of timing out after 100 ms. Earlier in the test, B had
sent messages to A (the nested send and the callback result).
`horizon_msgq_touch` set the changed bit, but `horizon_msgq_update` only
added changed bits and never removed them, so QS_SENDMESSAGE stayed "changed"
after A processed the messages. The wait uses QS_ALLINPUT as its changed mask
and woke at once. As `set_queue_bits` and `clear_queue_bits` do upstream,
touching now sets both bits, and a kind with nothing left pending clears both.
`tests/horizon_msg_queue.c` reproduces the failure and passes with the fix.
`[DYNAREC]` reports are also logged only when the counters change, not every
5 seconds in the launcher or after a program parks.

The launcher now uses an explicit game library and a controller-first Home
screen. Home is a horizontal carousel of upright covers: the focused cover
enlarges at the left while neighboring cards slide past it. The selected title
and dimmed cover backdrop follow the selection. Library uses square cards and
supports title search, favorites, and title/recent sorting. Press X on Home or
Library to add a game: browse the SD card, choose a supported `.exe`, review
it, and confirm. Adding a game does not start it. A opens Game Details, where
the game can be started, favorited, configured, or removed from the library.
Removing a game never deletes its executable or sidecar settings.

Put a `cover.png` beside an executable to give it artwork, or use the catalog's
existing artwork path fields. Images load on the icon worker and fall back to
the executable icon when missing or invalid. Home crops artwork to 2:3 and
Library to 1:1. Left/right or a horizontal swipe moves one game; tap a neighboring
cover to focus it, press A to play, Y for Options, or X to add a game.

Configure a SteamGridDB API key in Settings, then choose **Download artwork**
from a game's Details screen. Choose the matching SteamGridDB title, then
Wine-NX downloads the highest-community-score static image for each surface:
a 512x512 grid for Library, a 600x900 portrait for Home's carousel, and a wide
hero for the focused game's backdrop. They are cached in `switch/wine/artwork`
and replaced atomically.
Existing v2 catalogs are migrated when they are next saved.

Settings and a game's own settings stand as sections beside their rows: the
sections at the left, and each row with its name, the line that says what it
does, and what it is set to -- a switch, a value the row changes where it
stands, or an arrow into a screen of its own. L and R move between sections, so
Left and Right stay the row's own.

**Address space** under Game Settings says what a game needs of the address space
Horizon gives Wine-NX. A game with no relocations is linked for one address and
no other, and only a forwarder made with a 32-bit address space has the low 4 GB
that address lives in; Need for Speed Underground 2 is one. The forwarder that
opened Wine-NX fixes this for everything it starts, and nothing can change it
afterwards, so a game that needs the low 4 GB under a 36- or 39-bit forwarder is
not started at all. Auto reads the game's own header; 32-bit and Any force the
answer, and are kept in the game's `.wine-nx.txt` beside it.

Name the 32-bit forwarder under **Settings → 32-bit forwarder**, choosing it from
the applications the console has installed, and a game that needs it is offered
to it instead: the game's path goes to `switch/wine/run-next.txt`, the console is
asked to close this forwarder and open that one, and the launcher there starts
the game without asking, then takes the file away. Without the setting the game
is refused with a message naming what it needs.

The launcher no longer discovers every `.exe` below `drive_c`. Membership is
stored in `launcher-library-v2.ini`. On first use, paths that were explicitly
saved in the older `launcher-library.txt` are migrated; other executables must
be added by the user. Missing executables remain visible and can be relocated
from Game Details. `target.txt` restores selection only for a registered game.
The console fallback reads the same catalog instead of scanning directories.

`nx-wow64-dynarec-15` originally added a launcher. Before, the runtime started whatever
`target.txt` named, and trying another program meant editing it on a computer.
Starting the NRO without a program argument now shows a console menu before
Wine initializes (`source/launcher.c`):
- It lists the `.exe` files under `drive_c` and two folder levels below it,
  skipping `windows`. Each is checked with the same PE header test as the
  runtime and marked x86 or ARM64.
- Controller: Up/Down (D-pad or stick) with key repeat, L/R to page, A to
  start, + to quit, Y to toggle `verbose.txt`.
- It opens on the program named in `target.txt`, and starting a program
  rewrites that file. A program started from the menu always runs to its entry
  point.
- `args.txt` is used only when its first word, quoted or not, names the chosen
  program, so each program no longer needs its own `args.txt`. A program path
  passed as the NRO's argument still skips the menu.
`tests/launcher_list.c` covers program names, `args.txt` matching, order,
preselection and scrolling. Hardware confirmation is pending with
`build-switch-wow64-dynarec/wine-nx-notepad-dynarec-15.zip`.

`nx-wow64-dynarec-14` adds message queues, messages between threads, the
clipboard and user atoms to the Horizon server. These are three gaps found by
comparing the server requests win32u and user32 can send with the server's
dispatch table.
- Message queues (`horizon_msg_queue.h`, `server/queue.c`):
  - Each thread has shared queue data. Without it, win32u asked the server
    for every message check. It also treated hooks as installed, so every
    window procedure call and retrieved message made failing
    `get_msg_queue`/`start_hook_chain` round trips.
  - Wake and changed bits cover sent and posted messages, WM_QUIT, input,
    paints, expired timers and reply results. `get_message` clears changed
    bits as upstream does.
  - `set_queue_mask`, `get_queue_status` and a waitable queue object back
    `MsgWaitForMultipleObjects`. Waits poll every millisecond, so timers and
    paints need no event to fall due.
  - The access time stays 0, so win32u never skips `get_message` on the bits'
    word.
  - win32u's `wait_message` waits in 10 ms slices on the Switch and polls the
    display driver between them. Controller input is polled from the message
    loop, and a truly blocking wait would otherwise freeze menus.
- Messages between threads: `send_message` for every type, `reply_message` and
  `get_message_reply` handle results with nesting, ReplyMessage, timeouts,
  cancellation, SendMessageCallback results and thread exit on either side.
- Clipboard (`horizon_clipboard.h`, `server/clipboard.c`): open rules, owner,
  sequence numbers, synthesized text, metafile and bitmap formats, delay
  rendering, release, viewer, listeners with WM_CLIPBOARDUPDATE, and cleanup.
  `add_user_atom` and `get_user_atom_name` back RegisterWindowMessage and
  RegisterClipboardFormat.
- Session memory was also copied to an SD-card file on every update. Views now
  copy from server memory when they register, and flushes no longer write the
  file.
Host tests `horizon_msg_queue.c` and `horizon_clipboard.c` run under ASan and
UBSan. `pe32-messages.exe` exercises the same features from x86 code and exits
42 under host Wine 11. Hardware confirmation is pending with
`build-switch-wow64-dynarec/wine-nx-notepad-dynarec-14.zip`.

`nx-wow64-dynarec-13` adds the text caret. On build-12 hardware the Notepad
caret still did not appear. The Horizon server had no `set_caret_window` or
`set_caret_info` handlers, and win32u's CreateCaret, SetCaretPos, ShowCaret,
HideCaret and blink toggle all use them. Each failed before
`display_caret` could invert the bar into the window. The handlers follow
`server/queue.c`:
- The caret window and rectangle live in the shared input data. The hide count
  and on/off state stay in the server.
- A different caret window starts at 0,0 and hidden.
- `CARET_STATE_ON_IF_MOVED` turns the caret on only when the position changes.
- A handle other than the caret's is refused with `STATUS_ACCESS_DENIED`.
- Destroying the caret's window clears the caret.
Replies carry the values from before the change, which win32u uses to erase
and redraw. `tests/check_caret.py` runs the production helpers under a copy of
win32u's caret sequences. It tracks inverted pixels through show, blink, moves,
nested hide, recreation, focus change and destroy. Hardware confirmation is
pending with `build-switch-wow64-dynarec/wine-nx-notepad-dynarec-13.zip`.

`nx-wow64-dynarec-12` adds window timers to the Horizon server. The
server had no `set_win_timer`/`kill_win_timer` handlers, so `SetTimer`,
`KillTimer` and win32u's system timers failed. The text caret was drawn once
and never blinked, and tooltips, scrollbar auto-repeat and animations had no
`WM_TIMER`. `horizon_win_timers.h` follows `server/queue.c`:
- A timer belongs to its window's thread, or to the caller's thread without a window.
- Window timers keep their id; timers without a window reuse a known id or get
  one counting down from 0x7fff.
- Setting an existing timer replaces it.
- `get_message` returns an expired timer after posted messages, WM_QUIT, input
  and paints. The earliest one comes first, filtered by exact window and message range.
- Removing the message reschedules the timer past the current time, so a busy
  thread gets one message instead of a backlog.
- Destroying a window or ending a thread drops its timers.
The server measures time with a monotonic millisecond clock from `armGetSystemTick`.
`tests/horizon_win_timers.c` covers caret-blink timing, peek versus remove,
coalescing, filters, ids, replacement, kill and cleanup. `pe32-timers.exe` is a
new x86 test that checks thread and window timers, a TIMERPROC, kill and
replace, and coalescing after 250 ms asleep. It exits 42 under host Wine 11.
Hardware confirmation is pending with
`build-switch-wow64-dynarec/wine-nx-notepad-dynarec-12.zip`.

`nx-wow64-dynarec-11` makes per-operation traces opt-in. A build-9 Notepad
session wrote 72,065 runtime log lines: 64,322 `[SYSCALL]` lines (two per
system call), 5,954 `[NXFONT]`, and hundreds of window-painting traces. Each
was formatted and buffered for the SD card. `horizon_trace` opened, appended to
and closed `horizon-trace.log` for every line, for most server requests. These
sites now check `wine_nx_runtime_verbose` before formatting: system calls,
Horizon server traces, `[NTOPEN]`, `[IOCTL]`, `[AFD]`, `[NXFONT]`, freetype,
`[NXWIN]`, `[NXDCE]`, `[NXSURF]`, `[NXRESIZE]`, `[NXWINPROC]`, `[NXMOUSE]` and
the display driver. The runtime reads `sdmc:/switch/wine/verbose.txt` (`1`
enables them) and logs `[INIT] verbose traces on|off`. Build, loader, exception,
exit, lifecycle, thread-creation and periodic `[DYNAREC]` lines still appear.
Verified on hardware: Notepad, menus and the cursor became much faster. The
build-10 Font dialog fix, also in this build, now lists fonts.

`nx-wow64-dynarec-10` adds window lists to the Horizon server. On build-9
hardware, Word Wrap worked, which confirmed posted messages. The Font dialog
opened with empty Font, Style, Size and Script boxes and a visible Color box.
The log showed font enumeration succeeding (`NtGdiEnumFonts` returned TRUE on
its second call), followed by messages to the combobox that returned 0 without
reaching its window procedure. `NtUserBuildHwndList` had failed 110 times with
`STATUS_NOT_IMPLEMENTED`, and user32's `GetDlgItem` depends on it: after
`GW_CHILD` it asks the server's `get_window_list` for the siblings. Every
`GetDlgItem` therefore returned NULL. Fonts were sent to no window, and
`ShowWindow` hid nothing because Notepad does not request `CF_EFFECTS`.
`get_window_list` now follows `server/window.c`: top-level windows,
recursive children, siblings from a window, and the desktop window, with
optional thread filters and a full count for short buffers. `get_window_tree`
had taken next/previous siblings from neighbors in the global window list.
Comboboxes create child windows immediately, so a sibling chain broke at the
first combobox. Both now use the parent-filtered list order.
`tests/check_window_list.py` builds a Font-dialog window list and runs the
production helpers. Verified on hardware with build 11: the Font dialog lists
fonts.

`nx-wow64-dynarec-9` adds posted messages to the Horizon server. On build-8
hardware, Format > Font... and Format > Word Wrap chosen with A only closed
the menu. `track_menu` posts the chosen command as `WM_COMMAND`, and even a
thread posting to itself goes through the server's `send_message` request. The
Horizon server had no handler, so every post returned `STATUS_NOT_IMPLEMENTED`
and the log shows no command reaching Notepad. EndDialog's wake-up post and
PostQuitMessage had the same gap. `horizon_message_queue.h` now keeps posted
messages and matches them like `server/queue.c`: per thread, oldest first, by
message range and by window, including its children, or thread-only with
HWND -1. `get_message` returns posted messages before WM_QUIT, hardware input
and paints. Destroying a window or ending a thread drops its messages.
Messages sent to another thread still need replies and remain unimplemented.
`wine-nx-probe/tests/horizon_message_queue.c` runs under ASan/UBSan from
`check-runtime-console.sh`. Verified on hardware: Word Wrap toggles, and
Font... opens its dialog.

`nx-wow64-dynarec-8` fixes an input freeze found on hardware with build 7.
With Notepad's Format menu open, quick B presses on Font... were processed as
follows. Win32u peeked the third press, which was within the double-click
interval, as `WM_RBUTTONDBLCLK`. Menu tracking then removed it with a filter
containing only that message. The Horizon server matched queued mouse messages
against their plain and non-client forms only, never their double-click forms.
It therefore never returned the press for removal, and the menu loop kept
peeking it. The log shows about 34,000 identical `[NXMOUSE] hit ... in=204`
lines, and every later click was queued behind it. The server now matches the
same forms as Wine's `check_hw_message_filter`. `tests/check_mouse_filter.py`
fails on the build-7 filter and passes now. Fast left double-clicks inside a
menu had the same exposure. No Font dialog was requested in that session:
menu-bar menus track with `TPM_LEFTBUTTON`, so B on a menu item is ignored.
Font... has to be chosen with A. Verified on hardware: fast B presses in an
open menu no longer freeze input.

`nx-wow64-dynarec-7` adds a mouse cursor for controllers. The right analog
stick moves it, A is the left button and B the right. Touch input still works.
The runtime reads the stick and buttons through libnx's pad API. Motion is a
velocity past a 12% radial dead zone and grows with the square of the tilt,
reaching 1000 px/s, scaled by the time between polls. Horizon has no hardware
cursor, so the arrow is painted into the linear back buffer only for
`framebufferEnd` and the covered pixels are restored right after. Window blits
never capture it, and cursor-only frames are capped at 60 Hz. The Horizon
server now queues `WM_RBUTTONDOWN`/`WM_RBUTTONUP`, and `SetCursorPos` moves the
drawn cursor. `check-runtime-console.sh` tests the motion and sprite restore.
`tests/check_pointer_events.py` runs the driver's event translation for moves,
clicks, drags and touch. On hardware the cursor, A clicks and menu opening
worked; B inside an open menu exposed the freeze fixed in build 8.

`nx-wow64-dynarec-6` preserves zero PID/TID in the shared user-handle allocator.
Build 5 passed zero for synthetic desktop ownership, but the allocator silently
changed those IDs to one. Consequently the build-5 hardware log still has no
popup-owner restoration entries, and switching menus leaves old pixels behind.
Clicking the document repaints it through a separate path. The earlier ancestry
fixture mocked allocation and missed this conversion; it now includes the
production allocator and reproduces build 5's failure before the correction.
`check_desktop_ancestry.py --allocator-baseline` keeps the current desktop
handler but uses the old allocator to reproduce that specific failure.
Normal client PID/TID and handle metadata are also checked. The corrected
ancestry and popup framebuffer fixtures pass under ASan/UBSan. Hardware menu
switching remains to be verified with
`build-switch-wow64-dynarec/wine-nx-notepad-dynarec-6.zip`.

`nx-wow64-dynarec-5` attempted to fix popup restoration. The build-4 hardware logs show
16 popup hide callbacks but no owner restoration; the screenshot retains
Edit/Format/View pixels. Server-created desktop/message handles were labeled
with the requesting process ID despite having no local client WND. As a
result, get_win_ptr(desktop) returned NULL and GA_ROOT failed for Notepad,
discarding the popup's restoration owner. Synthetic desktop/message windows
were requested with server ownership (PID/TID zero), but the shared allocator
overrode that until build 6. The display driver presents after
hiding a popup and ignores late flushes from cached hidden surfaces.

`python3 wine-nx-probe/tests/check_desktop_ancestry.py` exercises the production
desktop handler, shared allocator and ancestor lookup; `--baseline` reproduces the zero root.
`python3 wine-nx-probe/tests/check_popup_restore.py` exercises the production
driver against a host framebuffer: restore covered pixels, present on hide,
suppress hidden flushes, switch menus and reopen a cached surface. Both use
ASan/UBSan. The build-5 hardware result and follow-up correction are recorded above.

`nx-wow64-dynarec-4` fixes ownership in the native Horizon callback bridge.
Callback arguments are copied before WoW64's in-place conversion. Return data
is copied before NtCallbackReturn unwinds, and remains owned by a per-thread,
per-nesting-depth slot until that depth is reused. Thread-key destructors free
all retained buffers. This prevents call_window_proc from freeing a packed
message that is also the returned geometry buffer, and prevents callbacks
from returning dangling native-stack data.

`python3 wine-nx-probe/tests/check_callback_lifetime.py --baseline` reproduces
the old bridge's aliasing. Without --baseline, the same fixture exercises the
fixed production functions under ASan/UBSan: caller-buffer free, input
isolation, stack return data, nested callbacks, repeated calls and two threads.
The build-4 hardware screenshot confirms correct full-screen edit sizing,
document text and status bar. Closed menu pixels still remain; build 5
addresses that separate restoration path.

Before build 4, Notepad reached a visible x86 GUI on hardware. That screenshot
shows its edit control retaining the initial 676x467 size inside the 1280x720
frame, and menu interaction is incorrect. `nx-wow64-dynarec-3` adds the missing
WoW64 GetPresentRect pointer conversion plus NXRESIZE/HZGEOM traces. The logs
also show malformed child client coordinates and a low-screen popup; the
relationship between these symptoms and the conversion gap is unconfirmed.
This is a diagnostic GUI package, not a hardware-confirmed layout/menu fix.

Notepad dynarec checkpoint: the first x86 GUI run loaded fonts and reached
native NtContinue, which returned STATUS_NOT_IMPLEMENTED before Wine hit a
breakpoint. `nx-wow64-dynarec-2` implements cooperative ARM64 continuation
through the existing Horizon restore routine, including SIMD/FP control and
the active TEB. `[CONTINUE]` traces record the destination PC/SP. The routine
uses x17 as branch scratch; precise asynchronous context restoration remains
outside this change. `tests/check_continue_context.py` checks the context
conversion and x18 policy; actual continuation needs hardware validation.
The new package is `build-switch-wow64-dynarec/wine-nx-notepad-dynarec-2.zip`.

This directory contains the Switch runtime build/package tooling.

The main project README is at the repository root:

```text
../README.md
```

Common build command:

```sh
WINE_NX_APP=notepad ./wine-nx-probe/build-switch.sh
```

Staged SD package:

```text
wine-nx-probe/build-switch/sd-card/switch/wine
```

## WoW64 worker wait checkpoint

`nx-wow64-threads-2` fixes immediate timeouts in the Horizon select handler.
Pending waits poll at 1 ms intervals outside the object lock, using Wine's
monotonic or NT wall-clock deadline as appropriate. Infinite waits remain
pending; signal-and-wait signals once; wait-any returns the selected index.
This is a basic blocking implementation, not full APC, keyed-event, mutex
ownership/abandonment, timer, or thread-exit support.

Run `python3 wine-nx-probe/tests/check_select_wait.py` for host regression
coverage. The Switch package launches `pe32-threads.exe`; expected success is
both worker results zero, protected counter `0x40`, `PASS ALL`, and process
exit `0x2a`. Hardware confirmed on 2026-09-10: time, heap, single-thread
TLS, file I/O, both workers, TLS isolation, events, and the protected counter
all passed, ending with `PASS ALL` and exit `0x2a`. Workers deliberately
remain parked; normal thread return/exit and join are not yet validated.

## Experimental ARM64 dynarec checkpoint

Run `sh wine-nx-probe/check-box64-execution.sh` to build and test both CPU
backends in the ARM64 container and build their Switch NROs. The dynarec
artifact is `build-box64-core/switch/box64-execution-dynarec.nro`.
The normal Wine runtime still selects the interpreter.

The dynarec uses distinct writable/executable aliases, revalidates guest code
on each block entry, and cancels an interrupted translation before unwinding
its stack. Call/return patching stays disabled because it writes executable
code in place. Tests check real native dispatch, gate round trips, FS, SSE,
x87, reentry, repeated operand/decoder fault recovery, and changed code at the
same guest address. The pinned vendor checkout remains untouched; CMake
creates checked patched copies for the host integration.

Remaining limitations before runtime adoption: generated blocks do not enforce
instruction budgets; faults do not reconstruct precise guest registers from
native registers; writes inside an executing block are not trapped; the code
arena uses bounded bump allocation without reclamation; concurrent guest code
modification is not synchronized. The dynarec test does not claim instruction
count accuracy or bounded execution. The standalone hardware test exercises
split mappings, cache maintenance and fault unwinding; preservation of a real
Wine TEB in x18 still needs validation through the Wine runtime.

`nx-dynarec-2` routes RDTSC through the shared host timer helper instead of
emitting CNTVCT_EL0 reads. This addresses the suspected timer trap from the
first hardware run (`ESR=0x6244f821`). Both backends must now call the helper
exactly twice in the RDTSC regression. Hardware confirmed on 2026-09-11:
initial EAX=101, ECX=77, timer helper PASS, x87/CPUID/MMX PASS, six recovered
operand/decoder faults followed by successful atomic execution, 25 native
dispatch entries and 14,000 emitted bytes. Gates, FS, SSE, reentry and code
revalidation all passed. Zero reported instructions/checked reads reflects
missing dynarec instruction accounting, not a failure to execute guest code.

Next integration milestone: an opt-in dynarec Wine runtime running the existing
PE32 regression suite, retaining the interpreter default/fallback. Native
execution through the full Wine loader, WoW64 gates and real thread TEBs needs
its own hardware validation; the standalone pass does not establish that.

## Opt-in 7zr dynarec runtime

`sh wine-nx-probe/build-wow64-dynarec.sh` builds the normal interpreter package,
then the opt-in `WINE_NX_BOX64_DYNAREC=ON` runtime in
`build-switch-wow64-dynarec`. It stages the same validated DLLs, 7zr executable,
archive fixtures and two-thread benchmark arguments, and writes
`build-switch-wow64-dynarec/wine-nx-dynarec-1.zip`.

The runtime build label is `nx-wow64-dynarec-1`. `[DYNAREC]` logs native dispatch
entries and emitted bytes every five seconds; these are not guest instruction
counts. The benchmark should produce Avr:/Tot: rows, its lifecycle verdict,
and exit zero. The full Wine 7zr benchmark is hardware-confirmed below; broader application
compatibility and archive operations remain to be tested.
The default runtime option remains the interpreter. Failure to allocate the
initial code arena falls back to interpretation. Both SD packages target the
same `/switch/wine` directory, so installing the interpreter package restores
that runtime.

The package keeps the PE32 functional, worker and lifecycle tests available.
For the existing archive checks, change args.txt to `C:\7zr.exe t
C:\7zr-sample.7z` (on one line), or `C:\7zr.exe x C:\7zr-tree.7z
-oC:\7zr-out -y`. Do not infer archive correctness or a performance improvement
from a successful build; both need the next hardware run.

### Full Wine dynarec hardware result

The uploaded `debug/wine-nx-runtime.log`, `debug/stdout.txt` and
`debug/horizon-trace.log` confirm `nx-wow64-dynarec-1` completed
`C:\7zr.exe b 1 -mmt2 -md18` on Switch. Compression: 77 KiB/s;
decompression: 49,353 KiB/s; total benchmark rating: 2,030 MIPS.
Runtime telemetry: 9,053 native dispatch entries, 2,112,688 emitted bytes.
Thread lifecycle verdict PASS (8 exits, 7 reaped, one permitted pending
zombie, zero live engines); process exit code zero.

The CPUID brand still says "Wine-NX Box64 i386 interpreter" because it is a
static string in wow64_box64_engine.c; native-entry telemetry establishes
that this run used the dynarec. These benchmark results do not establish a
speedup without a matching interpreter measurement. Real archive creation,
integrity testing and extraction remain separate dynarec validation steps.

### ARM64 WoW64 callback return (build 24)

The build-23 hardware trace for `pe32-video-startup.exe` still stopped after
Box64 run #305 returned to the syscall gate with EAX=5 (`NtCallbackReturn`).
Selecting `_setjmpex(buf, NULL)` in wow64.dll was insufficient: unlike Wine's
x86-64 implementation, ARM64 ntdll's `longjmp` always called `RtlUnwind`.
Build 24 adds the missing NULL-frame register restore to ARM64 ntdll, while
retaining the unwind path for non-NULL frames.

`python3 wine-nx-probe/tests/check_arm64_longjmp.py` executes the actual ARM64
assembly and longjmp function on an ARM64 host. Nested returns, stack and FP
state restoration, return values and routing of normal frames pass at O0/O2;
removing the new branch reproduces the build-23 failure in the same test.
This host regression does not establish that OpenTTD starts on Switch.

Install the build-24 package including `drive_c/windows/system32/ntdll.dll`,
`drive_c/windows/system32/wow64.dll` and the NRO. Run `pe32-video-startup.exe`
first: the next hardware checkpoint is `after application LoadIconW`, followed
by the cursor, class and window checks and `[VIDEO TEST] PASS ALL`.
