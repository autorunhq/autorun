# The experimental FEX

Patches tried out on top of Carafe's FEX for 32-bit programs. The build is
FEX-2609, `horizon-wine/fex/horizon.patch`, the series in `../fex`, and then
every `*.patch` in this folder in name order (`../build-fex.sh ... experimental`).
With no patch here it is Carafe's build under another name.

It is released apart from Carafe's, as `fex-experimental-<version>`, and a game
runs on it only when its settings say so: **Game settings, Emulation, FEX
build, Experimental** (`fex-build=experimental`). Carafe's build and the games
set to it are not touched by anything here.

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
