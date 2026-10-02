#!/usr/bin/env python3
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'horizon-wine/source/thread_profile.c').read_text()


def section(start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "thread_profile.h"
typedef uint32_t Handle, Result, u32;
typedef uint64_t u64;
typedef int32_t s32;
#define NX_PROF_MAX_THREADS 256
#define CUR_PROCESS_HANDLE 99
#define InfoType_CoreMask 1
#define R_FAILED(rc) ((rc) != 0)
'''
fixture += section('enum nx_shader_affinity', '/* horizon.c:')
fixture += r'''
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct nx_prof_thread registry[NX_PROF_MAX_THREADS];
static int four_cores_enabled = 1;
static Handle current_handle = 123;
static s32 current_core, current_priority;
static u64 current_mask, process_mask;
static unsigned int sets, follows, logs, failure;
static Handle threadGetCurHandle(void) { return current_handle; }
static Result svcGetInfo(u64 *mask, u32 type, Handle handle, u64 sub)
{
    assert(type == InfoType_CoreMask && handle == CUR_PROCESS_HANDLE && !sub);
    *mask = process_mask;
    return failure == 1;
}
static Result svcGetThreadCoreMask(s32 *core, u64 *mask, Handle handle)
{
    assert(handle == current_handle);
    *core = current_core;
    *mask = current_mask;
    return failure == 2;
}
static Result svcSetThreadCoreMask(Handle handle, s32 core, u32 mask)
{
    assert(handle == current_handle);
    if (failure == 3) return 1;
    current_core = core;
    current_mask = mask;
    sets++;
    return 0;
}
static Result __real_svcSetThreadCoreMask(Handle handle, s32 core, u32 mask)
{ return svcSetThreadCoreMask(handle, core, mask); }
static Result svcGetThreadPriority(s32 *priority, Handle handle)
{
    assert(handle == current_handle);
    *priority = current_priority;
    return 0;
}
static Result svcSetThreadPriority(Handle handle, s32 priority)
{
    assert(handle == current_handle);
    current_priority = priority;
    return 0;
}
void horizon_follow_thread_cores(void *teb, unsigned int mask) __attribute__((weak));
void horizon_follow_thread_cores(void *teb, unsigned int mask)
{
    assert(teb == (void *)1 && mask == current_mask);
    follows++;
}
static void wine_nx_runtime_trace(const char *line)
{
    assert(!pthread_mutex_trylock(&registry_mutex));
    pthread_mutex_unlock(&registry_mutex);
    assert(!strncmp(line, "[CORES]", 7));
    logs++;
}
'''
fixture += section('static int offload_thread_locked', 'static uint64_t thread_ticks')
fixture += r'''
static void reset(void)
{
    memset(registry, 0, sizeof(registry));
    registry[5] = (struct nx_prof_thread){.handle = 123, .tid = 180, .kind = 'w', .teb = 1};
    current_handle = 123;
    current_core = 0;
    current_mask = 1;
    current_priority = 59;
    process_mask = 15;
    sets = follows = logs = failure = 0;
}
int main(void)
{
    struct nx_prof_thread *thread = &registry[5];
    unsigned int token;
    reset();
    token = wine_nx_thread_pipeline_begin();
    assert(token == 6 && current_core == -1 && current_mask == 7);
    assert(thread->migratable == NX_SHADER_AFFINITY_PIPELINE && current_priority == 59);
    assert(sets == 1 && follows == 1);
    assert(!wine_nx_thread_pipeline_begin());
    wine_nx_thread_pipeline_end(0);
    assert(sets == 1 && current_mask == 7);
    struct nx_balance_thread balance[] = {{990, 0, 0, 0}, {5, -1, thread->fixed || thread->migratable, 0}};
    unsigned int before;
    nx_balance_assign(balance, 2, 3, &before);
    assert(balance[1].new_core == -1);
    wine_nx_thread_pipeline_end(token);
    assert(current_core == 0 && current_mask == 1 && current_priority == 59);
    assert(!thread->migratable && sets == 2 && follows == 2);
    wine_nx_thread_pipeline_end(token);
    assert(sets == 2);

    for (unsigned int n = 0; n < 50; n++)
        wine_nx_thread_pipeline_end(wine_nx_thread_pipeline_begin());
    assert(logs <= 1);

    reset();
    token = wine_nx_thread_pipeline_begin();
    wine_nx_thread_set_affinity(180, 2);
    wine_nx_thread_pipeline_end(token);
    assert(current_mask == 2 && current_core == 1 && thread->fixed);
    assert(!wine_nx_thread_pipeline_begin() && sets == 2);

    reset();
    token = wine_nx_thread_pipeline_begin();
    wine_nx_thread_set_name(180, "dxvk-shader");
    wine_nx_thread_pipeline_end(token);
    assert(current_mask == 7 && thread->migratable == NX_SHADER_AFFINITY_WORKER);
    assert(!wine_nx_thread_pipeline_begin() && sets == 1);
    wine_nx_thread_set_name(180, "worker");
    assert(current_mask == 1 && current_core == 0 && !thread->migratable);

    reset();
    wine_nx_thread_set_name(180, "dxvk-cs");
    assert(current_mask == 8 && current_priority == 63 && thread->helper);
    assert(!wine_nx_thread_pipeline_begin() && sets == 1);

    reset();
    token = wine_nx_thread_pipeline_begin();
    wine_nx_thread_set_name(180, "dxvk-cs");
    wine_nx_thread_pipeline_end(token);
    assert(current_mask == 8 && current_priority == 63 && thread->helper);

    for (unsigned int n = 1; n <= 3; n++) {
        reset();
        failure = n;
        assert(!wine_nx_thread_pipeline_begin());
        assert(current_mask == 1 && current_priority == 59 && !thread->migratable && !sets && !follows);
    }
    reset();
    token = wine_nx_thread_pipeline_begin();
    failure = 3;
    wine_nx_thread_pipeline_end(token);
    assert(current_mask == 7 && thread->migratable == NX_SHADER_AFFINITY_PIPELINE);
    failure = 0;
    wine_nx_thread_pipeline_end(token);
    assert(current_mask == 1 && !thread->migratable);

    reset();
    process_mask = 6;
    current_core = 1;
    current_mask = 2;
    token = wine_nx_thread_pipeline_begin();
    assert(current_mask == 6);
    wine_nx_thread_pipeline_end(token);
    assert(current_core == 1 && current_mask == 2);

    reset();
    current_core = -1;
    current_mask = 3;
    wine_nx_thread_pipeline_end(wine_nx_thread_pipeline_begin());
    assert(current_core == -1 && current_mask == 3);

    reset();
    thread->fixed = 1;
    assert(!wine_nx_thread_pipeline_begin() && !sets);
    reset();
    thread->kind = 's';
    assert(!wine_nx_thread_pipeline_begin() && !sets);
    reset();
    current_handle = 456;
    assert(!wine_nx_thread_pipeline_begin() && !sets);
    wine_nx_thread_pipeline_end(NX_PROF_MAX_THREADS + 1);
    puts("shader affinity: scope, nesting, balancing, explicit affinity, worker roles and SVC errors passed");
    return 0;
}
'''

thunks = (root / 'dlls/winevulkan/vulkan_thunks.c').read_text()
for bits in (32, 64):
    for kind in ('Compute', 'Graphics'):
        function = f'thunk{bits}_vkCreate{kind}Pipelines'
        body = thunks.split(f'static NTSTATUS {function}(void *args)', 1)[1].split('\n}', 1)[0]
        assert body.count('wine_nx_thread_pipeline_begin();') == 1
        assert body.count('wine_nx_thread_pipeline_end(nx_pipeline_affinity);') == 1
        assert body.index('wine_nx_thread_pipeline_begin();') < body.index('->p_vkCreate')
        assert body.index('->p_vkCreate') < body.index('wine_nx_thread_pipeline_end(')

with tempfile.TemporaryDirectory(prefix='autorun-shader-affinity-') as tmp:
    path = Path(tmp)
    test = path / 'affinity.c'
    test.write_text(fixture)
    for profile in (False, True):
        command = [os.environ.get('CC', 'cc'), '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-address', '-pthread',
                   '-I', str(root / 'horizon-wine/source'), str(test), '-o', str(path / 'affinity')]
        command += shlex.split(os.environ.get('CFLAGS', ''))
        if profile:
            command += ['-DWINE_NX_SHADER_PROFILE']
        subprocess.run(command, check=True)
        subprocess.run([str(path / 'affinity')], check=True, timeout=30)
