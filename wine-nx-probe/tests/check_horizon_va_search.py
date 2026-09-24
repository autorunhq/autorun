#!/usr/bin/env python3
"""Exercise Wine's actual bounded mmap probe with native mappings.

Wine's free-range tree can contain libnx stacks/JIT mappings it does not know
about. Wine must exhaustively search its bounded guest address space for a
usable aligned gap and stop immediately on errors other than EEXIST.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/ntdll/unix/virtual.c').read_text()


def block(text, marker):
    start = text.index(marker)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


fixture = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define __SWITCH__ 1
typedef int BOOL;
typedef uintptr_t UINT_PTR;
typedef uintptr_t ULONG_PTR;
#define TRUE 1
#define FALSE 0
#define MAP_FAILED ((void *)-1)
#define STATUS_NO_MEMORY 1
#define TRACE(...) ((void)0)
#define WARN(...) ((void)0)
#define ERR(...) ((void)0)
#define min(a,b) ((size_t)(a)<(size_t)(b)?(a):(b))
static const uintptr_t granularity_mask = 0xffff;
static uintptr_t gap_start, gap_end;
static int probes, fail_errno;
static void *anon_mmap_tryfixed(void *ptr, size_t size, int prot, int flags)
{
    uintptr_t addr = (uintptr_t)ptr;
    (void)prot; (void)flags;
    probes++;
    if (!fail_errno && addr >= gap_start && addr <= gap_end && size <= gap_end - addr)
        return ptr;
    errno = fail_errno ? fail_errno : EEXIST;
    return MAP_FAILED;
}
'''
fixture += block(source, 'static void* try_map_free_area(') + '\n'
fixture += r'''
/* Simulate one Wine-free range containing mappings owned by libnx. The real
 * probe below discovers the occupied addresses through mmap's EEXIST result. */
static void *search(void *base, void *end, size_t size, BOOL top_down)
{
    ptrdiff_t step = granularity_mask + 1;
    void *start = (void *)(((uintptr_t)base + granularity_mask) & ~granularity_mask);
    if (top_down)
    {
        step = -step;
        start = (void *)(((uintptr_t)end - size) & ~granularity_mask);
    }
    return try_map_free_area(base, end, step, start, size, 0);
}
int main(void)
{
    const size_t request = 60032u * 1024; /* race-load reservation from the log */
    void *out;
    /* Only a bounded 64 MiB gap accepts the request. */
    gap_start = 0x05000000;
    gap_end = gap_start + 0x04000000;
    out = search((void *)0x10000, (void *)0x40000000, request, FALSE);
    assert((uintptr_t)out == gap_start && probes > 1);
    probes = 0;
    out = search((void *)0x10000, (void *)0x40000000, request, TRUE);
    assert((uintptr_t)out >= gap_start && (uintptr_t)out + request <= gap_end);
    assert(probes > 1);
    /* The search must obey the caller's bounds and must not fabricate memory
     * when the remaining gap is too small or mmap fails for another reason. */
    probes = 0;
    assert(!search((void *)0x10000, (void *)gap_start, request, FALSE));
    gap_end = gap_start + request - 0x1000;
    assert(!search((void *)0x10000, (void *)0x10000000, request, FALSE));
    fail_errno = ENOMEM;
    probes = 0;
    assert(!search((void *)0x10000, (void *)0x10000000, request, FALSE));
    assert(probes == 1);
    /* Fast path: a directly available range needs one probe. */
    fail_errno = 0;
    gap_end = gap_start + request;
    probes = 0;
    assert(search((void *)gap_start, (void *)gap_end, request, FALSE) == (void *)gap_start);
    assert(probes == 1);
    puts("Horizon VA search: exhaustive aligned gap search, bounds and ENOMEM termination passed");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'va_search.c'
    exe = Path(tmp) / 'va_search'
    c.write_text(fixture)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(c), '-o', str(exe)], check=True)
    env = os.environ.copy()
    env['ASAN_OPTIONS'] = 'halt_on_error=1'
    env['UBSAN_OPTIONS'] = 'halt_on_error=1'
    subprocess.run([str(exe)], check=True, env=env)
