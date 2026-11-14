#!/usr/bin/env python3
"""Autorun's frame rate counter over a game drawn through OpenGL
(dlls/win32u/winnx_fps.h, winnx_opengl.c): the label's pixels, and that a
game's DXVK HUD setting (FPS counter for the Wine renderer) turns it on in the
runtime, counts every swap of the screen and draws it out of a menu's way."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
win32u = root / 'dlls/win32u'
test = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "winnx_fps.h"

static uint32_t pixels[WINE_NX_FPS_W * WINE_NX_FPS_H];

static uint32_t at( unsigned int x, unsigned int y ) { return pixels[y * WINE_NX_FPS_W + x]; }

/* The pixel in the middle of a glyph cell: char i, column col, row row. */
static uint32_t cell( unsigned int i, unsigned int col, unsigned int row )
{
    return at( WINE_NX_FPS_PAD + (i * 6 + col) * WINE_NX_FPS_SCALE + 1, WINE_NX_FPS_PAD + row * WINE_NX_FPS_SCALE + 1 );
}

int main(void)
{
    unsigned int width, i, col, row, ink = 0;

    /* "60 FPS": five characters and a space, the box as wide as the text. */
    width = wine_nx_fps_draw( 60, pixels );
    assert( width == 2 * WINE_NX_FPS_PAD + 6 * 6 * WINE_NX_FPS_SCALE - WINE_NX_FPS_SCALE );
    assert( at( 0, 0 ) == WINE_NX_FPS_BOX && at( width - 1, WINE_NX_FPS_H - 1 ) == WINE_NX_FPS_BOX );
    assert( at( width, 0 ) == 0 && at( WINE_NX_FPS_W - 1, WINE_NX_FPS_H - 1 ) == 0 );
    /* 6's top row is .XX.., 0's middle row is X.X.X, F's first column is all ink. */
    assert( cell( 0, 0, 0 ) == WINE_NX_FPS_BOX && cell( 0, 2, 0 ) == WINE_NX_FPS_INK && cell( 0, 3, 0 ) == WINE_NX_FPS_INK );
    assert( cell( 1, 0, 3 ) == WINE_NX_FPS_INK && cell( 1, 1, 3 ) == WINE_NX_FPS_BOX && cell( 1, 2, 3 ) == WINE_NX_FPS_INK );
    for (row = 0; row < 7; row++) assert( cell( 3, 0, row ) == WINE_NX_FPS_INK );
    /* The space has no ink at all. */
    for (row = 0; row < 7; row++)
        for (col = 0; col < 5; col++) assert( cell( 2, col, row ) == WINE_NX_FPS_BOX );
    /* Every glyph pixel is ink or box, nothing else. */
    for (i = 0; i < WINE_NX_FPS_W * WINE_NX_FPS_H; i++)
    {
        assert( pixels[i] == WINE_NX_FPS_INK || pixels[i] == WINE_NX_FPS_BOX || pixels[i] == 0 );
        ink += pixels[i] == WINE_NX_FPS_INK;
    }
    assert( ink );

    /* 0 and the widest, and a rate past four digits is held at 9999. */
    assert( wine_nx_fps_draw( 0, pixels ) == 2 * WINE_NX_FPS_PAD + 5 * 6 * WINE_NX_FPS_SCALE - WINE_NX_FPS_SCALE );
    assert( wine_nx_fps_draw( 9999, pixels ) == WINE_NX_FPS_W );
    assert( wine_nx_fps_draw( 123456, pixels ) == WINE_NX_FPS_W );
    for (row = 0; row < 7; row++) assert( (cell( 0, 0, row ) == WINE_NX_FPS_INK) == !!(wine_nx_fps_glyph( '9' )[row] & 0x10) );

    printf( "frame rate label: %ux%u, \"60 FPS\" %u wide\n", WINE_NX_FPS_W, WINE_NX_FPS_H, width );
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='autorun-fps-') as tmp:
    c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
    c.write_text(test)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(win32u), str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

# The overlay at the screen's swap: on while the runtime says so, every swap
# counted, the label drawn on the window's framebuffer unless a menu or the
# keyboard is up, through the quad that keeps the program's state.
winnx = (win32u / 'winnx_opengl.c').read_text()
assert '#include "winnx_fps.h"' in winnx and 'extern int wine_nx_fps_counter __attribute__((weak));' in winnx
draw = winnx[winnx.index('static void nx_osk_draw( struct opengl_drawable *base )'):]
draw = draw[:draw.index('\n}\n')]
assert 'BOOL fps_on = &wine_nx_fps_counter && wine_nx_fps_counter;' in draw
assert draw.count('if (!overlay_visible && !cursor_on && !fps_on) return;') == 2
assert 'if (!overlay_visible && !cursor_on) return;' not in draw
fps = draw[draw.index('    if (fps_on)\n'):]
assert re.match(r'    if \(fps_on\)\s*\{\s*nx_fps_count\(\);\s*if \(!overlay_visible\)\s*\{\s*'
                r'p_glBindFramebuffer\( GL_DRAW_FRAMEBUFFER, 0 \);', fps)
assert fps.index('nx_fps_gl_draw( osk, height );') < fps.index('p_glBindFramebuffer( GL_READ_FRAMEBUFFER, read_fb );')
label = winnx[winnx.index('static void nx_fps_gl_draw( struct nx_osk_gl *osk, EGLint height )'):]
label = label[:label.index('\n}\n')]
assert 'wine_nx_fps_draw( nx_fps_value, pixels );' in label and 'nx_quad_gl_draw( osk, osk->fps_texture,' in label
count = winnx[winnx.index('static void nx_fps_count( void )'):]
count = count[:count.index('\n}\n')]
assert 'if (now - nx_fps_since < 5000000) return;' in count

# The runtime: the program's DXVK HUD setting, cleared for each program.
runtime = (root / 'horizon-wine/source/runtime.c').read_text()
assert 'int wine_nx_fps_counter;' in runtime
assert 'runtime_dxvk_hud = 0;\n        wine_nx_fps_counter = 0;' in runtime
assert 'runtime_dxvk_hud = settings.dxvk_hud;\n            wine_nx_fps_counter = settings.dxvk_hud != 0;' in runtime

# The launcher: DXVK HUD for DXVK, and for the Wine renderer a switch of the
# same setting called FPS counter.
launcher = (root / 'horizon-wine/source/launcher.c').read_text()
rows = launcher[launcher.index('if (p->settings.d3d != LAUNCHER_D3D_WINE)\n            {\n                ADD_ROW( ROW_DXVK_VERSION'):]
rows = rows[:rows.index('ADD_ROW( ROW_FRAME_LIMIT')]
assert re.search(r'else\s*\{[^}]*ADD_ROW\( ROW_DXVK_HUD, SECTION_GRAPHICS, "FPS counter",', rows)
assert 'row->kind = UI_ROW_SWITCH;\n                row->on = p->settings.dxvk_hud != 0;' in rows
assert 'else if (id == ROW_DXVK_HUD && p->settings.d3d == LAUNCHER_D3D_WINE)\n                *value = !*value;' in launcher
print('frame rate counter: the label, the runtime\'s switch from the DXVK HUD setting, every swap counted, '
      'and the launcher\'s FPS counter for the Wine renderer')
