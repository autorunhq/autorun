#!/usr/bin/env python3
"""gl-thread: Mesa's glthread for an OpenGL game on nxvk.

nxvk's Switch EGL (src/egl/drivers/horizon/egl_horizon.c) makes its contexts
with st_api_create_context and never started glthread, and Mesa's DRI frontend
would leave it off with fewer than four CPUs anyway. nxvk-patches/0001 starts
it when mesa_glthread is true and, as Mesa's DRI frontend does, has the GL
thread finish before the application's thread uses the pipe context or a
drawable: before a swap, a swap interval, a make-current, a context's or a
surface's destruction. The runtime sets mesa_glthread for a program whose
settings say gl-thread=1, in nxvk's runtime only; build-nxvk.sh applies the
patches and keeps its package per patch, and CI's cache key follows them."""
import pathlib
import re
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
hw = root / 'horizon-wine'
patch = (hw / 'nxvk-patches/0001-egl-horizon-glthread.patch').read_text()

# The patch: only nxvk's Switch EGL, and what it adds.
assert re.findall(r'^\+\+\+ b/(\S+)', patch, re.M) == ['src/egl/drivers/horizon/egl_horizon.c']
added = '\n'.join(line[1:] for line in patch.splitlines() if line.startswith('+') and not line.startswith('+++'))
assert '#include "main/glthread.h"' in added and '#include "util/u_debug.h"' in added
assert re.search(r'if \(debug_get_bool_option\("mesa_glthread", false\)\)\s*_mesa_glthread_init\(ctx->st->ctx\);', added)
# glthread's thread calls the frontend's set_background_context as it starts:
# left NULL, Doom 3 jumped to address 0 there before its first frame.
assert 'hdpy->fscreen.set_background_context = horizon_set_background_context;' in added
assert 'horizon_set_background_context(struct st_context *st,' in added
assert 'horizon_glthread_finish(struct st_context *st)' in added and '_mesa_glthread_finish(st->ctx);' in added
# Each place the application's thread reaches into the context, in the hunk of
# the function it belongs to.
hunks = re.split(r'^@@ .*?@@', patch, flags=re.M)
def hunk_of(function):
    found = [h for h in hunks if function in h]
    assert found, function
    return '\n'.join(found)
for function, call in (('horizon_destroy_surface', 'horizon_glthread_finish(st_api_get_current());'),
                       ('struct horizon_flush_args args', 'horizon_glthread_finish(st);'),
                       ('zink_kopper_set_swap_interval', 'horizon_glthread_finish(st_api_get_current());'),
                       ('st_destroy_context(ctx->st);', 'horizon_glthread_finish(ctx->st);'),
                       ('st_api_make_current(NULL, NULL, NULL);', 'horizon_glthread_finish(ctx->st);')):
    assert re.search(r'^\+\s+' + re.escape(call), hunk_of(function), re.M), function
make_current = hunk_of('st_api_make_current(NULL, NULL, NULL);')
assert make_current.index('+   horizon_glthread_finish(st_api_get_current());') < \
    make_current.index(' st_api_make_current(NULL, NULL, NULL);')
swap = hunk_of('struct horizon_flush_args args')
# Right after the flush's arguments are made, so before the flush that follows them.
assert swap.index(' struct horizon_flush_args args = { st->pipe, back };') < \
    swap.index('+   horizon_glthread_finish(st);')

# build-nxvk.sh: every patch, on the commit's own files, in the package's stamp.
build = (hw / 'build-nxvk.sh').read_text()
assert 'git -C "$src" checkout -q --force "$commit"' in build
assert build.count('for patch in "$hw"/nxvk-patches/*.patch; do') == 2
assert 'git -C "$src" apply "$patch"' in build and 'echo "$stamp" > "$pkg/commit"' in build
assert build.index('checkout -q --force') < build.index('git -C "$src" apply')
workflow = (root / '.github/workflows/build.yml').read_text()
assert "hashFiles('horizon-wine/build-nxvk.sh', 'horizon-wine/nxvk-patches/*.patch')" in workflow

# The patch applies to the pinned commit's file when the checkout is there.
nxvk = hw / 'vendor/nxvk'
commit = re.search(r'^commit=([0-9a-f]{40})$', build, re.M).group(1)
if (nxvk / '.git').exists():
    with tempfile.TemporaryDirectory() as directory:
        original = subprocess.run(['git', '-C', str(nxvk), 'show', f'{commit}:src/egl/drivers/horizon/egl_horizon.c'],
                                  capture_output=True, text=True)
        if original.returncode == 0:
            path = pathlib.Path(directory)
            (path / 'src/egl/drivers/horizon').mkdir(parents=True)
            (path / 'src/egl/drivers/horizon/egl_horizon.c').write_text(original.stdout)
            subprocess.run(['git', 'apply', '--check', str(hw / 'nxvk-patches/0001-egl-horizon-glthread.patch')],
                           cwd=path, check=True)

# The setting: read, written, cleared with the rest, and only nxvk's runtime acts on it.
settings = (hw / 'source/launcher_settings.h').read_text()
assert 'settings->gl_thread = launcher_setting_state( kv, "gl-thread" ) == 1;' in settings
assert 'launcher_kv_set( kv, "gl-thread", settings->gl_thread ? "1" : NULL )' in settings
runtime = (hw / 'source/runtime.c').read_text()
assert 'runtime_gl_thread = 0;' in runtime and 'runtime_gl_thread = settings.gl_thread;' in runtime
block = runtime[runtime.index('    if (runtime_gl_thread)\n'):]
block = block[:block.index('#endif')]
assert block.index('#ifdef WINE_NX_NXVK') < block.index('setenv( "mesa_glthread", "true", 1 );')
# Set before the program starts, after its settings were read.
assert runtime.index('runtime_gl_thread = settings.gl_thread;') < runtime.index('setenv( "mesa_glthread", "true", 1 );')
launcher = (hw / 'source/launcher.c').read_text()
assert 'ROW_GL_THREAD' in launcher and 'p->settings.gl_thread = action == UI_ACTION_RESET ? 0 : !p->settings.gl_thread;' in launcher
print('gl-thread: the nxvk patch starts glthread on mesa_glthread and finishes it before swaps, make-current and '
      'destruction; build-nxvk.sh applies it; the setting reaches nxvk only')
