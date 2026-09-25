from pathlib import Path
import re

source = (Path(__file__).resolve().parents[1] / 'source/launcher.c').read_text()
menu = source[source.index('static void settings_menu('):]
rows = menu[:menu.index('action = ui_settings_run(')]

for row, kind in (('SET_SWAP_SIZE', 'UI_ROW_DROPDOWN'),):
    assert re.search(rf'rows\[{row}\]\.kind\s*=\s*{kind};', rows), row
    assert re.search(rf'rows\[{row}\]\.adjustable\s*=\s*0;', rows), row

print('Swap UI: size selection prepares and enables paging')

assert 'SET_SWAP_TEST' not in rows
assert 'SET_SWAP_GAME' not in rows
assert 'SET_SWAP_REMOVE' not in source
assert 'static const int sizes[] = { 0, 1024, 2048 }' in source
assert 'if (swap_store_remove( path )) ui_message( ui, "SD swap", strerror(errno) );' in source
assert 'launcher_swap_prepare( ui, path, sizes[selected] )' in source
assert 'launcher_kv_set( &l->look, "swap-mb", value )' in source
cmake = (Path(__file__).resolve().parents[1] / 'CMakeLists.txt').read_text()
assert 'foreach(target wine-horizon-real wine-virtual-real wine-system-real wine-win32u-real)' in cmake
assert 'target_compile_definitions(${target} PRIVATE WINE_NX_SWAP_POC)' in cmake
