#!/usr/bin/env python3
"""A 32-bit program's write-only, explicitly flushed buffer map through a
32-bit copy (dlls/opengl32/unix_wgl.c) copies nothing in and copies back only
what is flushed, as it is flushed; the copies are counted by kind for
[PROGRESS]."""
from pathlib import Path

root = Path(__file__).resolve().parents[2]
wgl = (root / 'dlls/opengl32/unix_wgl.c').read_text()
runtime = (root / 'horizon-wine/source/runtime.c').read_text()

m = wgl[wgl.index('static void *wow64_map_buffer('):wgl.index('static BOOL wow64_unmap_buffer(')]
assert 'buffer->copy_on_flush = (access & (GL_MAP_WRITE_BIT | GL_MAP_READ_BIT | GL_MAP_FLUSH_EXPLICIT_BIT))' in m
assert m.index('buffer->copy_on_flush =') < m.index('memcpy( buffer->map_ptr, buffer->host_ptr, length );'), \
    'the write-only case returns before the copy in'
f = wgl[wgl.index('static void flush_buffer('):wgl.index('static int find_vk_memory_type(')]
assert 'if (buffer->copy_on_flush)' in f
assert 'memcpy( (char *)buffer->host_ptr + offset, (char *)buffer->map_ptr + offset, length );' in f
u = wgl[wgl.index('static BOOL wow64_unmap_buffer('):]
u = u[:u.index('\n}\n')]
assert 'buffer->copy_on_flush = FALSE;' in u
# The flush reaches the copy before the driver flushes the range.
for fn in ('void wow64_glFlushMappedBufferRange(', 'void wow64_glFlushMappedNamedBufferRange('):
    body = wgl[wgl.index(fn):]
    body = body[:body.index('\n}\n')]
    assert body.index('flush_buffer(') < body.index('use_driver_buffer_map('), fn
for name in ('copy_in_read_mb', 'copy_in_write_mb', 'copy_back_mb', 'copy_saved_mb'):
    assert name in runtime, name
print('wow64 copy on flush: ok')
