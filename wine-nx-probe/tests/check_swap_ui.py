from pathlib import Path
import re

source = (Path(__file__).resolve().parents[1] / 'source/launcher.c').read_text()
menu = source[source.index('static void settings_menu('):]
rows = menu[:menu.index('action = ui_settings_run(')]

for row, kind in (('SET_SWAP_SIZE', 'UI_ROW_DROPDOWN'),
                  ('SET_SWAP_TEST', 'UI_ROW_ACTION'), ('SET_SWAP_REMOVE', 'UI_ROW_ACTION')):
    assert re.search(rf'rows\[{row}\]\.kind\s*=\s*{kind};', rows), row
    assert re.search(rf'rows\[{row}\]\.adjustable\s*=\s*0;', rows), row

print('Swap UI: dropdown and actions do not enter inline value-editing mode')

assert 'rows[SET_SWAP_GAME].kind = UI_ROW_SWITCH;' in rows
assert 'launcher_kv_get_int( &l->look, "swap-in-game", 0 )' in rows
cmake = (Path(__file__).resolve().parents[1] / 'CMakeLists.txt').read_text()
assert 'foreach(target wine-horizon-real wine-virtual-real wine-system-real wine-win32u-real)' in cmake
assert 'target_compile_definitions(${target} PRIVATE WINE_NX_SWAP_POC)' in cmake
