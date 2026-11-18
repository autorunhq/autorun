#!/usr/bin/env python3
"""The runtime's symbol file (tools/runtime_symbols.py) and its reader
(source/runtime_symbols.c): a position-independent ELF's functions named by
offset from where its code is loaded, as the NRO's are, the bare offset where
no function is known, and nothing for an address outside it."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'horizon-wine/tools'))
import runtime_symbols  # noqa: E402

program = r'''
int alpha(int x) { return x * 3; }
static int beta(int y) { return y + alpha(y); }
int main(void) { return beta(2); }
'''
test = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime_symbols.h"
int main(int argc, char **argv) {
    unsigned long alpha = strtoul(argv[2], NULL, 16), beta = strtoul(argv[3], NULL, 16), base = 0x40000000;
    char out[160], want[64];
    (void)argc;
    assert(wine_nx_runtime_symbols_open(argv[1], base) > 0);
    assert(!wine_nx_runtime_symbols_open(argv[1], base));  /* once */
    assert(wine_nx_runtime_symbol(base + alpha, out, sizeof out) && !strcmp(out, "alpha+0"));
    assert(wine_nx_runtime_symbol(base + alpha + 4, out, sizeof out) && !strcmp(out, "alpha+0x4"));
    assert(wine_nx_runtime_symbol(base + beta + 2, out, sizeof out) && !strcmp(out, "beta+0x2"));
    assert(!wine_nx_runtime_symbol(base - 4, out, sizeof out) && !out[0]);
    snprintf(want, sizeof want, "runtime+%#x", 0x10);
    assert(!wine_nx_runtime_symbol(base + 0x10, out, sizeof out) && !strcmp(out, want));
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='autorun-symbols-') as tmp:
    tmp = Path(tmp)
    (tmp / 'p.c').write_text(program)
    (tmp / 't.c').write_text(test)
    subprocess.run(['cc', '-O0', '-fpie', '-pie', str(tmp / 'p.c'), '-o', str(tmp / 'p.elf')], check=True)
    count = runtime_symbols.write(tmp / 'p.elf', tmp / 'p.sym')
    names = subprocess.run(['nm', str(tmp / 'p.elf')], capture_output=True, text=True, check=True).stdout
    alpha = re.search(r'^([0-9a-f]+) T alpha$', names, re.M).group(1)
    beta = re.search(r'^([0-9a-f]+) t beta$', names, re.M).group(1)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(root / 'horizon-wine/source'), str(tmp / 't.c'),
                    str(root / 'horizon-wine/source/runtime_symbols.c'), '-o', str(tmp / 't')], check=True)
    subprocess.run([str(tmp / 't'), str(tmp / 'p.sym'), alpha, beta], check=True)
# The exception handler (dlls/ntdll/unix/horizon.c). A page-table refresh
# takes the GPU's buffers out for a moment like any heap: a write fault on one
# is waited out, not handed to the program, where Vampire's thread spun in the
# dispatcher for good. And a fault in the runtime's own code that does reach
# the program names the function, the fault and the page.
horizon = (root / 'dlls/ntdll/unix/horizon.c').read_text()
transient = horizon[horizon.index('static BOOL horizon_transient_page_fault('):]
transient = transient[:transient.index('\n}\n')]
assert '!(info.attr & MemAttr_IsIpcMapped)' in transient and 'MemAttr_IsDeviceMapped))' not in transient
assert '(fault_status & ~3u) != 0x04' in transient  # translation faults only
native = horizon[horizon.index('static void horizon_trace_native_program_fault('):]
native = native[:native.index('\n}\n')]
assert 'svcQueryMemory( &info, &page_info, ctx->far.x )' in native and 'horizon_trace_native_backtrace( ctx );' in native
assert re.search(r'horizon_trace_program_fault\( ctx, &rec, data->teb \);\s+horizon_trace_native_program_fault\( ctx \);',
                 horizon)
# What Mesa says reaches the log: its debug messages (os_log_message, where
# mesa-switch's winsys says why a GPU submission failed) and native stderr
# (nouveau's NOUVEAU_ERR), whose device every thread's stream writes through.
cmake = (root / 'horizon-wine/CMakeLists.txt').read_text()
assert '-Wl,--wrap=os_log_message' in cmake
mesa = (root / 'horizon-wine/source/carafe_mesa.c').read_text()
assert 'void __wrap_os_log_message(const char* message)' in mesa and '"[MESA] %.*s%s"' in mesa
runtime = (root / 'horizon-wine/source/runtime.c').read_text()
assert 'devoptab_list[STD_ERR] = &native_stderr_device;' in runtime
main = runtime[runtime.index('\nint main( int argc, char **argv )'):]
assert main.index('consoleInit( NULL );') < main.index('runtime_native_stderr_hook();')
print(f'runtime symbols: {count} functions written; names, offsets, unknown and foreign addresses read back')
