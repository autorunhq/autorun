#!/usr/bin/env python3
"""Exercise the runtime's core-3 policy with mocked kernel permissions."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "wine-nx-probe/source/thread_profile.c").read_text()


def definition(name, structure=False):
    pattern = r"^struct " + name + r"\s*\{" if structure else r"^(?:static )?(?:int|void|Result) " + name + r"\([^;{]*\)\s*\{"
    match = re.search(pattern, source, re.M)
    assert match, name
    pos, depth = match.end(), 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[match.start():pos] + (";\n" if structure else "\n")


fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include "thread_profile.h"
typedef uint64_t u64;
typedef uint32_t u32, Handle, Result;
typedef int32_t s32;
#define NX_PROF_MAX_THREADS 4
#define CUR_PROCESS_HANDLE 0
#define InfoType_CoreMask 0
#define InfoType_PriorityMask 1
#define R_SUCCEEDED(r) (!(r))
#define R_FAILED(r) (!!(r))
#define CUR_THREAD_HANDLE 10
static u64 allowed_cores = 7, priorities = ~(UINT64_C(1) << 63), current_mask = 2;
static s32 current_core = 1, current_priority = 59;
static int fail_info, fail_core, fail_priority, calls, followed;
static char log_line[160];
static int svcGetInfo(u64 *out, int type, Handle process, int subtype) {
    (void)process; (void)subtype;
    *out = type == InfoType_CoreMask ? allowed_cores : priorities;
    return fail_info;
}
static int svcGetThreadCoreMask(s32 *core, u64 *mask, Handle handle) {
    assert(handle == 10); *core = current_core; *mask = current_mask; return 0;
}
static int svcGetThreadPriority(s32 *priority, Handle handle) {
    assert(handle == 10); *priority = current_priority; return 0;
}
Result __wrap_svcSetThreadCoreMask(Handle handle, s32 core, u32 mask);
#define svcSetThreadCoreMask __wrap_svcSetThreadCoreMask
Result __real_svcSetThreadCoreMask(Handle handle, s32 core, u32 mask) {
    assert(handle == 10); calls++;
    if (fail_core || !mask || (mask & ~allowed_cores)) return 1;
    assert(core == -1 || (mask & (1u << core))); current_core = core; current_mask = mask; return 0;
}
static int svcSetThreadPriority(Handle handle, s32 priority) {
    assert(handle == 10); calls++;
    if (fail_priority || !(priorities & (UINT64_C(1) << priority))) return 1;
    current_priority = priority; return 0;
}
static void wine_nx_runtime_trace(const char *line) { snprintf(log_line, sizeof(log_line), "%s", line); }
static Handle threadGetCurHandle(void) { return 10; }
static uint64_t thread_ticks(Handle handle) { assert(handle == 10); return 100; }
void horizon_follow_thread_cores(void *teb, unsigned mask) __attribute__((weak));
void horizon_follow_thread_cores(void *teb, unsigned mask) { assert(teb == (void *)123); followed = mask; }
'''
globals_ = r'''
static struct nx_prof_thread registry[NX_PROF_MAX_THREADS];
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static int four_cores_enabled;
static __thread int affinity_fixed;
'''
test = r'''
int main(void) {
    assert(!wine_nx_four_cores_available());
    allowed_cores = 15;
    assert(!wine_nx_four_cores_available());
    priorities = UINT64_MAX;
    assert(wine_nx_four_cores_available());
    assert(!svcSetThreadCoreMask(10, -1, 15));
    assert(current_mask == 7);
    assert(svcSetThreadCoreMask(10, 3, 8));
    assert(current_mask == 7);
    assert(!svcSetThreadCoreMask(10, 1, 2));
    calls = 0;
    fail_info = 1; assert(!wine_nx_four_cores_available()); fail_info = 0;
    assert(nx_thread_graphics_worker("dxvk-cs"));
    assert(nx_thread_graphics_worker("wined3d_cs"));
    assert(nx_thread_graphics_worker("vkd3d_queue"));
    assert(!nx_thread_graphics_worker("dxvk-submit"));
    assert(!nx_thread_graphics_worker("game-render-thread"));
    registry[0] = (struct nx_prof_thread){.handle = 10, .tid = 4, .kind = 'w', .teb = 123};
    wine_nx_thread_configure_cores(0);
    wine_nx_thread_set_name(4, "dxvk-cs");
    assert(!calls && !registry[0].helper);
    wine_nx_thread_configure_cores(1);
    registry[0].fixed = 1;
    wine_nx_thread_set_name(4, "dxvk-cs");
    assert(!calls); registry[0].fixed = 0;
    fail_core = 1;
    wine_nx_thread_set_name(4, "dxvk-cs");
    assert(!registry[0].helper && current_mask == 2 && current_priority == 59);
    fail_core = 0; fail_priority = 1;
    wine_nx_thread_set_name(4, "dxvk-cs");
    assert(!registry[0].helper && current_mask == 2 && current_priority == 59);
    fail_priority = 0;
    wine_nx_thread_set_name(4, "dxvk-cs");
    assert(registry[0].helper && current_mask == 8 && current_priority == 63);
    assert(registry[0].app_mask == 2 && registry[0].app_priority == 59);
    int previous = calls;
    wine_nx_thread_set_name(4, "dxvk-cs");
    assert(calls == previous);
    wine_nx_thread_set_name(4, "ordinary-worker");
    assert(!registry[0].helper && current_mask == 2 && current_priority == 59);
    wine_nx_thread_set_name(4, "dxvk-cs");
    fail_core = 1;
    wine_nx_thread_set_affinity(4, 4);
    assert(registry[0].helper && current_mask == 8 && current_priority == 63);
    fail_core = 0;
    wine_nx_thread_set_affinity(4, 4);
    assert(!registry[0].helper && registry[0].fixed && current_mask == 4 && current_priority == 59);
    assert(followed == 4);
    previous = calls;
    wine_nx_thread_set_name(4, "dxvk-cs");
    wine_nx_thread_set_name(999, "dxvk-cs");
    wine_nx_thread_set_affinity(4, 8);
    assert(calls == previous);
    registry[0].parked = 1;
    wine_nx_thread_register('c', 0, NULL);
    assert(registry[0].helper && !registry[0].fixed && !registry[0].parked);
    assert(!strcmp(registry[0].name, "compositor") && current_mask == 8 && current_priority == 63);
    current_core = 1; current_mask = 2; current_priority = 59;
    wine_nx_thread_register('w', 5, (void *)123);
    assert(!registry[0].helper && !registry[0].name[0] && registry[0].tid == 5);
    assert(current_mask == 2 && current_priority == 59);
    puts("Core affinity: permissions, opt-in, workers, native defaults, rollback, rename, explicit affinity and slot reuse passed");
}
'''
functions = "".join(definition(name) for name in (
    "__wrap_svcSetThreadCoreMask",
    "wine_nx_four_cores_available", "wine_nx_thread_configure_cores", "offload_thread_locked",
    "restore_thread_locked", "wine_nx_thread_set_name", "wine_nx_thread_set_affinity", "wine_nx_thread_register"))
with tempfile.TemporaryDirectory(prefix="wine-nx-cores-") as tmp:
    c, exe = Path(tmp) / "test.c", Path(tmp) / "test"
    c.write_text(fixture + definition("nx_prof_thread", True) + globals_ + functions + test)
    subprocess.run(["clang", "-Wall", "-Wextra", "-Werror", "-pthread", "-fsanitize=address,undefined",
                    "-I" + str(root / "wine-nx-probe/source"), str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=15)
