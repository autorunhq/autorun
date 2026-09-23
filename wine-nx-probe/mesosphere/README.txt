Autorun low-window patch

Base: Atmosphere 1.11.2, 5388824be146a89619e8d641acd64599cf1c5f62.
Only 39-bit processes with program ID 0548EABB35576000 receive the new layout.
That is Autorun's generation-2 main forwarder for
sdmc:/switch/wine/wine-nx-runtime.nro, without additional arguments.
Other titles keep their normal layout.

The 39-bit forwarder, native heap, stacks and TLS stay above 4 GiB. The low
window starts at 0x00200000 and is reserved for Win32 guest mappings. A
game's normal 2 GiB/non-LAA or 4 GiB/LAA pointer limit still applies.

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

With the console powered off, copy atmosphere/mesosphere.bin and
atmosphere/kips/autorun-loader.kip from the archive to the SD card. Reinstall
the 39-bit forwarder from Autorun's System settings. The runtime checks
executable mapping at 0x00400000 before using the low window.

Rollback: power off, remove only this patch's mesosphere.bin and
autorun-loader.kip (restore prior overrides if any), then boot normally.
Games requiring fixed low addresses cannot launch without the patch.
Games and settings are untouched.
