"""Where llvm-mingw is: the build Wine's Windows side and Autorun's own setup
programs are compiled with.

build.sh fetches it from llvm-mingw's releases, under a name that says which
machine it is for (llvm-mingw-20260505-ucrt-ubuntu-22.04-x86_64 on the GitHub
runner), and exports WINE_NX_LLVM_MINGW for what it then runs, so the tools
below ask for that first, then for the one llvm-mingw in toolchains."""
from pathlib import Path
import os

horizon_wine = Path(__file__).resolve().parents[1]


def bin_dir():
    """llvm-mingw's bin folder: the one build.sh fetched, else the one in
    toolchains."""
    env = os.environ.get("WINE_NX_LLVM_MINGW")
    if env:
        return Path(env) / "bin"
    found = sorted((horizon_wine / "toolchains").glob("llvm-mingw-*/bin"))
    if found:
        return found[0]
    raise SystemExit("No llvm-mingw in horizon-wine/toolchains; sh build.sh fetches it "
                     "(it exports WINE_NX_LLVM_MINGW for the tools below).")
