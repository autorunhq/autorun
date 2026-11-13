#!/usr/bin/env python3
"""A program's top-level window on Horizon (dlls/win32u/window.c show_window):
maximized to the screen, as the startup info's SW_SHOWMAXIMIZED asks
(runtime.c), unless it is smaller than a game's window (winnx_drv.c's
WINE_NX_GAME_WINDOW_MIN_W by _H). Age of Redemption's .NET configurator, a
435x520 dialog, was stretched to 1280x720 with its controls where they were
laid out; such a window keeps its own size and place, and uses up the
startup info's maximize."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]
window = (root / 'dlls/win32u/window.c').read_text()
show = window[window.index('static BOOL show_window( HWND hwnd, INT cmd )'):]
show = show[:show.index('\n}\n')]

drv = (root / 'dlls/win32u/winnx_drv.c').read_text()
width = int(re.search(r'#define WINE_NX_GAME_WINDOW_MIN_W (\d+)', drv).group(1))
height = int(re.search(r'#define WINE_NX_GAME_WINDOW_MIN_H (\d+)', drv).group(1))
assert f'(rect.right - rect.left < {width} || rect.bottom - rect.top < {height})' in show, (width, height)

decided = show.index('nx_fullscreen = !(style & (WS_CHILD | WS_POPUP)) && !get_window_relative( hwnd, GW_OWNER );')
own = show.index('if (nx_fullscreen && cmd != SW_HIDE && !(style & WS_MAXIMIZE))')
cleared = show.index('nx_fullscreen = FALSE;', own)
used_up = show.index('if (set_startup_info_flags( STARTF_USESHOWWINDOW, 0 ) & STARTF_USESHOWWINDOW)', own)
forced = show.index('if (nx_fullscreen && (cmd == SW_SHOW || cmd == SW_SHOWNORMAL || cmd == SW_SHOWDEFAULT))')
startup = show.index('&& set_startup_info_flags( STARTF_USESHOWWINDOW, 0 ) & STARTF_USESHOWWINDOW)', forced)
screen = show.index('if (nx_fullscreen && show_flag)')
# Decided before the screen's maximize and before Wine applies the startup
# info, which then finds it used up.
assert decided < own < cleared < used_up < forced < startup < screen
assert 'get_window_rect( hwnd, &rect, get_thread_dpi() )' in show[own:forced]

runtime = (root / 'horizon-wine/source/runtime.c').read_text()
assert 'params->dwFlags = STARTF_USESHOWWINDOW;\n    params->wShowWindow = SW_SHOWMAXIMIZED;' in runtime
print(f'Top-level windows: maximized unless smaller than {width}x{height}, which keep their own size')
