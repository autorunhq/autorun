Autorun low-window prototype

Base: Atmosphere 1.11.2, 5388824be146a89619e8d641acd64599cf1c5f62.
Only 39-bit processes with program ID 0548EABB35576000 receive the new layout.
That is Autorun's generation-2 main forwarder for
sdmc:/switch/wine/wine-nx-runtime.nro, without additional arguments.
Other titles and the 32-bit forwarder keep their normal layout.

The loader places the forwarder above 4 GiB. Mesosphere places its native
heap, stacks and TLS above 4 GiB and allows code aliases from 0x00200000.
The 2 MiB null guard stays inaccessible. Autorun reserves the remaining low
4 GiB against native allocations, without committing physical RAM.
Guest-visible WoW64 structures, x86 DLLs and game allocations stay low.
This does not change a game's 2 GiB/non-LAA or 4 GiB/LAA pointer limit.

Build with devkitA64, libnx, Python 3 and Atmosphere's build dependencies:
  sh wine-nx-probe/build-low-window.sh /path/to/rebuilt/wine-nx-runtime.nro

The archive uses Fusee's SD overrides; it does not replace package3.
Use it only with the matching Atmosphere 1.11.2 package3 and fusee.bin.
For Hekate fss0/pkg3 boots, duplicate the working entry and add these lines
after its fss0/pkg3 line, preserving its other settings:
  kernel=atmosphere/mesosphere.bin
  kip1=atmosphere/kips/autorun-loader.kip
Keep the original entry unchanged. These directives do not apply to an entry
using payload=fusee.bin; Fusee reads the SD overrides itself.
Do not overwrite a custom kernel or existing loader override without backing
it up. Keep a known-working boot entry and SD-card access for recovery.

Test:
1. Back up wine-nx-runtime.nro and any existing Atmosphere overrides.
2. Install the NRO, then reinstall the main 39-bit forwarder from Autorun's
   System settings. Keep the 32-bit forwarder installed.
3. With the console powered off, copy atmosphere/mesosphere.bin and
   atmosphere/kips/autorun-loader.kip from this archive to the SD card.
   Do not leave another loader KIP override in that directory.
4. Boot through the matching Fusee and open the main Autorun forwarder.
5. In wine-nx-runtime.log check title ID 0548EABB35576000, 39-bit address
   space, and both [LOWVA] RX execute at 00400000: PASS and
   [LOWVA] RW alias at ffff0000: PASS, followed by the final [LOWVA] PASS.
   Failure keeps fixed-low games on the 32-bit forwarder.
6. Leave the game's Address space on Auto. Test a Win32 validation program,
   then Dead Space with Box64, then FEX. Also retest a Win64 game.
   An explicit 32-bit setting still selects the old forwarder.
7. Repeat without the two kernel/loader overrides: fixed-low Win32 games
   must use the 32-bit forwarder, and Win64 games must remain on 39-bit.

Rollback: power off, remove only this prototype's mesosphere.bin and
autorun-loader.kip (restore prior overrides if any), then boot normally.
The new NRO also works without the patch. Games and settings are untouched.

Host tests mock Horizon syscalls. A passing host test or build is not proof
that the kernel boots or the layout works on Switch. Hardware validation is
pending; send wine-nx-runtime.log from the first patched 39-bit launch.

Boot override references:
https://github.com/Atmosphere-NX/Atmosphere/blob/5388824be146a89619e8d641acd64599cf1c5f62/fusee/program/source/fusee_stratosphere.cpp
https://github.com/CTCaer/hekate/blob/master/README.md
