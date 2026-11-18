# The experimental FEX

Patches tried out on top of Carafe's FEX for 32-bit programs. The build is
FEX-2609, `horizon-wine/fex/horizon.patch`, the series in `../fex`, and then
every `*.patch` in this folder in name order (`../build-fex.sh ... experimental`).
With no patch here it is Carafe's build under another name.

It is released apart from Carafe's, as `fex-experimental-<version>`, and a game
runs on it only when its settings say so: **Game settings, Emulation, FEX
build, Experimental** (`fex-build=experimental`). Carafe's build and the games
set to it are not touched by anything here.

## What each version tries

| Version | On top of Carafe's `fex=2609-1` |
| --- | --- |
| `2609-1` | Nothing: Carafe's build under another name. Its release was removed once `2609-4` came, to keep the list short. |
| `2609-2` | FEX's own code compiled for the console's CPU (`-mcpu=cortex-a57`). No patch: the translated code is the same, the translator and its helpers are tuned for the A57. (Link-time optimization was tried with it and does not link, see `../build-fex.sh`.) |
| `2609-3` | `2609-2` and `0001`: FEX-2611's `preserve_all` for the helpers translated code calls (upstream `c76fccf25`, `ef006bdb1`). x87 maths at full precision (FEX options, x87 reduced precision off) is called without saving every register around each call. Nothing changes for a game on reduced precision. |
| `2609-4` | `2609-3` and `0002`: the C runtime's `_ftol` (Visual C++ 6) and `_ftol2` (Visual C++ 2002/2003), which every `(int)` cast of a float calls, are one conversion instead of some thirty instructions, with x87 reduced precision on. Doom 3 spent about a fifth of its translated time there while loading a map; in play it was under 1%. |
| `2609-5` | `2609-4` and `0003`: a disk cache of translated code of its own for each experimental build. With FEX options, disk cache on, `2609-4` ran the code `2609-3` had stored, so its change was never translated; its release was removed. |
| `2609-6` | `2609-5` and `0004`: a 32-bit `DIV`/`IDIV` whose `EDX` is only the extension of `EAX` (nearly all of them) divides 32 bits instead of joining the two into 64, a shorter sequence and a shorter divide on the A57. From `berlint-hub/FEX` (`arena/799d045b-fex`). `2609-2`'s release was removed to keep three. |

A game with **FEX options, disk cache** on keeps translated code on the card.
The cache does not know which FEX filled it, so from `2609-5` on the version is
part of its key (`0003`): a new version starts with an empty cache, and the
first run of a game on it translates everything again.

To try something:

1. Add a patch against the tree the Carafe series leaves, named like those
   (`0001-fex-what-it-does.patch`).
2. Raise `fex-experimental` in `../versions.txt` (`2609-2`, `2609-3`, ...): a
   card sees a new release only under a new version.
3. Push. The `carafe dlls` workflow builds and releases it, and the launcher
   lists it under Experimental.

A patch that proved itself moves to `../fex` with a new `fex` version; one that
did not is deleted. An option a patch adds for the launcher's FEX options goes
in `horizon-wine/source/fex_options.h`, after the Carafe ones.
