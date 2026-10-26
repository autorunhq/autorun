#!/bin/sh
# Build Autorun: the runtime, in the switch-dev image, and the SD card archive,
# horizon-wine/build-autorun/autorun-NNN.zip.
#
#   sh build.sh [--x86] [--dlls] [--clean] [--jobs N]
#
#   --x86    ship the x86-only runtime (Box64, no FEX) instead of the AMD64 one
#   --dlls   also build the DLL repository's DLLs (horizon-dlls) that changed
#   --clean  remove the runtime's build folders first
#   --jobs   parallel jobs (default: the machine's cores)
#
# Needs Docker, git, Python 3, make and bison (Homebrew's, on a Mac). It fetches
# the llvm-mingw toolchain Wine's Windows side is built with, and builds the
# boot payloads Quick setup installs, when they are missing. The Windows DLLs
# are not in the archive: Autorun downloads them.
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
hw="$root/horizon-wine"
x86=0 dlls=0 clean=0
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)"
while [ $# -gt 0 ]; do
    case "$1" in
        --x86) x86=1 ;;
        --dlls) dlls=1 ;;
        --clean) clean=1 ;;
        --jobs) jobs="$2"; shift ;;
        -h|--help) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option $1; see --help." >&2; exit 1 ;;
    esac
    shift
done
step() { printf '\n==> %s\n' "$*"; }

for tool in docker git python3 make curl; do
    command -v "$tool" >/dev/null || { echo "Missing $tool." >&2; exit 1; }
done
docker info >/dev/null 2>&1 || { echo "Docker is not running." >&2; exit 1; }
image="$(cat "$hw/switch-dev.txt")"

# The DLL repository: the package is checked against it.
if [ ! -f "$root/horizon-dlls/switch/wine/horizon-dlls/manifest.json" ]; then
    step "Fetching the DLL repository (horizon-dlls)"
    git -C "$root" submodule update --init horizon-dlls
fi

# llvm-mingw, for Wine's headers and Windows modules.
llvm_version=20260505
case "$(uname -s)-$(uname -m)" in
    Darwin-*) llvm_name="llvm-mingw-$llvm_version-ucrt-macos-universal" ;;
    Linux-aarch64|Linux-arm64) llvm_name="llvm-mingw-$llvm_version-ucrt-ubuntu-22.04-aarch64" ;;
    Linux-x86_64) llvm_name="llvm-mingw-$llvm_version-ucrt-ubuntu-22.04-x86_64" ;;
    *) echo "No llvm-mingw build for $(uname -s) $(uname -m)." >&2; exit 1 ;;
esac
llvm="$hw/toolchains/$llvm_name"
if [ ! -x "$llvm/bin/aarch64-w64-mingw32-clang" ]; then
    step "Fetching $llvm_name"
    mkdir -p "$hw/toolchains"
    curl -fL --retry 3 -o "$hw/toolchains/$llvm_name.tar.xz" \
        "https://github.com/mstorsjo/llvm-mingw/releases/download/$llvm_version/$llvm_name.tar.xz"
    tar -xJf "$hw/toolchains/$llvm_name.tar.xz" -C "$hw/toolchains"
    rm "$hw/toolchains/$llvm_name.tar.xz"
fi
export WINE_NX_LLVM_MINGW="$llvm" WINE_NX_JOBS="$jobs"

step "Pulling $image"
docker pull -q "$image" >/dev/null

# The boot payloads Quick setup installs (Atmosphere 1.11.2's Mesosphere and
# loader, and Horizon-OC's), embedded in the runtime. Built once; it takes a
# while, as Atmosphere's libraries are built twice. Atmosphere builds only
# against devkitPro's released libnx, which the image keeps apart.
if [ ! -f "$hw/toolchains/boot-payloads/bundle/setup_boot_manifest.h" ]; then
    step "Building the boot payloads (Atmosphere and Horizon-OC), once"
    # git in the image must trust the mounted tree: bind mounts keep the host's
    # UIDs, and the container runs as root.
    docker run --rm -v "$root:/work" -w /work -e WINE_NX_JOBS="$jobs" \
        -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*' \
        -e DEVKITPRO=/opt/devkitpro-release "$image" sh horizon-wine/build-boot-bundle.sh
fi

if [ "$clean" = 1 ]; then
    step "Removing the runtime's build folders"
    rm -rf "$hw/build-switch-amd64" "$hw/build-switch-wow64-mesa-switch" "$hw/build-autorun"
fi

# The AMD64 runtime; its build also configures the Wine tree both runtimes
# compile against, which the x86 one needs first.
if [ "$x86" = 0 ] || [ ! -f "$hw/build-wine-amd64-pe/Makefile" ]; then
    step "Building the AMD64 runtime (FEX, DXVK, VKD3D-Proton)"
    WINE_NX_FEX=1 WINE_NX_DXVK=1 WINE_NX_VKD3D=1 sh "$hw/build-amd64-components.sh"
fi
if [ "$x86" = 1 ]; then
    step "Building the x86 runtime"
    sh "$hw/build-x86.sh"
fi

if [ "$dlls" = 1 ]; then
    step "Building the DLL repository's changed DLLs"
    python3 "$root/horizon-dlls/tools/build-dlls.py" --jobs "$jobs"
fi

step "Packaging"
# The tuned FEX modules a fetch left in fex/opt-modules (the fex-opt release)
# are staged with the package; without them the pinned modules stand alone.
if [ -d "$hw/fex/opt-modules/switch/wine" ]; then
    set -- --extra "$hw/fex/opt-modules/switch/wine"
else
    set --
fi
if [ "$x86" = 1 ]; then
    python3 "$hw/tools/package-autorun.py" --x86 "$@"
else
    python3 "$hw/tools/package-autorun.py" "$@"
fi
