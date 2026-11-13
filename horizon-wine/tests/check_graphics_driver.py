#!/usr/bin/env python3
"""A program's Graphics driver setting (launcher_settings.h): read from its
settings file, written back, the key it had first moved over, and anything else
read as the default; and the runtime's choice of the runtime to start it in."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = root / 'horizon-wine/source'
test = r'''
#include <assert.h>
#include <stdio.h>
#include "launcher_settings.h"
static void roundtrip(const char *text, int want, const char *key_after) {
    struct launcher_kv kv = {{0}, 0}; struct launcher_settings s;
    snprintf(kv.text, sizeof(kv.text), "%s", text); kv.size = strlen(kv.text);
    launcher_settings_read(&kv, &s);
    assert((int)s.graphics_driver == want);
    assert(launcher_settings_write(&kv, &s));
    if (key_after) assert(strstr(kv.text, key_after)); else assert(!strstr(kv.text, "graphics-driver"));
    assert(!strstr(kv.text, "gl-driver"));
}
int main(void) {
    struct launcher_kv kv = {{0}, 0}; struct launcher_settings s;
    roundtrip("", LAUNCHER_GRAPHICS_NVC0, NULL);
    roundtrip("graphics-driver=zink\n", LAUNCHER_GRAPHICS_ZINK, "graphics-driver=zink");
    roundtrip("graphics-driver=nxvk\n", LAUNCHER_GRAPHICS_NXVK, "graphics-driver=nxvk");
    roundtrip("gl-driver=zink\n", LAUNCHER_GRAPHICS_ZINK, "graphics-driver=zink");
    roundtrip("graphics-driver=bogus\n", LAUNCHER_GRAPHICS_NVC0, NULL);
    launcher_settings_read(&kv, &s);
    s.graphics_driver = LAUNCHER_GRAPHICS_COUNT;
    assert(!launcher_settings_write(&kv, &s));
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='autorun-graphics-driver-') as tmp:
    c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
    c.write_text(test)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(source), str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

# The runtime goes to the other runtime only for a program, never for the
# components setup, and only with the other runtime on the card.
runtime = (source / 'runtime.c').read_text()
hop = runtime[runtime.index('static int runtime_switch_graphics_runtime('):]
hop = hop[:hop.index('\n}\n')]
assert 'if (wants_nxvk == runtime_is_nxvk) return 0;' in hop
assert 'access( other, F_OK )' in hop and 'run_next_push( target )' in hop
assert re.search(r'!runtime_components_run &&\s+runtime_switch_graphics_runtime\(', runtime)
assert '"/wine-nx-runtime-nxvk.nro"' in runtime
# Started without a program, the nxvk runtime starts the card's own runtime
# for the launcher rather than draw one through a Mesa it cannot vouch for.
handover = runtime[runtime.index('#ifdef WINE_NX_NXVK\n        /* The nxvk runtime runs the programs set to it'):]
handover = handover[:handover.index('#endif')]
assert 'envSetNextLoad( RUNTIME_NRO_MESA_SWITCH, RUNTIME_NRO_MESA_SWITCH )' in handover
assert handover.index('envSetNextLoad') < runtime.index('chosen = wine_nx_launcher_run(') - runtime.index(handover)
# The console menu's catalog is not on the stack (some 560 KB of it).
console = (source / 'launcher_console.c').read_text()
assert 'static struct launcher_catalog catalog;' in console
# nxvk's Mesa sizes the GPU's memory from sysconf(_SC_PHYS_PAGES), which
# newlib does not answer; without it NVK refuses the GPU and Zink has none.
platform = (source / 'runtime_platform.c').read_text()
sysconf = platform[platform.index('long sysconf( int name )\n{'):]
sysconf = sysconf[:sysconf.index('\n}\n')]
assert '_SC_PHYS_PAGES' in sysconf and 'InfoType_TotalMemorySize' in sysconf
# Both runtimes run in the loader's one process: whatever one leaves running
# goes on into the next. USB's watcher thread is ended on every way out, and
# the nxvk runtime starts the card's runtime for the launcher itself.
leave = runtime[runtime.index('static int leave_cleanly( void )'):]
leave = leave[:leave.index('\n}\n')]
assert 'wine_nx_usb_stop()' in leave and leave.index('wine_nx_usb_stop()') < leave.index('socketExit();')
back = runtime[runtime.index('static int return_to_launcher( void )'):]
back = back[:back.index('\n}\n')]
assert 'if (runtime_is_nxvk && !access( RUNTIME_NRO_MESA_SWITCH, F_OK )) next = RUNTIME_NRO_MESA_SWITCH;' in back
assert 'memory_left_behind_ex( 1, &mine );' in handover and handover.index('leave_cleanly();') < handover.index('memory_left_behind_ex')
# NVK's compressed images each take an allocation and compression tags of
# their own, and Zink ran out of them: off for nxvk and for Zink on
# mesa-switch, before a Vulkan instance reads NVK_DEBUG, unless asked for.
compression = runtime[runtime.index('static void runtime_nvk_compression( const char *what )'):]
compression = compression[:compression.index('\n}\n')]
assert 'config_bool( "nvk-compression", 0,' in compression and 'no_compression' in compression
init = runtime[runtime.index('log_line( "[INIT] Mesa from nxvk:'):]
assert init.index('runtime_nvk_compression(') < init.index('wine_nx_vulkan_probe();') < init.index('wine_nx_graphics_selftest();')
zink = runtime[runtime.index('if (settings.graphics_driver == LAUNCHER_GRAPHICS_ZINK && runtime_mesa_has_zink())'):]
zink = zink[:zink.index('unsetenv( "MESA_SWITCH_GL_DRIVER" );')]
assert 'runtime_nvk_compression(' in zink and 'setenv( "MESA_SWITCH_GL_DRIVER", "zink", 1 );' in zink
# The switch-dev image's mesa-switch has no Zink, and its EGL then refuses the
# display: a game set to Zink runs on nxvk, Zink on NVK, which keeps it.
has = runtime[runtime.index('static int runtime_mesa_has_zink( void )'):]
has = has[:has.index('\n}\n')]
assert "extern void zink_create_screen( void ) __attribute__((weak));" in has
assert '(zink && (runtime_is_nxvk || !runtime_mesa_has_zink()))' in runtime
assert '.mesa_zink = runtime_mesa_has_zink(),' in runtime
# 32-bit programs' persistent buffer maps: NVK claims placed maps and fd export
# that it cannot do on Horizon, so Wine's Vulkan path is off on the Switch and
# buffer storage is pinned memory's (mesa-switch) or hidden (Zink), not left to
# fail (Vampire on nxvk, WineD3D's "Failed to map chunk memory").
wgl = (root / 'dlls/opengl32/unix_wgl.c').read_text()
vk = wgl[wgl.index('static BOOL initialize_vk_device( TEB *teb, const struct opengl_context *ctx )'):]
vk = vk[:vk.index('if (!ctx->extensions[GL_EXT_memory_object_fd] )')]
assert re.search(r'#ifdef __SWITCH__\n(    /\*.*\n|     \*.*\n)*    return FALSE;\n#endif', vk)
# mesa-switch frees a destroyed context's buffer lists while the screen's one
# pushbuffer still has them linked: the loading screen's first frame is drawn
# with the program's context, and none is made or destroyed for it (Vampire
# froze on "corrupt or oversized bufctx list" when one was).
winnx = (root / 'dlls/win32u/winnx_opengl.c').read_text()
first = winnx[winnx.index('static void nx_screen_first_frame('):]
first = first[:first.index('\n}\n')]
assert 'eglCreateContext' not in first and 'eglDestroyContext' not in first
assert 'EGLContext context = funcs->p_eglGetCurrentContext();' in first
assert 'eglDestroyContext' not in winnx
launcher = (source / 'launcher.c').read_text()
assert '"wine-nx-runtime-nxvk.nro"' in launcher
print('graphics driver: setting read and written, old key moved over, the runtime started for nxvk only when on the card')
