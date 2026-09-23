#!/bin/sh
# Host run of the launcher with SDL's dummy video driver: builds it with ASan,
# stages the programs of a built card as symlinks under "sdmc:", drives the
# screens with tests/launcher_host.c's script and checks what it wrote and
# returned. Screenshots go to $LAUNCHER_SHOTS when it is set.
# Needs Homebrew's sdl2 (sdl2-compat), sdl3, sdl2_ttf and libpng.
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
probe="$root/wine-nx-probe"
# Two programs to stand in for games, out of whichever stage the card was last
# packaged from: the full package keeps its own and takes the checkpoints' away.
drive_c=""
for stage in full-sd-card notepad-sd-card openttd-sd-card audio-sd-card opengl-sd-card d3d9-sd-card war3-sd-card; do
    candidate="$probe/build-switch-wow64-dynarec/$stage/switch/wine/drive_c"
    [ -f "$candidate/notepad.exe" ] && { drive_c="$candidate"; break; }
done
[ -n "$drive_c" ] || { echo "no staged drive_c with notepad.exe: run tools/package-wow64-full.py" >&2; exit 1; }
build="$(mktemp -d "${TMPDIR:-/tmp}/wine-nx-launcher.XXXXXX")"
trap 'rm -rf "$build"' EXIT HUP INT TERM
font="${LAUNCHER_FONT:-/System/Library/Fonts/Supplemental/Arial.ttf}"
shots="${LAUNCHER_SHOTS:-$build}"

# The icons embedded in the launcher must match assets/, and fill as SVG does.
python3 "$probe/tools/make-launcher-icons.py" --check
clang -std=gnu11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$probe/tests/launcher_svg.c" "$probe/source/launcher_svg.c" -o "$build/launcher_svg"
"$build/launcher_svg"

clang -std=gnu11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$probe/tests/launcher_catalog.c" "$probe/source/launcher_catalog.c" -o "$build/launcher_catalog"

# The three NCAs a forwarder is made of, built here and taken apart again: the
# loader's NPDM comes out of whichever build dir has one, since it is what the
# address space is patched into.
npdm=""
for dir in build-switch-wow64-dynarec build-switch-wow64-mesa-switch build-switch-wow64; do
    [ -f "$probe/$dir/hbl-main.npdm" ] && { npdm="$probe/$dir/hbl-main.npdm"; break; }
done
if [ -n "$npdm" ]; then
    mkdir -p "$build/switch-shim" "$build/ncas"
    cp "$probe/tests/switch_shim.h" "$build/switch-shim/switch.h"
    clang -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -O1 -g \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        -I "$probe/tests" -I "$build/switch-shim" \
        "$probe/tests/forwarder_build.c" -o "$build/forwarder_build"
    "$build/forwarder_build" "$npdm" "$probe/assets/autorun.jpg" "$build/ncas"
else
    echo "forwarder: skipped, no hbl-main.npdm in any build directory"
fi
"$build/launcher_catalog"

clang -std=gnu11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$probe/source" $(sdl2-config --cflags) -I/opt/homebrew/include \
    "$probe/tests/launcher_host.c" "$probe/source/launcher.c" "$probe/source/launcher_catalog.c" "$probe/source/launcher_ui.c" \
    "$probe/source/launcher_graphics.c" \
    "$probe/source/steamgriddb.c" \
    "$probe/source/launcher_svg.c" \
    $(sdl2-config --libs) -L/opt/homebrew/lib -lSDL2_ttf -lpng -lcurl -o "$build/launcher_host"
# sdl2-compat looks for SDL3 next to the program, not in Homebrew's lib folder.
ln -s /opt/homebrew/lib/libSDL3.0.dylib "$build/libSDL3.dylib"

# The floating keyboard: what its buttons and taps send, and its picture.
clang -std=gnu11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$probe/source" $(sdl2-config --cflags) -I/opt/homebrew/include \
    "$probe/tests/osk.c" "$probe/source/osk.c" \
    $(sdl2-config --libs) -L/opt/homebrew/lib -lSDL2_ttf -lpng -o "$build/osk"
"$build/osk" "$font" "$shots"

card="$build/card/sdmc:"
mkdir -p "$card/switch/wine/drive_c/openttd" "$card/games/deep/er/still"
ln -s "$drive_c/notepad.exe" "$drive_c/7zr.exe" "$card/switch/wine/drive_c/"
ln -s "$drive_c/notepad.exe" "$card/switch/wine/drive_c/openttd/openttd.exe"
ln -s "$drive_c/notepad.exe" "$card/games/deep/er/still/Deep.exe"

# + -> Run a program once: the browser's pick starts at once and stays out of
# the library.
cat > "$build/once-script.txt" <<SCRIPT
wait 10
key plus
wait 3
key down
wait 2
key a
wait 3
key a
wait 3
key a
wait 3
key a
wait 5
SCRIPT
( cd "$build/card" && SDL_VIDEODRIVER=dummy "$build/launcher_host" "$font" "$build/once-script.txt" \
    > "$build/once-out.txt" 2>&1 ) || { cat "$build/once-out.txt"; exit 1; }
grep -q "launcher returned 1 target 'sdmc:/switch/wine/drive_c/openttd/openttd.exe'" "$build/once-out.txt" || {
    cat "$build/once-out.txt"; exit 1; }
if [ -f "$card/switch/wine/launcher-library-v2.ini" ] && grep -q '^\[game ' "$card/switch/wine/launcher-library-v2.ini"; then
    cat "$card/switch/wine/launcher-library-v2.ini"; exit 1
fi
rm -f "$card/switch/wine/launcher-library-v2.ini" "$card/switch/wine/target.txt"
echo "launcher host run: + Run a program once starts it without adding it"

# An empty explicit catalog must stay empty even though drive_c contains several
# executables. Add OpenTTD through the browser, verify that adding did not launch
# it, then open its options with Y and start it from there. The browser asks
# which storage to look in before it shows any folder, so the card is chosen
# first, then the folder, then the program, then Add in the review.
cat > "$build/script.txt" <<SCRIPT
wait 10
shot $shots/library-empty.png
key x
wait 5
key a
wait 3
key a
wait 3
key a
wait 3
key a
wait 3
key a
wait 5
shot $shots/library-added.png
key y
wait 5
shot $shots/game-details.png
key right
wait 3
key a
wait 5
SCRIPT

cd "$build/card"
SDL_VIDEODRIVER=dummy "$build/launcher_host" "$font" "$build/script.txt" > "$build/out.txt" 2>&1 || { cat "$build/out.txt"; exit 1; }
grep -q "launcher returned 1 target 'sdmc:/switch/wine/drive_c/openttd/openttd.exe'" "$build/out.txt" || { cat "$build/out.txt"; exit 1; }
grep -q '^version=3$' "sdmc:/switch/wine/launcher-library-v2.ini"
grep -q '^path=sdmc:/switch/wine/drive_c/openttd/openttd.exe$' "sdmc:/switch/wine/launcher-library-v2.ini"
test "$(grep -c '^\[game ' "sdmc:/switch/wine/launcher-library-v2.ini")" -eq 1
grep -qx "sdmc:/switch/wine/drive_c/openttd/openttd.exe" "sdmc:/switch/wine/target.txt"
grep "^\[LAUNCHER\]" "$build/out.txt"

# Restart from the saved catalog. Home must show the game that was played and A
# must start it without importing the other staged EXEs.
cat > "$build/restart-script.txt" <<SCRIPT
wait 5
key a
wait 3
key a
SCRIPT
SDL_VIDEODRIVER=dummy "$build/launcher_host" "$font" "$build/restart-script.txt" > "$build/restart-out.txt" 2>&1 || {
    cat "$build/restart-out.txt"; exit 1;
}
grep -q "1 catalog games (1 registered, 1 shown)" "$build/restart-out.txt" || { cat "$build/restart-out.txt"; exit 1; }
grep -q "launcher returned 1 target 'sdmc:/switch/wine/drive_c/openttd/openttd.exe'" "$build/restart-out.txt" || {
    cat "$build/restart-out.txt"; exit 1;
}
echo "launcher host run: empty home, explicit add, persistence, details and start passed"


# Eight played covers exercise Home's row: animated hit testing, swipe selection,
# both ends, the header, Y Options, and the square library.
cat > "$build/carousel-script.txt" <<SCRIPT
wait 35
key left
wait 20
shot $shots/carousel-first.png
key right
key right
wait 30
shot $shots/carousel-third.png
tap 400 250
wait 30
shot $shots/carousel-tap.png
swipe 750 250 450 250
wait 30
shot $shots/carousel-swipe.png
key right
key right
key right
key right
wait 30
shot $shots/carousel-last.png
key up
wait 10
shot $shots/carousel-header.png
key right
key right
key right
key a
wait 10
shot $shots/settings.png
key b
wait 10
key left
key left
key left
key down
key r
wait 15
shot $shots/carousel-library.png
key l
wait 20
key y
wait 10
shot $shots/carousel-options.png
key a
wait 5
key a
SCRIPT
SDL_VIDEODRIVER=dummy "$build/launcher_host" "$font" "$build/carousel-script.txt" --carousel-fixture > "$build/carousel-out.txt" 2>&1 || {
    cat "$build/carousel-out.txt"; exit 1;
}
grep -q "launcher returned 1 target 'sdmc:/switch/wine/drive_c/Vanguard/Game.exe'" "$build/carousel-out.txt" || {
    cat "$build/carousel-out.txt"; exit 1;
}
echo "launcher host run: Home history row, taps, swipes, boundaries, header focus and Y Options passed"

# A library larger than one page: the grid has to page, and the buttons
# have to be acted on while the icon worker posts an event per cover decoded.
# tests/launcher_shot.py stages the games and reads back which cover each card
# ended up showing, so a cover that went missing or came from another game
# fails here rather than looking right in a screenshot nobody opens.
big="$build/big/sdmc:"
mkdir -p "$big/switch/wine"
python3 "$probe/tests/launcher_shot.py" stage "$big" "$drive_c/notepad.exe" 120

cat > "$build/scroll-script.txt" <<SCRIPT
wait 60
shot $shots/scroll-top.png
key down
wait 8
key down
wait 8
key down
wait 8
key down
wait 45
shot $shots/scroll-down.png
key up
wait 8
key up
wait 8
key up
wait 8
key up
wait 45
shot $shots/scroll-back.png
key right
key right
key right
key right
key right
wait 35
shot $shots/page-upper.png
key left
wait 20
shot $shots/page-upper-back.png
key down
key right
wait 35
shot $shots/page-lower.png
key left
wait 20
shot $shots/page-lower-back.png
trigger zr 32767
trigger zr 25000
wait 35
shot $shots/page-trigger-held.png
trigger zr 0
trigger zr 32767
trigger zr 0
wait 35
shot $shots/page-trigger-next.png
key zl
trigger zl 32767
trigger zl 0
wait 35
shot $shots/page-trigger-back.png
SCRIPT
( cd "$build/big" && SDL_VIDEODRIVER=dummy "$build/launcher_host" "$font" "$build/scroll-script.txt" \
    > "$build/scroll-out.txt" 2>&1 ) || { cat "$build/scroll-out.txt"; exit 1; }
grep -q "120 catalog games (120 registered, 120 shown)" "$build/scroll-out.txt" || { cat "$build/scroll-out.txt"; exit 1; }
# Four presses down select game 20, the first slot of the third page.
python3 "$probe/tests/launcher_shot.py" check "$shots/scroll-top.png" 0 0
python3 "$probe/tests/launcher_shot.py" check "$shots/scroll-down.png" 20 0
python3 "$probe/tests/launcher_shot.py" check "$shots/scroll-back.png" 0 0
python3 "$probe/tests/launcher_shot.py" check "$shots/page-upper.png" 10 0
python3 "$probe/tests/launcher_shot.py" check "$shots/page-upper-back.png" 0 4
python3 "$probe/tests/launcher_shot.py" check "$shots/page-lower.png" 10 5
python3 "$probe/tests/launcher_shot.py" check "$shots/page-lower-back.png" 0 9
python3 "$probe/tests/launcher_shot.py" check "$shots/page-trigger-held.png" 10 9
python3 "$probe/tests/launcher_shot.py" check "$shots/page-trigger-next.png" 20 9
python3 "$probe/tests/launcher_shot.py" check "$shots/page-trigger-back.png" 0 9
echo "launcher host run: library row edges and triggers turn pages and keep every cover"

# The same library from cold, with the presses coming while the worker is still
# decoding covers and posting an event for each one. A frame that spent itself
# on one of those events would leave the buttons queued behind them, and the
# grid would still be on the first row long after the presses.
cat > "$build/busy-script.txt" <<SCRIPT
wait 18
key down
wait 2
key down
wait 2
key down
wait 2
key down
wait 25
shot $shots/busy-scrolled.png
SCRIPT
( cd "$build/big" && SDL_VIDEODRIVER=dummy "$build/launcher_host" "$font" "$build/busy-script.txt" \
    > "$build/busy-out.txt" 2>&1 ) || { cat "$build/busy-out.txt"; exit 1; }
python3 "$probe/tests/launcher_shot.py" check "$shots/busy-scrolled.png" 20 0 3
echo "launcher host run: buttons are read while the covers are still being decoded"
