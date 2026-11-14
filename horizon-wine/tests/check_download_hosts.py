#!/usr/bin/env python3
"""Where the launcher's downloads may go (horizon-wine/source/horizon_dlls_curl.c,
allowed): the DLL repository's files and release bundle, and the game fixes
repository's index and fix files, which launcher_fixes.c fetches through the
same transport. Anything else is refused before a connection is made.

The game fixes repository was left out once: every check of its index and
every download of a fix's files ended at once ("[FIXES] check in 0 ms: ...
not valid"), so a card only ever had the index it was shipped with and could
apply no fix whose files it did not hold already."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = root / 'horizon-wine/source'
curl = (source / 'horizon_dlls_curl.c').read_text()
match = re.search(r'static int allowed\( const char \*url \)\n\{.*?\n\}\n', curl, re.S)
assert match, 'allowed() not found in horizon_dlls_curl.c'

test = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "horizon_dlls.h"
#include "game_fixes.h"

''' + match.group(0) + r'''

int main( void )
{
    char url[512];

    assert( allowed( HORIZON_DLLS_MANIFEST_URL ) );
    assert( allowed( HORIZON_DLLS_BUNDLE_URL ) );
    assert( allowed( GAME_FIXES_INDEX_URL ) );
    /* A fix's file, as game_fixes_download names it. */
    snprintf( url, sizeof(url), GAME_FIXES_RAW "%s/%s", "halo", "halo.reg" );
    assert( allowed( url ) );

    assert( !allowed( "https://example.com/index.json" ) );
    assert( !allowed( "http://raw.githubusercontent.com/autorunhq/autorun-game-fixes/main/switch/wine/game-fixes/index.json" ) );
    assert( !allowed( "https://raw.githubusercontent.com/autorunhq/autorun-game-fixes-other/main/switch/wine/game-fixes/x" ) );
    assert( !allowed( "https://raw.githubusercontent.com/someone/autorun-horizon-dlls/main/x" ) );
    assert( !allowed( "" ) );
    puts( "downloads: the DLL repository and the game fixes repository, nothing else" );
    return 0;
}
'''

with tempfile.TemporaryDirectory() as temp:
    src = Path(temp) / 'hosts.c'
    exe = Path(temp) / 'hosts'
    src.write_text(test)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(source), str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
