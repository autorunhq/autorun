#!/bin/sh
# Game fixes (source/game_fixes.c) against the game fixes repository checkout
# in game-fixes/, with a folder standing in for GitHub: the index the build
# ships and the one downloaded, games recognised, files downloaded and checked,
# a setup prepared to run before its game and recorded after, updates, skips,
# games told by their version resource, link time and size, settings profiles and launch arguments,
# and a fix this build cannot apply whole left out.
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
horizon_wine="$root/horizon-wine"
repo="$root/game-fixes/switch/wine/game-fixes"
[ -f "$repo/index.json" ] || { echo "no game fixes repository in $root/game-fixes" >&2; exit 1; }
build="$(mktemp -d "${TMPDIR:-/tmp}/game-fixes.XXXXXX")"
trap 'rm -rf "$build"' EXIT HUP INT TERM
# OpenSSL's SHA-256 for horizon_dlls.c on Linux; macOS has it in CommonCrypto.
crypto=""; [ "$(uname -s)" = Linux ] && crypto=-lcrypto

clang -std=gnu11 -Wall -Wextra -Werror -Wno-deprecated-declarations -O1 -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$horizon_wine/source" \
    -I/opt/homebrew/include "$horizon_wine/tests/game_fixes.c" "$horizon_wine/source/game_fixes.c" \
    "$horizon_wine/source/horizon_dlls.c" "$horizon_wine/source/game_settings.c" -L/opt/homebrew/lib -lzstd -lz $crypto -o "$build/game_fixes"
mkdir -p "$build/card/sdmc:/switch/wine" "$build/served"
# Wine's own programs stand in for a game: regedit.exe has a version resource,
# notepad.exe none.
dlls="$root/horizon-dlls/switch/wine/drive_c/windows"
[ -f "$dlls/system32/regedit.exe" ] || { echo "no $dlls/system32/regedit.exe: check out horizon-dlls" >&2; exit 1; }
(cd "$build/card" && "$build/game_fixes" "$repo" "$build/served" "$dlls/system32/regedit.exe" "$dlls/syswow64/notepad.exe")

# The settings engine, and the launch arguments, with the repository's profiles
# as a card has them: Fallout, Medal of Honor, Spider-Man, Kingdom Hearts and
# Shadow of the Tomb Raider, each as its compiled code once did.
for test in game_settings kingdom_hearts tomb_raider; do
    clang -std=gnu11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
        -I "$horizon_wine/source" -I "$horizon_wine/tests" -I/opt/homebrew/include \
        "-DGAME_FIXES_INDEX_PATH=\"$repo/index.json\"" "$horizon_wine/tests/$test.c" \
        "$horizon_wine/source/game_fixes.c" "$horizon_wine/source/horizon_dlls.c" -L/opt/homebrew/lib -lzstd -lz $crypto \
        -o "$build/$test"
    (cd "$build" && "./$test")
done
