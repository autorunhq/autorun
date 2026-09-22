#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
virtual = (root / 'dlls/ntdll/unix/virtual.c').read_text()
system = (root / 'dlls/ntdll/unix/system.c').read_text()
exception = (root / 'dlls/ntdll/exception.c').read_text()


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "rtlsupportapi.h"
#include "ddk/wdm.h"
#undef CONTEXT_XSTATE
#define CONTEXT_XSTATE CONTEXT_AMD64_XSTATE
#include "unwind.h"
#undef linux
#define TRACE(...) ((void)0)
#define FIXME(...) ((void)0)
#define ERR(...) assert(0)
static KUSER_SHARED_DATA shared_data;
static KUSER_SHARED_DATA *user_shared_data = &shared_data;
static USHORT native_machine = IMAGE_FILE_MACHINE_ARM64;
static USHORT supported_machines[] = {IMAGE_FILE_MACHINE_ARM64, IMAGE_FILE_MACHINE_I386, IMAGE_FILE_MACHINE_AMD64};
static unsigned supported_machines_count = ARRAY_SIZE(supported_machines);
static unsigned clocks;
static void virtual_get_system_info(SYSTEM_BASIC_INFORMATION *info, BOOL wow64) {
    assert(!wow64);
    memset(info, 0, sizeof(*info));
    info->MmNumberOfPhysicalPages = 0x100000;
}
static unsigned horizon_get_processor_count(void) { return 3; }
static void usd_update_time(void) { ++clocks; }
static void *usd_clock_thread(void *arg) { return arg; }
static int start_clock(pthread_t *thread, const pthread_attr_t *attr, void *(*entry)(void *), void *arg) {
    assert(thread && !attr && entry == usd_clock_thread && !arg);
    return 0;
}
#define pthread_create start_clock
'''

code = function(system[system.rindex('static void init_xstate_features('):],
                'static void init_xstate_features(')
code += function(system[system.rindex('void init_shared_data_cpuinfo('):],
                 'void init_shared_data_cpuinfo(')
code += function(virtual, 'void wine_nx_start_user_shared_data_clock(')
code += function(exception, 'ULONG64 WINAPI RtlGetEnabledExtendedFeatures(')
code += exception[exception.index('struct context_copy_range\n'):
                  exception.index('/**********************************************************************\n *              RtlLocateLegacyContext')]

tests = r'''
int main(void) {
    _Alignas(64) unsigned char buffer[0x800];
    ARM64EC_NT_CONTEXT *context = (void *)buffer;
    ARM64_NT_CONTEXT arm = {0}, restored;
    CONTEXT_EX *extended = NULL;
    ULONG flags = ctx_flags_arm_to_x64(CONTEXT_ARM64_FULL | CONTEXT_ARM64_FEX_YMMSTATE);
    ULONG length;

    assert(RtlInitializeExtendedContext(context, flags, &extended) == STATUS_NOT_SUPPORTED);
    assert(!extended);
    wine_nx_start_user_shared_data_clock();
    assert(clocks == 1 && shared_data.ActiveProcessorCount == 3);
    assert(shared_data.ProcessorFeatures[PF_ARM_V8_INSTRUCTIONS_AVAILABLE]);
    assert(shared_data.ProcessorFeatures[PF_SSE4_2_INSTRUCTIONS_AVAILABLE]);
    assert(shared_data.XState.EnabledFeatures == 7);
    assert(shared_data.XState.Features[XSTATE_AVX].Size == sizeof(YMMCONTEXT));

    for (unsigned offset = 0; offset < 64; offset += 16) {
        context = (void *)(buffer + offset);
        memset(buffer, 0xa5, sizeof(buffer));
        assert(!RtlGetExtendedContextLength(flags, &length) && length + offset <= sizeof(buffer));
        assert(!RtlInitializeExtendedContext(context, flags, &extended));
        assert(RtlLocateExtendedFeature(extended, XSTATE_AVX, &length));
        assert(length == sizeof(YMMCONTEXT));
        assert(!RtlLocateExtendedFeature(extended, 3, NULL));
        arm.ContextFlags = CONTEXT_ARM64_FULL | CONTEXT_ARM64_FEX_YMMSTATE;
        arm.Pc = 0x1800ee050;
        arm.Sp = 0x812f008;
        arm.X8 = 0x1e;
        for (unsigned i = 0; i < 32; ++i) {
            arm.V[i].Low = 0x1234000000000000ull + i;
            arm.V[i].High = 0x5678000000000000ull + i;
        }
        context_arm_to_x64(context, &arm);
        assert(context->Pc == arm.Pc && context->Sp == arm.Sp && context->X8 == arm.X8);
        assert(!memcmp(RtlLocateExtendedFeature(extended, XSTATE_AVX, NULL), arm.V + 16, sizeof(YMMCONTEXT)));
        context_x64_to_arm(&restored, context);
        assert(!memcmp(restored.V, arm.V, sizeof(arm.V)));
        assert(restored.Pc == arm.Pc && restored.Sp == arm.Sp);
    }
    assert(!RtlInitializeExtendedContext(buffer, CONTEXT_AMD64_FULL, &extended));
    assert(!RtlLocateExtendedFeature(extended, XSTATE_AVX, NULL));
    puts("Shared CPU context: Horizon initialization and ARM64EC XMM/YMM round trips passed");
}
'''

with tempfile.TemporaryDirectory(prefix='wine-nx-shared-context-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(fixture + code + tests)
    subprocess.run([os.environ.get('WINE_NX_HOST_CC', '/usr/bin/clang'), '-std=gnu11', '-g', '-O2',
                    '-fms-extensions', '-fshort-wchar', '-fsanitize=address,undefined',
                    '-D__WINESRC__', '-D_WIN64',
                    '-I' + str(root / 'include'), '-I' + str(root / 'dlls/ntdll'),
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=30)
