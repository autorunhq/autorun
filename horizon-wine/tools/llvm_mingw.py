"""Where llvm-mingw is: the build Wine's Windows side and Autorun's own setup
programs are compiled with.

build.sh fetches it into horizon-wine/toolchains under a name that says which
machine it is for (llvm-mingw-20260505-ucrt-macos-universal on a Mac,
...-ubuntu-22.04-x86_64 on Linux), and exports WINE_NX_LLVM_MINGW for what it
then runs, so the tools below ask for that first rather than for one machine's
name."""
from pathlib import Path
import os

horizon_wine = Path(__file__).resolve().parents[1]


def bin_dir():
    """The bin folder of llvm-mingw: the one build.sh fetched, else the only
    one in toolchains, else the Mac's, which is where it was first put."""
    env = os.environ.get("WINE_NX_LLVM_MINGW")
    if env:
        return Path(env) / "bin"
    found = sorted((horizon_wine / "toolchains").glob("llvm-mingw-*/bin"))
    if found:
        return found[0]
    return horizon_wine / "toolchains/llvm-mingw-20260505-ucrt-macos-universal/bin"
