Copy every file from this folder next to SPEED2.EXE. That is the install.
Скопируйте все файлы из этой папки рядом с SPEED2.EXE. Это и есть установка.

================================================================================
English
================================================================================

Need for Speed Underground 2 — one drop-on-top overlay for Wine-NX.
1280×720 16:9 HUD (hex patch) + SPEED2.keys.txt pad map.

Requires SPEED2.EXE dated Oct 29 2004 (4788224 bytes). Do not add
ThirteenAG NFSUnderground2.WidescreenFix.asi or dinput8.dll: Wine-NX
DirectInput is the controller. This patch is the HUD math only.

Install
  1. Copy this folder's contents onto the game directory, next to SPEED2.EXE.
     On Switch Wine-NX that path is:
       sdmc:/switch/wine/drive_c/nfsu/   (next to speed2.exe)
  2. On a computer with Python 3, in that same folder:
       python3 patch_speed2_widescreen.py SPEED2.EXE --in-place
     or:
       ./install.sh
     Safe to run again. It refuses any other build. First run writes
     SPEED2.EXE.bak-4x3 (the original 4:3 exe).
  3. Close Wine-NX with HOME and start it again so it reloads the folder.

Files (copy all of them)
  README.txt
  SPEED2.keys.txt                 pad overlay (frozen; not Barnyard)
  SPEED2.wine-nx.txt              launcher sidecar (visible, not hidden)
  patch_speed2_widescreen.py      Oct 29 2004 1280×720 HUD patch
  install.sh                      same as the python3 line above

HUD at 1280×720 (ThirteenAG formulas, no ASI)
  fHudScaleX = (1/Width * (Height/480)) * 2     = 0.00234375
  fHudPosX   = 640 / (640 * fHudScaleX)         = 426.667  (was 320)
  fHudPosX*2                                    = 853.333  (was 640)
  fWidescreenHudOffset = 240*(Width/Height)-320 = 106.667
  In-race widgets: .dat lookup, left -109 / right +119.
  FMV: only the real movie quad ±2/3. Frontend ±0.5 quads stay 4:3.

ThirteenAG's ASI targets SPEED2 v1.2 (Feb 9 2005). This overlay is the
Oct 29 2004 exe; HUD/FMV/res LUT byte patterns still match.

================================================================================
Русский
================================================================================

Need for Speed Underground 2 — один оверлей «положил сверху» для Wine-NX.
HUD 1280×720 16:9 (hex-патч) и раскладка пада SPEED2.keys.txt.

Нужен SPEED2.EXE от 29 октября 2004 (4788224 байт). Не кладите
NFSUnderground2.WidescreenFix.asi и dinput8.dll от ThirteenAG: пад в
Wine-NX — это DirectInput. Здесь только математика HUD.

Установка
  1. Скопируйте содержимое этой папки в каталог игры, рядом с SPEED2.EXE.
     На Switch в Wine-NX это:
       sdmc:/switch/wine/drive_c/nfsu/   (рядом с speed2.exe)
  2. На компьютере с Python 3, в той же папке:
       python3 patch_speed2_widescreen.py SPEED2.EXE --in-place
     или:
       ./install.sh
     Можно запускать повторно. Другую сборку exe скрипт отвергает.
     Первый запуск пишет SPEED2.EXE.bak-4x3 (оригинал 4:3).
  3. Закройте Wine-NX кнопкой HOME и запустите снова, чтобы папка
     перечиталась.

Файлы (копировать все)
  README.txt
  SPEED2.keys.txt                 оверлей пада (заморожен; не Barnyard)
  SPEED2.wine-nx.txt              sidecar лаунчера (не скрытый файл)
  patch_speed2_widescreen.py      патч HUD 1280×720 для Oct 29 2004
  install.sh                      то же, что строка python3 выше
