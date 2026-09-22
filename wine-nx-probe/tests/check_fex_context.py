#!/usr/bin/env python3
"""Exercise FEX exception entry/restore assembly with a user-mode SVC stand-in."""
from pathlib import Path
import platform
import subprocess
import tempfile

if platform.system() != 'Linux' or platform.machine() not in ('aarch64', 'arm64'):
    raise SystemExit('FEX context: requires AArch64 Linux')

probe = Path(__file__).resolve().parents[1]
source = (probe / 'source/fex_context.S').read_text()
source = source.replace('    svc #0x28', '    b test_return')
source = source.replace('    mrs x2, tpidrro_el0',
                        '    adrp x2, test_tls\n    ldr x2, [x2, :lo12:test_tls]')
assembly = ['.text', '.global test_invoke', 'test_invoke:', 'sub sp, sp, #256']
assembly += [f'stp x{i}, x{i + 1}, [sp, #{(i - 19) * 8}]' for i in range(19, 31, 2)]
assembly += [f'stp q{i}, q{i + 1}, [sp, #{96 + (i - 8) * 16}]' for i in range(8, 16, 2)]
assembly += ['mrs x3, fpcr', 'mrs x4, fpsr', 'stp x3, x4, [sp, #224]',
             'mov x8, x0', 'mov w0, w2', 'ldr w3, [x8, #784]', 'msr fpcr, x3',
             'ldr w3, [x8, #788]', 'msr fpsr, x3']
assembly += [f'ldp q{i}, q{i + 1}, [x8, #{272 + i * 16}]' for i in range(0, 32, 2)]
assembly += [f'ldr x{i}, [x8, #{8 + i * 8}]' for i in range(9, 31)]
assembly += ['b __wrap___libnx_exception_entry', '.global __real___libnx_exception_entry',
             '__real___libnx_exception_entry:', 'adrp x2, original_called',
             'mov w3, #1', 'str w3, [x2, :lo12:original_called]', 'b test_return',
             '.global __libnx_exception_returnentry', '__libnx_exception_returnentry: brk #1',
             '.global test_return', 'test_return:', 'adrp x0, captured', 'add x0, x0, :lo12:captured']
assembly += [f'str x{i}, [x0, #{8 + i * 8}]' for i in range(9, 30)]
assembly += [f'stp q{i}, q{i + 1}, [x0, #{272 + i * 16}]' for i in range(0, 32, 2)]
assembly += ['mrs x2, fpcr', 'str w2, [x0, #784]', 'mrs x2, fpsr', 'str w2, [x0, #788]',
             'ldp x3, x4, [sp, #224]', 'msr fpcr, x3', 'msr fpsr, x4']
assembly += [f'ldp x{i}, x{i + 1}, [sp, #{(i - 19) * 8}]' for i in range(19, 31, 2)]
assembly += [f'ldp q{i}, q{i + 1}, [sp, #{96 + (i - 8) * 16}]' for i in range(8, 16, 2)]
assembly += ['add sp, sp, #256', 'ret', '.section .note.GNU-stack,"",%progbits']
test = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct context { uint32_t flags, pstate; uint64_t x[31], sp, pc; unsigned char v[512]; uint32_t fpcr, fpsr; };
struct kernel_context { uint64_t x[9], lr, sp, pc; uint32_t pstate, afsr0, afsr1, esr; uint64_t far; };
struct dump { uint32_t error, fpcr, fpsr, magic; uint64_t x[29], fp, lr, sp, pc, padding; unsigned char v[512];
              uint32_t pstate, afsr0, afsr1, esr; uint64_t far; };
struct slot { uintptr_t tls; void *dump, *top; } wine_nx_fex_exception_slots[256];
struct context captured;
uintptr_t test_tls;
int original_called;
extern void test_invoke(const struct context *, struct kernel_context *, unsigned);
extern void wine_nx_fex_continue_context(void), __libnx_exception_returnentry(void);
static unsigned char stacks[2][0x10400] __attribute__((aligned(16)));

static void check_live(const struct context *expected)
{
    assert(!memcmp(&captured.x[9], &expected->x[9], 21 * 8));
    assert(!memcmp(captured.v, expected->v, sizeof(expected->v)));
    assert(captured.fpcr == expected->fpcr && captured.fpsr == expected->fpsr);
}

int main(void)
{
    struct context expected = { .pstate = 0xa0000000, .sp = 0x55550000, .pc = 0x44440000,
                                .fpcr = 0x400000, .fpsr = 1 };
    struct kernel_context kernel, before;
    for (unsigned i = 0; i < 31; i++) expected.x[i] = 0x180000 + i;
    for (unsigned i = 0; i < sizeof(expected.v); i++) expected.v[i] = i ^ (i >> 8);
    test_tls = (uintptr_t)&kernel;
    for (unsigned n = 0; n < 2; n++)
    {
        struct slot *slot = &wine_nx_fex_exception_slots[n * 173];
        slot->tls = test_tls;
        slot->dump = stacks[n];
        slot->top = stacks[n] + sizeof(stacks[n]);
        for (unsigned nested = 0; nested < 2; nested++)
        {
            memset(&kernel, 0, sizeof(kernel));
            memcpy(kernel.x, expected.x, sizeof(kernel.x));
            kernel.lr = expected.x[30];
            kernel.sp = nested ? (uintptr_t)slot->top - 0x800 : expected.sp;
            kernel.pc = expected.pc;
            kernel.pstate = expected.pstate;
            kernel.afsr0 = 4; kernel.afsr1 = 5; kernel.esr = 0x92000007; kernel.far = 0x1234;
            before = kernel;
            memset(slot->dump, 0xee, 0x400);
            test_invoke(&expected, &kernel, 0x301);
            assert(!original_called);
            const struct dump *dump = (void *)(uintptr_t)kernel.x[0];
            assert((uintptr_t)dump == (nested ? before.sp - 0x400 : (uintptr_t)slot->dump));
            assert(kernel.sp == (nested ? (uintptr_t)dump : (uintptr_t)slot->top));
            assert(kernel.pc == (uintptr_t)__libnx_exception_returnentry);
            assert(!memcmp(dump->x, expected.x, sizeof(dump->x)));
            assert(dump->fp == expected.x[29] && dump->lr == expected.x[30]);
            assert(dump->sp == before.sp && dump->pc == before.pc && dump->pstate == before.pstate);
            assert(dump->error == 0x301 && dump->magic == 0xfec0);
            assert(dump->fpcr == expected.fpcr && dump->fpsr == expected.fpsr);
            assert(dump->afsr0 == 4 && dump->afsr1 == 5 && dump->esr == before.esr && dump->far == 0x1234);
            assert(!memcmp(dump->v, expected.v, sizeof(expected.v)));
            if (nested) assert(*(unsigned char *)slot->dump == 0xee);
            check_live(&expected);
        }
        slot->tls = 0;
    }
    kernel.x[0] = (uintptr_t)&expected;
    kernel.pc = (uintptr_t)wine_nx_fex_continue_context;
    test_invoke(&expected, &kernel, 0);
    assert(!original_called);
    assert(!memcmp(kernel.x, expected.x, sizeof(kernel.x)));
    assert(kernel.lr == expected.x[30] && kernel.sp == expected.sp && kernel.pc == expected.pc);
    assert(kernel.pstate == expected.pstate);
    check_live(&expected);
    kernel.pc = expected.pc;
    test_invoke(&expected, &kernel, 0);
    assert(original_called);
    puts("FEX context: capture, nesting, thread slots and full register restore passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wine-nx-fex-context-') as directory:
    directory = Path(directory)
    (directory / 'entry.S').write_text(source)
    (directory / 'driver.S').write_text('\n'.join(assembly) + '\n')
    (directory / 'test.c').write_text(test)
    output = directory / 'test'
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-O2', '-o', str(output),
                    str(directory / 'test.c'), str(directory / 'entry.S'), str(directory / 'driver.S')], check=True)
    subprocess.run([str(output)], check=True)
