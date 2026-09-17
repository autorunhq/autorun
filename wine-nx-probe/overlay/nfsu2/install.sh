#!/bin/sh
# Apply the 1280x720 HUD patch to SPEED2.EXE in this directory.
set -e
cd "$(dirname "$0")"
exe=""
for name in SPEED2.EXE SPEED2.exe speed2.exe speed2.EXE; do
    if [ -f "$name" ]; then
        exe="$name"
        break
    fi
done
if [ -z "$exe" ]; then
    echo "Copy this overlay next to SPEED2.EXE (Oct 29 2004) and run again." >&2
    echo "Скопируйте этот оверлей рядом с SPEED2.EXE (29 окт 2004) и запустите снова." >&2
    exit 1
fi
python3 patch_speed2_widescreen.py "$exe" --in-place
