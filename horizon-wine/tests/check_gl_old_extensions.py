#!/usr/bin/env python3
"""gl-old-extensions: an old OpenGL game is shown a short extension list.

Kingpin (the Quake 2 engine) printed glGetString( GL_EXTENSIONS ) through a
4096-byte buffer on its stack. Mesa's list is about twice that: the copy ran
past the top of the stack and the game's handlers then jumped to 0x745f6c69,
the letters "il_t" of an extension's name."""
import pathlib
import re

root = pathlib.Path(__file__).resolve().parents[2]
unix_wgl = (root / 'dlls/opengl32/unix_wgl.c').read_text()
wgl_h = (root / 'include/wine/wgl.h').read_text()

# Every name kept is one Wine knows, and the list fits the smallest buffers of
# those years with room to spare.
start = unix_wgl.index('old_gl_extensions[] =')
names = re.findall(r'"(GL_\w+)"', unix_wgl[start:unix_wgl.index('};', start)])
assert len(names) == len(set(names)) and len(names) >= 30
for name in names:
    assert f'USE_GL_EXT({name})' in wgl_h, name
assert sum(len(name) + 1 for name in names) < 2048

# Only what the program is shown is cut: the client's list, before its array
# and so its string are made. The driver's own list, which Wine reads, is not.
keep = unix_wgl[unix_wgl.index('static void keep_old_gl_extensions('):unix_wgl.index('static void init_client_context(')]
assert 'client->extensions[i] = FALSE;' in keep and 'ctx->extensions' not in keep
assert 'i < WGL_FIRST_EXTENSION' in keep  # WGL's own stay
init = unix_wgl[unix_wgl.index('static void init_client_context('):]
call = 'if (&wine_nx_gl_old_extensions && wine_nx_gl_old_extensions) keep_old_gl_extensions( client, extension_names );'
assert init.index(call) < init.index('client->extension_array[count++] = i;')

# The runtime turns it on per program, and says so; a switch in the game's settings.
runtime = (root / 'horizon-wine/source/runtime.c').read_text()
assert '\nint wine_nx_gl_old_extensions;\n' in runtime
assert 'wine_nx_gl_old_extensions = settings.gl_old_extensions;' in runtime
assert '[INIT] old OpenGL extension list (gl-old-extensions)' in runtime
settings = (root / 'horizon-wine/source/launcher_settings.h').read_text()
assert 'settings->gl_old_extensions = launcher_setting_state( kv, "gl-old-extensions" ) == 1;' in settings
assert 'launcher_kv_set( kv, "gl-old-extensions", settings->gl_old_extensions ? "1" : NULL )' in settings
launcher = (root / 'horizon-wine/source/launcher.c').read_text()
assert 'ADD_ROW( ROW_GL_OLD_EXTENSIONS, SECTION_GRAPHICS, "Old OpenGL extension list",' in launcher
assert 'case ROW_GL_OLD_EXTENSIONS:' in launcher
print('old OpenGL extension list: ok')
