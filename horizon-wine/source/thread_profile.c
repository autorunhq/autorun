/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 *
 * Where the CPU goes, without verbose traces. Threads register their kernel
 * handles as they start: Wine threads in ntdll's start_thread, the main thread
 * in runtime_start_wow64 and server connection threads in horizon.c. Each
 * [PROGRESS] report then adds a [THREADS] line with every thread's share of a
 * core over the interval, from the kernel's tick count, and the core it is on,
 * and a [SERVER] line with the time spent in round trips to Wine's server.
 *
 * With sdmc:/switch/wine/profile.txt, a sampler also pauses the busiest threads
 * every 2 ms, reads where they are and lets them run again. [PROF] lines split
 * each one's time between translated x86 code (named by module), ARM64 PE
 * modules (wow64.dll's thunks, ntdll.dll), the runtime's own code (Mesa, Wine's
 * unix side, the dynarec's helpers) and system calls (by svc number and the
 * caller of libnx's stub), and name the calls that led to each native or
 * system call sample: in the runtime, and in the x86 code the thread came from.
 * Runtime addresses are offsets into wine-nx-runtime.elf. Samples count whether
 * the thread was running or blocked.
 *
 * While a thread is paused the sampler allocates nothing and takes no lock, so
 * the paused thread cannot hold up its own resumption.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "thread_profile.h"

#define NX_PROF_MAX_THREADS  128
#define WINE_NX_STOP_SUSPEND 1
#define WINE_NX_STOP_END     2
#define WINE_NX_STOP_KERNEL  4
#define WINE_NX_STOP_HELD    8
#define WINE_NX_STOP_LEAVING 16
#define WINE_NX_STOP_GONE    32
#define NX_PROF_TARGETS      4
#define NX_PROF_PERIOD_NS    2000000
#define NX_PROF_MIN_PERMILLE 30  /* sample threads that used at least 3% of a core */
#define NX_PROF_DEPTH        8
#define NX_PROF_LINE         1000
#define NX_PROF_MAX_REQUESTS 1024

#define NX_PROF_BOX64_ESP 14
#define NX_PROF_BOX64_EBP 15

#define NX_PROF_FEX_STATE             28
#define NX_PROF_FEX_ESP               8
#define NX_PROF_FEX_EBP               9
#define NX_PROF_FEX_BLOCK_MAX         0x1000000
#define NX_PROF_FEX_SCAN_MAX          0x40000
#define NX_PROF_FEX_TAIL_SIZE         32
#define NX_PROF_FEX_TAIL_RIP          8
#define NX_PROF_FEX_TAIL_ENTRIES      24
#define NX_PROF_FEX_TAIL_ENTRY_OFFSET 28
#define NX_PROF_FEX_FRAME_SIZE        3776
#define NX_PROF_FEX_FALLBACKS         1640
#define NX_PROF_FEX_FALLBACK_COUNT    47
#define NX_PROF_FEX_LOOP_TOP          2912
#define NX_PROF_FEX_LOOP_TOP_FILL     2920
#define NX_PROF_FEX_LINKER            2944
#define NX_PROF_FEX_SIGILL            2968
#define NX_PROF_FEX_LUDIV             3016
#define NX_PROF_FEX_SIN               3032
#define NX_PROF_FEX_DISPATCHER_SIZE   0x4000
#define NX_PROF_FEX_STORE_HEADER      0xf9000380u

enum nx_prof_fex_part
{
    NX_PROF_FEX_ENTRY,
    NX_PROF_FEX_LOOKUP,
    NX_PROF_FEX_LINK,
    NX_PROF_FEX_DIVIDE,
    NX_PROF_FEX_X87,
    NX_PROF_FEX_FALLBACK,
    NX_PROF_FEX_DISPATCHER,
    NX_PROF_FEX_PARTS
};

#define NX_PROF_JIT_PCS       16384
#define NX_PROF_JIT_BLOCKS    256
#define NX_PROF_JIT_FIRST     20000
#define NX_PROF_JIT_HOST_MAX  0x10000
#define NX_PROF_JIT_GUEST_MAX 0x2000
#define NX_PROF_JIT_FILE      "sdmc:/switch/wine/logs/jit-blocks.txt"

static size_t nx_prof_vl64pair_size( uint8_t type )
{
    if (!(type & 0x80)) return 1;
    if ((type & 0xc0) == 0x80) return 2;
    return type == 0xc0 ? 9 : 17;
}

static size_t nx_prof_vl64pair( const uint8_t *data, uint64_t *arm, uint64_t *rip )
{
    uint8_t type = data[0];
    int32_t arm32, rip32;

    if (!(type & 0x80))
    {
        *arm = (uint64_t)((type & 0xf) + 1) << 2;
        *rip = ((type >> 4) & 7) + 1;
        return 1;
    }
    if ((type & 0xc0) == 0x80)
    {
        *arm = (uint64_t)((int64_t)(int8_t)data[1] * 4);
        *rip = (uint64_t)(int64_t)((int8_t)(uint8_t)(type << 2) >> 2);
        return 2;
    }
    if (type == 0xc0)
    {
        memcpy( &arm32, data + 1, sizeof(arm32) );
        memcpy( &rip32, data + 5, sizeof(rip32) );
        *arm = (uint64_t)(int64_t)arm32;
        *rip = (uint64_t)(int64_t)rip32;
        return 9;
    }
    memcpy( arm, data + 1, sizeof(*arm) );
    memcpy( rip, data + 9, sizeof(*rip) );
    return 17;
}

extern void wine_nx_runtime_trace( const char *msg );
extern int wine_nx_box64_pc_to_x86( uintptr_t pc, uintptr_t *x86 ) __attribute__((weak));
extern int wine_nx_image_at( const void *addr, void **base, char *name, size_t size ) __attribute__((weak));
extern char __end__[];
/* dlls/ntdll/unix/server.c: round trips to the in-process server by request. */
extern unsigned int wine_nx_server_request_count __attribute__((weak));
extern unsigned int wine_nx_server_calls[] __attribute__((weak));
extern unsigned long long wine_nx_server_ticks[] __attribute__((weak));
extern const char *wine_nx_server_names[] __attribute__((weak));

enum nx_shader_affinity
{
    NX_SHADER_AFFINITY_NONE,
    NX_SHADER_AFFINITY_WORKER,
    NX_SHADER_AFFINITY_PIPELINE,
};

struct nx_prof_thread
{
    Handle handle;  /* 0: free */
    unsigned int tid;
    char kind;
    int parked;     /* stopped at a quit point, waiting to hear whether to end */
    s32 core;       /* as the thread saw itself when it registered */
    int fixed;      /* the program chose its cores */
    int helper;
    int migratable;
    char name[32];
    s32 app_core, app_priority;
    u64 app_mask;
    uint64_t teb;
    uint64_t last_ticks, balance_ticks, orchestrator_ticks;
    volatile int stop[2];
    const void * volatile frame;
    const void * volatile changed_contexts;
};

/* horizon.c: a moved Wine thread's server connection thread follows it. */
extern void horizon_follow_thread_cores( void *teb, unsigned int mask ) __attribute__((weak));
static __thread int affinity_fixed;
int wine_nx_balance_enabled = 1;
static int four_cores_enabled;
static u32 reserved_cores;

struct nx_prof_row
{
    Handle handle;
    unsigned int tid, permille;
    char kind;
    s32 core;
    char name[32];
    uint64_t teb;
};

struct nx_prof_target
{
    Handle handle;  /* 0: none */
    unsigned int tid;
    char kind;
    uint64_t teb;
    uint32_t samples, missed;
    uint32_t kinds[NX_PROF_KINDS];
    uint32_t fex_parts[NX_PROF_FEX_PARTS];
    struct nx_prof_table tables[NX_PROF_TABLES];
    struct nx_prof_table stages;
    uint32_t rooted;
};

#define NX_PROF_STAGE_DEPTH  64
#define NX_PROF_STAGE_LEVELS 8
#define NX_PROF_STAGE_SHOWN  3
#define NX_PROF_FRAME_CACHE  256

static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct nx_prof_thread registry[NX_PROF_MAX_THREADS];
/* Held by the sampler for a round of samples, by the report and by threads
 * leaving; registry_mutex is never taken while holding it. */
static pthread_mutex_t profile_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct nx_prof_target targets[NX_PROF_TARGETS];
static int profiling;
/* The runtime's image: __start__ is an absolute 0, so the base comes from the
 * kernel's view of the code mapping. */
static uintptr_t runtime_base, runtime_end;
static struct nx_prof_table jit_blocks;
static struct nx_prof_bucket jit_pcs[NX_PROF_JIT_PCS];
static uint32_t jit_samples, jit_next_dump = NX_PROF_JIT_FIRST;

__thread volatile int *wine_nx_stop_word;
__thread const void *wine_nx_kernel_frame;
static __thread struct nx_prof_thread *self_thread;

extern int horizon_futex_wait( const int *addr, int value, long long timeout_ns );

extern Result __real_svcSetThreadCoreMask( Handle handle, s32 core, u32 mask );

/* libnx gives new pthreads every permitted core. Only graphics opt into core 3. */
Result __wrap_svcSetThreadCoreMask( Handle handle, s32 core, u32 mask )
{
    if (core == 3) core = -1;
    return __real_svcSetThreadCoreMask( handle, core, mask & ~8u );
}

int wine_nx_four_cores_available( void )
{
    u64 cores, priorities;

    return R_SUCCEEDED( svcGetInfo( &cores, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0 ) ) &&
           R_SUCCEEDED( svcGetInfo( &priorities, InfoType_PriorityMask, CUR_PROCESS_HANDLE, 0 ) ) &&
           nx_thread_helper_available( cores, priorities );
}

void wine_nx_thread_configure_cores( int enabled )
{
    four_cores_enabled = enabled && wine_nx_four_cores_available();
    wine_nx_runtime_trace( four_cores_enabled ? "[CORES] graphics workers may use core 3 at priority 63" :
                           enabled ? "[CORES] core 3 unavailable; reinstall the Autorun forwarder and restart" :
                                     "[CORES] application cores 0-2" );
}

static int offload_thread_locked( struct nx_prof_thread *thread )
{
    if (!four_cores_enabled || thread->fixed || thread->helper) return 0;
    if (R_FAILED( svcGetThreadCoreMask( &thread->app_core, &thread->app_mask, thread->handle ) ) ||
        R_FAILED( svcGetThreadPriority( &thread->app_priority, thread->handle ) )) return -1;
    if (R_FAILED( __real_svcSetThreadCoreMask( thread->handle, 3, 8 ) )) return -1;
    if (R_FAILED( svcSetThreadPriority( thread->handle, 63 ) ))
    {
        svcSetThreadCoreMask( thread->handle, thread->app_core, thread->app_mask );
        return -1;
    }
    thread->helper = 1;
    return 1;
}

static int restore_thread_locked( struct nx_prof_thread *thread, s32 core, u32 mask )
{
    if (thread->helper && R_FAILED( svcSetThreadPriority( thread->handle, thread->app_priority ) )) return 0;
    if (R_FAILED( svcSetThreadCoreMask( thread->handle, core, mask ) ))
    {
        if (thread->helper) svcSetThreadPriority( thread->handle, 63 );
        return 0;
    }
    thread->helper = 0;
    thread->migratable = NX_SHADER_AFFINITY_NONE;
    if (thread->teb && &horizon_follow_thread_cores)
        horizon_follow_thread_cores( (void *)(uintptr_t)thread->teb, mask );
    return 1;
}

static int migrate_shader_locked( struct nx_prof_thread *thread )
{
    u64 mask, previous_mask;
    s32 previous_core;

    if (thread->fixed || thread->migratable) return 0;
    if (R_FAILED( svcGetInfo( &mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0 ) ) || !(mask &= 7)) return -1;
    if (mask & ~reserved_cores) mask &= ~reserved_cores;
    if (R_FAILED( svcGetThreadCoreMask( &previous_core, &previous_mask, thread->handle ) ) ||
        R_FAILED( svcSetThreadCoreMask( thread->handle, -1, mask ) )) return -1;
    thread->app_core = previous_core;
    thread->app_mask = previous_mask;
    thread->migratable = NX_SHADER_AFFINITY_WORKER;
    if (thread->teb && &horizon_follow_thread_cores)
        horizon_follow_thread_cores( (void *)(uintptr_t)thread->teb, mask );
    return (int)mask;
}

unsigned int wine_nx_thread_pipeline_begin( void )
{
    Handle handle = threadGetCurHandle();
    unsigned int i, token = 0;

    pthread_mutex_lock( &registry_mutex );
    for (i = 0; i < NX_PROF_MAX_THREADS; i++)
    {
        struct nx_prof_thread *thread = &registry[i];
        int moved;

        if (thread->handle != handle || thread->kind != 'w') continue;
        if (thread->helper) break;
        if ((moved = migrate_shader_locked( thread )) > 0)
        {
            thread->migratable = NX_SHADER_AFFINITY_PIPELINE;
            token = i + 1;
        }
        break;
    }
    pthread_mutex_unlock( &registry_mutex );
    return token;
}

void wine_nx_thread_pipeline_end( unsigned int token )
{
    struct nx_prof_thread *thread;
    int restored = 1;

    if (!token || token > NX_PROF_MAX_THREADS) return;
    pthread_mutex_lock( &registry_mutex );
    thread = &registry[token - 1];
    if (thread->handle == threadGetCurHandle() && thread->migratable == NX_SHADER_AFFINITY_PIPELINE)
        restored = restore_thread_locked( thread, thread->app_core, thread->app_mask );
    pthread_mutex_unlock( &registry_mutex );
    if (!restored) wine_nx_runtime_trace( "[CORES] Vulkan compilation: could not restore affinity" );
}

void wine_nx_thread_set_name( unsigned int tid, const char *name )
{
    char line[128] = "";
    unsigned int i;

    pthread_mutex_lock( &registry_mutex );
    for (i = 0; i < NX_PROF_MAX_THREADS; i++)
    {
        struct nx_prof_thread *thread = &registry[i];
        int moved;

        if (!thread->handle || thread->kind != 'w' || thread->tid != tid) continue;
        if ((thread->helper && !nx_thread_graphics_worker( name )) ||
            (thread->migratable && !nx_thread_shader_worker( name )))
            if (!restore_thread_locked( thread, thread->app_core, thread->app_mask )) break;
        snprintf( thread->name, sizeof(thread->name), "%s", name );
        if (nx_thread_shader_worker( name ))
        {
            if (thread->migratable == NX_SHADER_AFFINITY_PIPELINE)
                thread->migratable = NX_SHADER_AFFINITY_WORKER;
            if ((moved = migrate_shader_locked( thread )) > 0)
                snprintf( line, sizeof(line), "[CORES] %u %s: affinity %#x, priority unchanged", tid, name, moved );
            else if (moved < 0)
                snprintf( line, sizeof(line), "[CORES] %u %s: could not change shader affinity", tid, name );
        }
        else if (nx_thread_graphics_worker( name ))
        {
            if ((moved = offload_thread_locked( thread )))
                snprintf( line, sizeof(line), "[CORES] %u %s: %s", tid, name,
                          moved > 0 ? "core 3, priority 63" : "could not offload graphics worker" );
        }
        break;
    }
    pthread_mutex_unlock( &registry_mutex );
    if (line[0]) wine_nx_runtime_trace( line );
}

void wine_nx_thread_set_affinity( unsigned int tid, unsigned int mask )
{
    unsigned int i;

    mask &= 7;
    if (!mask) return;
    pthread_mutex_lock( &registry_mutex );
    for (i = 0; i < NX_PROF_MAX_THREADS; i++)
    {
        struct nx_prof_thread *thread = &registry[i];

        if (!thread->handle || thread->kind != 'w' || thread->tid != tid) continue;
        if (restore_thread_locked( thread, __builtin_ctz( mask ), mask ))
            thread->fixed = 1;
        break;
    }
    pthread_mutex_unlock( &registry_mutex );
}

static uint64_t thread_ticks( Handle handle )
{
    static u32 info = InfoType_ThreadTickCount;
    u64 ticks;

    if (R_SUCCEEDED( svcGetInfo( &ticks, info, handle, UINT64_MAX ) )) return ticks;
    /* Before 13.0.0 the kernel numbers it differently. */
    if (info == InfoType_ThreadTickCount &&
        R_SUCCEEDED( svcGetInfo( &ticks, InfoType_ThreadTickCountDeprecated, handle, UINT64_MAX ) ))
    {
        info = InfoType_ThreadTickCountDeprecated;
        return ticks;
    }
    return 0;
}

void wine_nx_thread_register( char kind, unsigned int tid, void *teb )
{
    Handle handle = threadGetCurHandle();
    unsigned int i, slot = NX_PROF_MAX_THREADS;
    s32 core = -1;
    u64 mask;

    if (R_FAILED( svcGetThreadCoreMask( &core, &mask, CUR_THREAD_HANDLE ) )) core = -1;
    pthread_mutex_lock( &registry_mutex );
    /* A handle still listed belonged to a thread that ended without saying so. */
    for (i = 0; i < NX_PROF_MAX_THREADS; i++)
    {
        if (registry[i].handle == handle) break;
        if (!registry[i].handle && slot == NX_PROF_MAX_THREADS) slot = i;
    }
    if (i < NX_PROF_MAX_THREADS) slot = i;
    if (slot < NX_PROF_MAX_THREADS)
    {
        memset( &registry[slot], 0, sizeof(registry[slot]) );
        registry[slot].handle = handle;
        registry[slot].tid = tid;
        registry[slot].kind = kind;
        registry[slot].core = core;
        registry[slot].teb = (uintptr_t)teb;
        registry[slot].fixed = affinity_fixed;
        registry[slot].last_ticks = registry[slot].balance_ticks = registry[slot].orchestrator_ticks =
            thread_ticks( handle );
        wine_nx_stop_word = registry[slot].stop;
        self_thread = &registry[slot];
        if (kind == 'c')
        {
            snprintf( registry[slot].name, sizeof(registry[slot].name), "compositor" );
            offload_thread_locked( &registry[slot] );
        }
    }
    pthread_mutex_unlock( &registry_mutex );
}

unsigned int wine_nx_threads_wake( void )
{
    Handle self = threadGetCurHandle();
    unsigned int i, woken = 0;

    /* No lock: a thread ending while this runs leaves a handle that is simply
     * refused, and waiting on a thread that is being told to end would not do. */
    for (i = 0; i < NX_PROF_MAX_THREADS; i++)
    {
        Handle handle = registry[i].handle;

        if (!handle || handle == self) continue;
        if (R_SUCCEEDED( svcCancelSynchronization( handle ) )) woken++;
    }
    return woken;
}

/* A thread says it has stopped where it can be ended, or has gone back to work. */
void wine_nx_thread_parked( int parked )
{
    Handle handle = threadGetCurHandle();
    unsigned int i;

    for (i = 0; i < NX_PROF_MAX_THREADS; i++)
        if (registry[i].handle == handle) registry[i].parked = parked;
}

unsigned int wine_nx_threads_program( void )
{
    Handle self = threadGetCurHandle();
    unsigned int i, count = 0;

    /* The presenter is the runtime's own and is stopped separately; everything
     * else belongs to the program that is being closed. */
    for (i = 0; i < NX_PROF_MAX_THREADS; i++)
        if (registry[i].handle && registry[i].handle != self && registry[i].kind != 'c') count++;
    return count;
}

unsigned int wine_nx_threads_other( void )
{
    Handle self = threadGetCurHandle();
    unsigned int i, count = 0;

    for (i = 0; i < NX_PROF_MAX_THREADS; i++)
        if (registry[i].handle && registry[i].handle != self) count++;
    return count;
}

void wine_nx_thread_unregister( void )
{
    Handle handle = threadGetCurHandle();
    unsigned int i;

    pthread_mutex_lock( &registry_mutex );
    for (i = 0; i < NX_PROF_MAX_THREADS; i++)
        if (registry[i].handle == handle)
        {
            __atomic_fetch_or( &registry[i].stop[0], WINE_NX_STOP_GONE, __ATOMIC_SEQ_CST );
            svcSignalToAddress( (void *)&registry[i].stop[0], SignalType_Signal, 0, -1 );
            registry[i].handle = 0;
        }
    wine_nx_stop_word = NULL;
    self_thread = NULL;
    pthread_mutex_unlock( &registry_mutex );
    pthread_mutex_lock( &profile_mutex );
    for (i = 0; i < NX_PROF_TARGETS; i++)
        if (targets[i].handle == handle) targets[i].handle = 0;
    pthread_mutex_unlock( &profile_mutex );
}

static int in_runtime( uint64_t address )
{
    return address - runtime_base < runtime_end - runtime_base;
}

int wine_nx_thread_stop_request( const void *teb, unsigned int tid, int set, int clear, int code, Handle *handle )
{
    unsigned int i;
    int found = 0;

    if (!teb) return 0;
    pthread_mutex_lock( &registry_mutex );
    for (i = 0; i < NX_PROF_MAX_THREADS && !found; i++)
    {
        struct nx_prof_thread *thread = &registry[i];

        if (!thread->handle || thread->teb != (uintptr_t)teb || thread->tid != tid) continue;
        if (set & WINE_NX_STOP_END) thread->stop[1] = code;
        if (set) __atomic_fetch_or( &thread->stop[0], set, __ATOMIC_SEQ_CST );
        if (clear) __atomic_fetch_and( &thread->stop[0], ~clear, __ATOMIC_SEQ_CST );
        svcSignalToAddress( (void *)&thread->stop[0], SignalType_Signal, 0, -1 );
        if (handle) *handle = thread->handle;
        found = 1;
    }
    pthread_mutex_unlock( &registry_mutex );
    return found;
}

static int stop_state( struct nx_prof_thread *thread )
{
    return __atomic_load_n( &thread->stop[0], __ATOMIC_SEQ_CST );
}

static void stop_signal( struct nx_prof_thread *thread )
{
    svcSignalToAddress( (void *)&thread->stop[0], SignalType_Signal, 0, -1 );
}

static void wait_while_held( struct nx_prof_thread *thread )
{
    int state;

    while (((state = stop_state( thread )) & WINE_NX_STOP_HELD) && !(state & WINE_NX_STOP_END))
        horizon_futex_wait( (const int *)&thread->stop[0], state, -1 );
}

static const void *take_changed( struct nx_prof_thread *thread )
{
    if (!__atomic_load_n( &thread->changed_contexts, __ATOMIC_ACQUIRE )) return NULL;
    return __atomic_exchange_n( &thread->changed_contexts, NULL, __ATOMIC_SEQ_CST );
}

const void *wine_nx_kernel_enter( const void *frame )
{
    struct nx_prof_thread *thread = self_thread;
    int state, next;

    if (!thread) return NULL;
    if (!frame)
    {
        __atomic_fetch_and( &thread->stop[0], ~WINE_NX_STOP_LEAVING, __ATOMIC_SEQ_CST );
        return NULL;
    }
    __atomic_store_n( &thread->frame, frame, __ATOMIC_SEQ_CST );
    state = stop_state( thread );
    do
    {
        next = (state | WINE_NX_STOP_KERNEL) & ~WINE_NX_STOP_LEAVING;
        if ((state & WINE_NX_STOP_SUSPEND) && !(state & WINE_NX_STOP_END)) next |= WINE_NX_STOP_HELD;
    } while (!__atomic_compare_exchange_n( &thread->stop[0], &state, next, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ));
    if (!(next & WINE_NX_STOP_HELD)) return NULL;
    stop_signal( thread );
    wait_while_held( thread );
    return take_changed( thread );
}

const void *wine_nx_kernel_leave( void )
{
    struct nx_prof_thread *thread = self_thread;
    int state, next;

    if (!thread) return NULL;
    state = stop_state( thread );
    for (;;)
    {
        if ((state & WINE_NX_STOP_HELD) && !(state & WINE_NX_STOP_END))
        {
            wait_while_held( thread );
            state = stop_state( thread );
            continue;
        }
        if ((state & WINE_NX_STOP_SUSPEND) && !(state & WINE_NX_STOP_END)) next = state | WINE_NX_STOP_HELD;
        else next = (state & ~(WINE_NX_STOP_KERNEL | WINE_NX_STOP_HELD)) | WINE_NX_STOP_LEAVING;
        if (!__atomic_compare_exchange_n( &thread->stop[0], &state, next, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ))
            continue;
        if (!(next & WINE_NX_STOP_HELD)) break;
        stop_signal( thread );
        state = next;
    }
    return take_changed( thread );
}

int wine_nx_suspend_requested( void )
{
    return self_thread && (stop_state( self_thread ) & WINE_NX_STOP_SUSPEND);
}

static struct nx_prof_thread *registered_locked( const void *teb, unsigned int tid )
{
    unsigned int i;

    for (i = 0; i < NX_PROF_MAX_THREADS; i++)
        if (registry[i].handle && registry[i].teb == (uintptr_t)teb && registry[i].tid == tid) return &registry[i];
    return NULL;
}

int wine_nx_thread_state( const void *teb, unsigned int tid, const void **frame )
{
    struct nx_prof_thread *thread;
    int state = -1;

    if (!teb) return -1;
    pthread_mutex_lock( &registry_mutex );
    if ((thread = registered_locked( teb, tid )))
    {
        state = stop_state( thread );
        if (frame) *frame = thread->frame;
    }
    pthread_mutex_unlock( &registry_mutex );
    return state;
}

int wine_nx_thread_hold( const void *teb, unsigned int tid, const void **frame, int *changed )
{
    struct nx_prof_thread *thread;
    int state, held = 0;

    if (!teb) return 0;
    pthread_mutex_lock( &registry_mutex );
    if ((thread = registered_locked( teb, tid )))
    {
        state = stop_state( thread );
        while ((state & WINE_NX_STOP_SUSPEND) && !(state & WINE_NX_STOP_END))
        {
            if (state & WINE_NX_STOP_HELD) held = 1;
            else if (!(state & WINE_NX_STOP_KERNEL)) break;
            else if (__atomic_compare_exchange_n( &thread->stop[0], &state, state | WINE_NX_STOP_HELD, 0,
                                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ))
            {
                stop_signal( thread );
                held = 1;
            }
            if (held) break;
        }
        if (held)
        {
            *frame = thread->frame;
            *changed = __atomic_load_n( &thread->changed_contexts, __ATOMIC_SEQ_CST ) != NULL;
        }
    }
    pthread_mutex_unlock( &registry_mutex );
    return held;
}

void wine_nx_thread_signal( const void *teb, unsigned int tid )
{
    struct nx_prof_thread *thread;

    if (!teb) return;
    pthread_mutex_lock( &registry_mutex );
    if ((thread = registered_locked( teb, tid )))
    {
        __atomic_fetch_or( &thread->stop[0], WINE_NX_STOP_HELD, __ATOMIC_SEQ_CST );
        stop_signal( thread );
    }
    pthread_mutex_unlock( &registry_mutex );
}

void wine_nx_thread_release( const void *teb, unsigned int tid, const void *changed )
{
    struct nx_prof_thread *thread;

    if (!teb) return;
    pthread_mutex_lock( &registry_mutex );
    if ((thread = registered_locked( teb, tid )))
    {
        if (changed) __atomic_store_n( &thread->changed_contexts, changed, __ATOMIC_SEQ_CST );
        __atomic_fetch_and( &thread->stop[0], ~WINE_NX_STOP_HELD, __ATOMIC_SEQ_CST );
        stop_signal( thread );
    }
    pthread_mutex_unlock( &registry_mutex );
}

void wine_nx_thread_wait_change( const void *teb, unsigned int tid, int observed )
{
    struct nx_prof_thread *thread;

    if (!teb) return;
    pthread_mutex_lock( &registry_mutex );
    thread = registered_locked( teb, tid );
    pthread_mutex_unlock( &registry_mutex );
    if (thread) horizon_futex_wait( (const int *)&thread->stop[0], observed, 20000000LL );
}

int wine_nx_stop_state( int *code )
{
    volatile int *word = wine_nx_stop_word;

    if (!word) return 0;
    if (code) *code = word[1];
    return __atomic_load_n( word, __ATOMIC_ACQUIRE );
}

void wine_nx_stop_sleep( s64 ns )
{
    volatile int *word = wine_nx_stop_word;
    int value;

    if (!word) svcSleepThread( ns );
    else if (!((value = __atomic_load_n( word, __ATOMIC_ACQUIRE )) & WINE_NX_STOP_END))
        svcWaitForAddress( (void *)word, ArbitrationType_WaitIfEqual, value, ns );
}

int wine_nx_in_runtime_code( uint64_t address )
{
    MemoryInfo info;
    u32 page;

    if (!runtime_end)
    {
        if (R_FAILED( svcQueryMemory( &info, &page, (u64)(uintptr_t)&wine_nx_in_runtime_code ) )) return 1;
        runtime_base = info.addr;
        runtime_end = (uintptr_t)__end__;
    }
    return in_runtime( address );
}

int wine_nx_thread_pause_granted( void )
{
    return envIsSyscallHinted( 0x32 ) && envIsSyscallHinted( 0x33 );
}

static uint64_t read_word( uint64_t address )
{
    return *(const uint64_t *)(uintptr_t)address;
}

static uint32_t read_word32( uint64_t address )
{
    return *(const uint32_t *)(uintptr_t)address;
}

static int readable_size( uint64_t address, uint64_t size, MemoryInfo *info )
{
    uint64_t offset;
    u32 page;

    if (!size || R_FAILED( svcQueryMemory( info, &page, address ) ) || !(info->perm & Perm_R) ||
        info->type == MemType_Unmapped || address < info->addr) return 0;
    offset = address - info->addr;
    return offset <= info->size && size <= info->size - offset;
}

static int readable( uint64_t address, MemoryInfo *info )
{
    return readable_size( address, 1, info );
}

/* Both walks run while the thread is paused: memory reads and system calls. */
static unsigned int walk_callers( const ThreadContext *ctx, uint64_t *callers )
{
    MemoryInfo info;

    if (!readable( ctx->sp, &info )) return 0;
    return nx_prof_callers( ctx->lr, ctx->fp, ctx->sp > info.addr ? ctx->sp : info.addr, info.addr + info.size,
                            read_word, callers, NX_PROF_DEPTH );
}

#define CARAFE_SCAN_WORDS 512

static int image_at( uint64_t address, void **base, char *name, size_t size );

static unsigned int copy_stack( const ThreadContext *ctx, uint64_t *words )
{
    MemoryInfo info;
    uint64_t at, end;
    unsigned int count = 0;

    if (!readable( ctx->sp, &info )) return 0;
    end = info.addr + info.size;
    for (at = ctx->sp & ~7ull; at + 8 <= end && count < CARAFE_SCAN_WORDS; at += 8) words[count++] = read_word( at );
    return count;
}

static int in_code( uint64_t address, MemoryInfo *last )
{
    u32 page;

    if (address >= last->addr && address - last->addr < last->size) return (last->perm & Perm_X) != 0;
    if (R_FAILED( svcQueryMemory( last, &page, address ) ) || address < last->addr)
    {
        last->addr = last->size = 0;
        return 0;
    }
    return (last->perm & Perm_X) != 0;
}

static int after_call( uint64_t address, MemoryInfo *last )
{
    uint32_t insn;
    char name[8];
    void *base;

    if ((address & 3) || address < 0x10000 || !in_code( address - 4, last )) return 0;
    insn = read_word32( address - 4 );
    if ((insn & 0xfc000000) != 0x94000000 && (insn & 0xfffffc1f) != 0xd63f0000) return 0;
    return image_at( address, &base, name, sizeof(name) );
}

static unsigned int pick_calls( const uint64_t *words, unsigned int count, uint64_t *callers )
{
    MemoryInfo last = { 0 };
    unsigned int i, picked = 0;

    for (i = 0; i < count && picked < NX_PROF_DEPTH; i++)
        if (after_call( words[i], &last ) && (!picked || callers[picked - 1] != words[i])) callers[picked++] = words[i];
    return picked;
}

static int fex_x86_registers( uint64_t teb, uint32_t *eip, uint32_t *esp, uint32_t *ebp )
{
    MemoryInfo info;
    uint64_t thread, frame;
    uint32_t control;

    if (!readable_size( teb + NX_PROF_TEB_FEX_THREAD, 2 * sizeof(uint64_t), &info )) return 0;
    thread = read_word( teb + NX_PROF_TEB_FEX_THREAD );
    control = read_word32( teb + NX_PROF_TEB_FEX_CONTROL );
    if (!thread || (control & (NX_PROF_FEX_IN_JIT | NX_PROF_FEX_AREA_DIRTY))) return 0;
    if (!readable_size( thread, sizeof(uint64_t), &info )) return 0;
    frame = read_word( thread );
    if (frame <= thread || frame >= thread + NX_PROF_FEX_THREAD_SPAN) return 0;
    if (!readable_size( frame, NX_PROF_FEX_GREGS + (NX_PROF_FEX_REG_EBP + 1) * sizeof(uint64_t), &info )) return 0;
    *eip = (uint32_t)read_word( frame + NX_PROF_FEX_RIP );
    *esp = (uint32_t)read_word( frame + NX_PROF_FEX_GREGS + NX_PROF_FEX_REG_ESP * sizeof(uint64_t) );
    *ebp = (uint32_t)read_word( frame + NX_PROF_FEX_GREGS + NX_PROF_FEX_REG_EBP * sizeof(uint64_t) );
    return read_word32( teb + NX_PROF_TEB_FEX_CONTROL ) == control;
}

static int x86_registers( uint64_t teb, uint32_t *eip, uint32_t *esp, uint32_t *ebp )
{
    MemoryInfo info;
    uint64_t cpu, context;

    if (!teb || !readable_size( teb + NX_PROF_TEB_CPU_AREA, sizeof(uint64_t), &info )) return 0;
    if (!(cpu = read_word( teb + NX_PROF_TEB_CPU_AREA ))) return 0;
    context = cpu + NX_PROF_CPU_CONTEXT;
    if (!readable_size( cpu, NX_PROF_CPU_CONTEXT + NX_PROF_I386_ESP + sizeof(uint32_t), &info )) return 0;
    if (*(const uint16_t *)(uintptr_t)(cpu + NX_PROF_CPU_MACHINE) != NX_PROF_MACHINE_I386) return 0;
    if (fex_x86_registers( teb, eip, esp, ebp )) return 1;
    *eip = 0;
    *esp = read_word32( context + NX_PROF_I386_ESP );
    *ebp = read_word32( context + NX_PROF_I386_EBP );
    return 1;
}

static unsigned int walk_x86( uint64_t teb, uint32_t *callers )
{
    MemoryInfo info;
    uint32_t eip, esp, ebp;

    if (!x86_registers( teb, &eip, &esp, &ebp ) || !readable( esp, &info )) return 0;
    return nx_prof_x86_callers( esp, ebp, info.addr, info.addr + info.size, read_word32, callers, NX_PROF_DEPTH );
}

/* Inside translated code, the x86 frame chain from the guest's EBP and ESP. */
static unsigned int walk_x86_frames( const ThreadContext *ctx, unsigned int esp_reg, unsigned int ebp_reg,
                                     uint32_t *callers )
{
    uint32_t esp = (uint32_t)ctx->cpu_gprs[esp_reg].x, ebp = (uint32_t)ctx->cpu_gprs[ebp_reg].x;
    MemoryInfo info;

    if (!readable( esp, &info )) return 0;
    return nx_prof_x86_frames( esp, ebp, info.addr, info.addr + info.size, read_word32, callers, NX_PROF_DEPTH );
}

static unsigned int walk_x86_root( uint32_t esp, uint32_t ebp, uint32_t *chain, int *rooted )
{
    MemoryInfo info;
    uint64_t lo, hi;
    unsigned int n = 0;

    *rooted = 0;
    if (!readable( esp, &info )) return 0;
    lo = info.addr;
    hi = info.addr + info.size;
    while (n < NX_PROF_STAGE_DEPTH && ebp >= lo && ebp >= esp && (uint64_t)ebp + 8 <= hi && !(ebp & 3))
    {
        uint32_t next = read_word32( ebp ), ret = read_word32( (uint64_t)ebp + 4 );

        if (ret) chain[n++] = ret;
        if (!next || (uint64_t)next + 8 > hi)
        {
            *rooted = 1;
            break;
        }
        if (next <= ebp) break;
        ebp = next;
    }
    return n;
}

static unsigned int stage_chain( const struct nx_prof_target *target, const ThreadContext *ctx, int translated,
                                 int fex, uint32_t *chain )
{
    uint32_t eip, esp, ebp;
    unsigned int n;
    int rooted;

    if (fex)
    {
        esp = (uint32_t)ctx->cpu_gprs[NX_PROF_FEX_ESP].x;
        ebp = (uint32_t)ctx->cpu_gprs[NX_PROF_FEX_EBP].x;
    }
    else if (translated)
    {
        esp = (uint32_t)ctx->cpu_gprs[NX_PROF_BOX64_ESP].x;
        ebp = (uint32_t)ctx->cpu_gprs[NX_PROF_BOX64_EBP].x;
    }
    else if (!x86_registers( target->teb, &eip, &esp, &ebp )) return 0;
    n = walk_x86_root( esp, ebp, chain, &rooted );
    return rooted ? n : 0;
}

static int game_frame( uint32_t address )
{
    static struct { uint32_t address; int game; } seen[NX_PROF_FRAME_CACHE];
    unsigned int slot = (address >> 2) % NX_PROF_FRAME_CACHE;
    char module[64];
    void *base;

    if (seen[slot].address != address)
    {
        seen[slot].game = image_at( address, &base, module, sizeof(module) ) && !module[0];
        seen[slot].address = address;
    }
    return seen[slot].game;
}

static unsigned int game_anchor( const uint32_t *chain, unsigned int n )
{
    unsigned int anchor;

    for (anchor = n; anchor > 0; anchor--)
        if (game_frame( chain[anchor - 1] )) break;
    return anchor;
}

static void add_stages( struct nx_prof_target *target, const uint32_t *chain, unsigned int anchor )
{
    unsigned int level;

    target->rooted++;
    for (level = 1; level <= NX_PROF_STAGE_LEVELS && level <= anchor; level++)
        nx_prof_add( &target->stages, ((uint64_t)level << 32) | chain[anchor - level] );
}

static int fex_block_pc_to_x86( uint64_t block, uint64_t pc, uintptr_t *x86 )
{
    uint64_t tail, host, guest, entry, end;
    uint32_t entries, i;
    MemoryInfo info;

    if (pc < block || pc - block >= NX_PROF_FEX_BLOCK_MAX || !readable_size( block, sizeof(uint32_t), &info ))
        return 0;
    tail = block + read_word32( block );
    if (!readable_size( tail, NX_PROF_FEX_TAIL_SIZE, &info ) || pc - block >= read_word( tail )) return 0;
    host = block;
    guest = read_word( tail + NX_PROF_FEX_TAIL_RIP );
    entries = read_word32( tail + NX_PROF_FEX_TAIL_ENTRIES );
    entry = tail + read_word32( tail + NX_PROF_FEX_TAIL_ENTRY_OFFSET );
    end = entry;
    for (i = 0; i < entries; i++)
    {
        uint64_t arm, rip;
        size_t size;

        if (entry >= end)
        {
            if (!readable( entry, &info )) return 0;
            end = info.addr + info.size;
        }
        size = nx_prof_vl64pair_size( *(const uint8_t *)(uintptr_t)entry );
        if (end - entry < size) return 0;
        nx_prof_vl64pair( (const uint8_t *)(uintptr_t)entry, &arm, &rip );
        if (pc < host + arm) break;
        host += arm;
        guest += rip;
        entry += size;
    }
    *x86 = guest;
    return 1;
}

static void jit_pc_add( uint64_t pc )
{
    unsigned int start = (unsigned int)((pc * 0x9e3779b97f4a7c15ull) >> 50), i;

    for (i = 0; i < NX_PROF_PROBES; i++)
    {
        struct nx_prof_bucket *bucket = &jit_pcs[(start + i) & (NX_PROF_JIT_PCS - 1)];

        if (bucket->count && bucket->key != pc) continue;
        bucket->key = pc;
        bucket->count++;
        return;
    }
}

static int fex_pc_to_x86( const ThreadContext *ctx, uintptr_t *x86, uint64_t *start )
{
    uint64_t pc = ctx->pc.x, state = ctx->cpu_gprs[NX_PROF_FEX_STATE].x;
    MemoryInfo info;

    if (in_runtime( pc ) || !readable_size( state, sizeof(uint64_t), &info )) return 0;
    *start = read_word( state );
    return fex_block_pc_to_x86( *start, pc, x86 );
}

static int fex_entry_to_x86( uint64_t pc, uintptr_t *x86, uint64_t *start )
{
    uint64_t at = pc;
    uint32_t word;
    int64_t offset;
    MemoryInfo info;

    if (in_runtime( pc ) || !readable_size( pc, sizeof(uint32_t), &info )) return 0;
    word = read_word32( pc );
    if (word == NX_PROF_FEX_STORE_HEADER)
    {
        if (pc - 4 < info.addr) return 0;
        at = pc - 4;
        word = read_word32( at );
    }
    if ((word & 0x9f00001fu) != 0x10000000u) return 0;
    offset = (int64_t)((((word >> 5) & 0x7ffff) << 2) | ((word >> 29) & 3));
    if (offset & 0x100000) offset -= 0x200000;
    if (offset >= 0) return 0;
    *start = at + offset;
    return fex_block_pc_to_x86( *start, pc, x86 );
}

static int fex_scan_to_x86( uint64_t pc, uintptr_t *x86, uint64_t *start )
{
    uint64_t at, low;
    uint32_t word;
    int64_t offset;
    MemoryInfo info;

    if (in_runtime( pc ) || !readable_size( pc, sizeof(uint32_t), &info )) return 0;
    low = pc - info.addr > NX_PROF_FEX_SCAN_MAX ? pc - NX_PROF_FEX_SCAN_MAX : info.addr;
    for (at = (pc & ~3ull) - 4; at >= low && at + 8 <= info.addr + info.size; at -= 4)
    {
        if (read_word32( at + 4 ) != NX_PROF_FEX_STORE_HEADER) continue;
        word = read_word32( at );
        if ((word & 0x9f00001fu) != 0x10000000u) continue;
        offset = (int64_t)((((word >> 5) & 0x7ffff) << 2) | ((word >> 29) & 3));
        if (offset & 0x100000) offset -= 0x200000;
        if (offset >= 0) continue;
        if (fex_block_pc_to_x86( at + offset, pc, x86 ))
        {
            *start = at + offset;
            return 1;
        }
    }
    return 0;
}

static int fex_dispatcher_part( const ThreadContext *ctx )
{
    uint64_t pc = ctx->pc.x, state = ctx->cpu_gprs[NX_PROF_FEX_STATE].x, fill, fallback = ~0ull, handler;
    MemoryInfo info;
    unsigned int i;

    if (in_runtime( pc ) || !readable_size( state, NX_PROF_FEX_FRAME_SIZE, &info )) return -1;
    fill = read_word( state + NX_PROF_FEX_LOOP_TOP_FILL );
    if (!fill || pc + 0x100 < fill || pc + 0x100 - fill >= NX_PROF_FEX_DISPATCHER_SIZE) return -1;
    for (i = 0; i < NX_PROF_FEX_FALLBACK_COUNT; i++)
    {
        handler = read_word( state + NX_PROF_FEX_FALLBACKS + i * 16 );
        if (handler > fill && handler < fallback) fallback = handler;
    }
    if (pc >= fallback) return NX_PROF_FEX_FALLBACK;
    if (pc >= read_word( state + NX_PROF_FEX_SIN )) return NX_PROF_FEX_X87;
    if (pc >= read_word( state + NX_PROF_FEX_LUDIV )) return NX_PROF_FEX_DIVIDE;
    if (pc >= read_word( state + NX_PROF_FEX_LINKER ) && pc < read_word( state + NX_PROF_FEX_SIGILL ))
        return NX_PROF_FEX_LINK;
    if (pc >= read_word( state + NX_PROF_FEX_LOOP_TOP ) && pc < read_word( state + NX_PROF_FEX_LINKER ))
        return NX_PROF_FEX_LOOKUP;
    return NX_PROF_FEX_DISPATCHER;
}

static void record( struct nx_prof_target *target, const ThreadContext *ctx, int translated, uintptr_t x86,
                    const uint64_t *callers, unsigned int count, const uint32_t *x86_callers, unsigned int x86_count )
{
    uintptr_t pc = ctx->pc.x;
    uint32_t sites[2] = { NX_PROF_NO_SITE, NX_PROF_NO_SITE };
    int native = in_runtime( pc ), svc = -1;
    unsigned int i, n = 0;
    enum nx_prof_kind kind;
    uint64_t key;

    if (!translated && native && pc - runtime_base >= 4)
        svc = nx_prof_svc_at( *(const uint32_t *)pc, *(const uint32_t *)(pc - 4) );
    kind = nx_prof_classify( translated, native, svc );
    if (kind == NX_PROF_X86) key = x86 & ~0x1full;
    else if (kind == NX_PROF_IMAGE) key = pc & ~0xffull;
    else if (kind == NX_PROF_SVC) key = nx_prof_svc_key( svc, ctx->lr - runtime_base );
    else key = (pc - runtime_base) & ~0x1full;
    target->samples++;
    target->kinds[kind]++;
    nx_prof_add( &target->tables[kind], key );
    if (kind == NX_PROF_X86)
    {
        /* Who called the function running: which lock a spin loop is on, say. */
        if (x86_count)
            nx_prof_add( &target->tables[NX_PROF_X86_FRAMES],
                         nx_prof_pair_key( x86_callers[0], x86_count > 1 ? x86_callers[1] : NX_PROF_NO_SITE ) );
        return;
    }

    /* Outside translated code the x86 context is the one exported at the last
     * gate: at a system call or unix call, the x86 code that made it. */
    if (x86_count > 1)
        nx_prof_add( &target->tables[NX_PROF_X86_CALLERS],
                     nx_prof_pair_key( x86_callers[1], x86_count > 2 ? x86_callers[2] : NX_PROF_NO_SITE ) );
    if (kind == NX_PROF_IMAGE) return;

    /* The two calls outside the sample's own function: a system call's key
     * already names the stub's caller. */
    for (i = kind == NX_PROF_SVC && count && callers[0] == ctx->lr ? 1 : 0; i < count && n < 2; i++)
    {
        if (!in_runtime( callers[i] )) break;
        sites[n++] = (uint32_t)(callers[i] - runtime_base);
    }
    nx_prof_add( &target->tables[NX_PROF_CHAIN], nx_prof_pair_key( sites[0], sites[1] ) );
}

extern volatile int wine_nx_quit_requested __attribute__((weak));

static void sampler( void *arg )
{
    uint64_t callers[NX_PROF_DEPTH];
    uint32_t x86_callers[NX_PROF_DEPTH];
    uint32_t stage_chains[NX_PROF_TARGETS][NX_PROF_STAGE_DEPTH];
    unsigned int stage_counts[NX_PROF_TARGETS], stage_anchors[NX_PROF_TARGETS];
    Handle stage_handles[NX_PROF_TARGETS];
    unsigned int i, tries, count, x86_count;
    uintptr_t x86 = 0;
    uint64_t block = 0;
    int translated, fex, part;

    (void)arg;
    for (;;)
    {
        svcSleepThread( NX_PROF_PERIOD_NS );
        /* It reads other threads' stacks and contexts, so it must stop before
         * they and the memory they ran in are taken away. */
        if (&wine_nx_quit_requested && wine_nx_quit_requested) break;
        memset( stage_counts, 0, sizeof(stage_counts) );
        pthread_mutex_lock( &profile_mutex );
        for (i = 0; i < NX_PROF_TARGETS; i++)
        {
            struct nx_prof_target *target = &targets[i];
            ThreadContext ctx;
            Result rc;

            stage_handles[i] = 0;
            if (!target->handle) continue;
            if (R_FAILED( svcSetThreadActivity( target->handle, ThreadActivity_Paused ) ))
            {
                target->missed++;
                continue;
            }
            /* The pause takes hold once the thread is off its core. */
            for (tries = 0; R_FAILED( rc = svcGetThreadContext3( &ctx, target->handle ) ) && tries < 4; tries++)
                svcSleepThread( 0 );
            count = x86_count = 0;
            translated = fex = 0;
            if (R_SUCCEEDED( rc ))
            {
                count = walk_callers( &ctx, callers );
                if (&wine_nx_box64_pc_to_x86) translated = wine_nx_box64_pc_to_x86( ctx.pc.x, &x86 );
                if (!translated) translated = fex = fex_pc_to_x86( &ctx, &x86, &block );
                part = -1;
                if (!translated && (translated = fex = fex_entry_to_x86( ctx.pc.x, &x86, &block ))) part = NX_PROF_FEX_ENTRY;
                if (!translated) translated = fex = fex_scan_to_x86( ctx.pc.x, &x86, &block );
                if (!translated) part = fex_dispatcher_part( &ctx );
                if (part >= 0) target->fex_parts[part]++;
                x86_count = fex ? walk_x86_frames( &ctx, NX_PROF_FEX_ESP, NX_PROF_FEX_EBP, x86_callers ) :
                            translated ? walk_x86_frames( &ctx, NX_PROF_BOX64_ESP, NX_PROF_BOX64_EBP, x86_callers ) :
                                         walk_x86( target->teb, x86_callers );
                stage_counts[i] = stage_chain( target, &ctx, translated, fex, stage_chains[i] );
            }
            svcSetThreadActivity( target->handle, ThreadActivity_Runnable );
            stage_handles[i] = target->handle;
            if (R_SUCCEEDED( rc )) record( target, &ctx, translated, x86, callers, count, x86_callers, x86_count );
            else target->missed++;
            if (R_SUCCEEDED( rc ) && fex)
            {
                nx_prof_add( &jit_blocks, block );
                jit_pc_add( ctx.pc.x & ~3ull );
                jit_samples++;
            }
        }
        pthread_mutex_unlock( &profile_mutex );
        for (i = 0; i < NX_PROF_TARGETS; i++)
            stage_anchors[i] = stage_counts[i] ? game_anchor( stage_chains[i], stage_counts[i] ) : 0;
        pthread_mutex_lock( &profile_mutex );
        for (i = 0; i < NX_PROF_TARGETS; i++)
            if (stage_anchors[i] && targets[i].handle == stage_handles[i])
                add_stages( &targets[i], stage_chains[i], stage_anchors[i] );
        pthread_mutex_unlock( &profile_mutex );
    }
}

/* The sampler's own thread, waited for and closed by wine_nx_profile_stop.
 * libnx keeps its threads in a list through this structure, so it stays where
 * it was created and is never copied. */
static Thread sampler_thread;

void wine_nx_profile_start( void )
{
    Thread *thread = &sampler_thread;
    MemoryInfo info;
    char line[200];
    u32 page;
    Result rc;

    if (__atomic_load_n( &profiling, __ATOMIC_RELAXED )) return;
    /* A system call the process is not granted ends it. */
    if (!envIsSyscallHinted( 0x32 ) || !envIsSyscallHinted( 0x33 ))
    {
        wine_nx_runtime_trace( "[PROF] sampler not started: svcSetThreadActivity or svcGetThreadContext3 is not granted" );
        return;
    }
    if (R_FAILED( rc = svcQueryMemory( &info, &page, (u64)(uintptr_t)&wine_nx_profile_start ) ))
    {
        snprintf( line, sizeof(line), "[PROF] sampler not started: svcQueryMemory failed (rc=%#x)", (unsigned int)rc );
        wine_nx_runtime_trace( line );
        return;
    }
    runtime_base = info.addr;
    runtime_end = (uintptr_t)__end__;
    /* Above every program thread (priority 59) and the audio feeder (56). */
    rc = threadCreate( thread, sampler, NULL, NULL, 0x4000, 0x24, 2 );
    if (R_FAILED( rc )) rc = threadCreate( thread, sampler, NULL, NULL, 0x4000, 0x24, -2 );
    if (R_SUCCEEDED( rc ) && R_FAILED( rc = threadStart( thread ) )) threadClose( thread );
    profiling = R_SUCCEEDED( rc );
    snprintf( line, sizeof(line), "[PROF] sampler %s (rc=%#x): every %u ms, the %u busiest threads; runtime %#lx-%#lx",
              profiling ? "started" : "not started", (unsigned int)rc, NX_PROF_PERIOD_NS / 1000000, NX_PROF_TARGETS,
              (unsigned long)runtime_base, (unsigned long)runtime_end );
    wine_nx_runtime_trace( line );
}

/* The sampler leaves its loop as soon as a quit is asked for, but its stack is
 * heap the kernel lent it, and only threadClose gives that back. Called on the
 * way out, after the threads it was sampling have ended. */
void wine_nx_profile_stop( void )
{
    if (!profiling) return;
    profiling = 0;
    /* Bounded, and closed only once it really has exited: threadClose takes the
     * stack away from underneath a thread that is still running on it. */
    if (R_FAILED( waitSingle( waiterForThread( &sampler_thread ), 3000000000ULL ) ))
    {
        wine_nx_runtime_trace( "[PROF] the sampler did not end; its stack stays lent out" );
        return;
    }
    threadClose( &sampler_thread );
    wine_nx_runtime_trace( "[PROF] sampler ended" );
}

static int appendf( char *line, int len, const char *fmt, ... )
{
    va_list args;
    int ret;

    if (len < 0 || len >= NX_PROF_LINE) return len;
    va_start( args, fmt );
    ret = vsnprintf( line + len, NX_PROF_LINE - len, fmt, args );
    va_end( args );
    return ret < 0 ? len : len + ret;
}

/* Tenths of a percent. */
static unsigned int share( uint32_t count, uint32_t total )
{
    return total ? (unsigned int)((uint64_t)count * 1000 / total) : 0;
}

static int image_at( uint64_t address, void **base, char *name, size_t size )
{
    return &wine_nx_image_at && wine_nx_image_at( (void *)(uintptr_t)address, base, name, size );
}

/* The runtime's own functions by name (source/runtime_symbols.c). */
extern int wine_nx_runtime_symbol( uintptr_t address, char *out, size_t size ) __attribute__((weak));

static void name_address( uint64_t address, char *buf, size_t size )
{
    char module[64];
    void *base;

    if (image_at( address, &base, module, sizeof(module) ))
        snprintf( buf, size, "%s+%#lx", module[0] ? module : "exe", (unsigned long)(address - (uintptr_t)base) );
    else if (!wine_nx_runtime_symbol || (!wine_nx_runtime_symbol( (uintptr_t)address, buf, size ) && !buf[0]))
        snprintf( buf, size, "%#lx", (unsigned long)address );
}

static void name_site( uint32_t site, int guest, char *buf, size_t size )
{
    if (site == NX_PROF_NO_SITE) snprintf( buf, size, "-" );
    else if (guest) name_address( site, buf, size );
    else snprintf( buf, size, "+%#x", (unsigned int)site );
}

/* Where every thread is standing, once nothing has moved for a while. A program
 * waiting for something that will not come looks like an idle one from outside,
 * and the only difference is where its threads are stopped. Each is paused just
 * long enough to read it, as the sampler does, and set going again. */
void wine_nx_threads_report_stalled( void )
{
    Handle self = threadGetCurHandle();
    unsigned int i, reported = 0;

    for (i = 0; i < NX_PROF_MAX_THREADS; i++)
    {
        uint64_t callers[NX_PROF_DEPTH], native[NX_PROF_DEPTH], words[CARAFE_SCAN_WORDS];
        uint32_t x86_callers[NX_PROF_DEPTH], eip = 0, esp, ebp;
        char line[NX_PROF_LINE], where[96], kind;
        Handle handle = registry[i].handle;
        unsigned int frames, copied = 0, natives, f, tid, x86_frames = 0;
        int x86 = 0;
        ThreadContext ctx;
        Result rc;
        int tries, len;

        if (!handle || handle == self) continue;
        pthread_mutex_lock( &profile_mutex );
        if (R_FAILED( svcSetThreadActivity( handle, ThreadActivity_Paused ) ))
        {
            pthread_mutex_unlock( &profile_mutex );
            continue;
        }
        for (tries = 0; R_FAILED( rc = svcGetThreadContext3( &ctx, handle ) ) && tries < 4; tries++)
            svcSleepThread( 0 );
        frames = R_SUCCEEDED( rc ) ? walk_callers( &ctx, callers ) : 0;
        tid = registry[i].tid;
        kind = registry[i].kind;
        if (R_SUCCEEDED( rc ) && kind == 'w')
        {
            copied = copy_stack( &ctx, words );
            if ((x86 = x86_registers( registry[i].teb, &eip, &esp, &ebp )))
                x86_frames = walk_x86( registry[i].teb, x86_callers );
        }
        svcSetThreadActivity( handle, ThreadActivity_Runnable );
        pthread_mutex_unlock( &profile_mutex );
        if (R_FAILED( rc )) continue;
        natives = pick_calls( words, copied, native );
        name_address( ctx.pc.x, where, sizeof(where) );
        len = snprintf( line, sizeof(line), "[STALL] thread %u%c at %s sp=%#lx lr=%#lx",
                        tid, kind, where,
                        (unsigned long)ctx.sp, (unsigned long)ctx.lr );
        for (f = 0; f < frames; f++)
        {
            name_address( callers[f], where, sizeof(where) );
            len = appendf( line, len, " < %s", where );
        }
        if (natives) len = appendf( line, len, " | native" );
        for (f = 0; f < natives; f++)
        {
            name_address( native[f], where, sizeof(where) );
            len = appendf( line, len, " < %s", where );
        }
        if (x86)
        {
            name_address( eip, where, sizeof(where) );
            len = appendf( line, len, " | x86 at %s esp=%#x ebp=%#x", where, esp, ebp );
        }
        for (f = 0; f < x86_frames; f++)
        {
            name_address( x86_callers[f], where, sizeof(where) );
            len = appendf( line, len, " < %s", where );
        }
        wine_nx_runtime_trace( line );
        reported++;
    }
    {
        char line[NX_PROF_LINE];

        snprintf( line, sizeof(line), "[STALL] %u of %u threads read", reported, wine_nx_threads_other() );
        wine_nx_runtime_trace( line );
    }
}

/* Translated code by module, busiest first. */
static void report_modules( const struct nx_prof_target *target, enum nx_prof_kind kind, const char *label,
                            const char *outside, char *line )
{
    struct { void *base; char name[48]; uint32_t count; } modules[16], swap;
    const struct nx_prof_table *table = &target->tables[kind];
    unsigned int count = 0, i, j;
    uint32_t unknown = 0;
    int len;

    for (i = 0; i < NX_PROF_BUCKETS; i++)
    {
        const struct nx_prof_bucket *bucket = &table->buckets[i];
        char name[48];
        void *base;

        if (!bucket->count) continue;
        if (!image_at( bucket->key, &base, name, sizeof(name) ))
        {
            unknown += bucket->count;
            continue;
        }
        for (j = 0; j < count && modules[j].base != base; j++) continue;
        if (j == count)
        {
            if (count == sizeof(modules) / sizeof(modules[0]))
            {
                unknown += bucket->count;
                continue;
            }
            modules[j].base = base;
            snprintf( modules[j].name, sizeof(modules[j].name), "%s", name[0] ? name : "exe" );
            modules[j].count = 0;
            count++;
        }
        modules[j].count += bucket->count;
    }
    for (i = 1; i < count; i++)
        for (j = i; j > 0 && modules[j - 1].count < modules[j].count; j--)
        {
            swap = modules[j - 1];
            modules[j - 1] = modules[j];
            modules[j] = swap;
        }
    len = appendf( line, 0, "[PROF] %u%c %s by module:", target->tid, target->kind, label );
    for (i = 0; i < count && i < 8; i++)
        len = appendf( line, len, " %s %u.%u%%", modules[i].name, share( modules[i].count, target->samples ) / 10,
                       share( modules[i].count, target->samples ) % 10 );
    if (unknown) len = appendf( line, len, " %s %u.%u%%", outside, share( unknown, target->samples ) / 10,
                                share( unknown, target->samples ) % 10 );
    if (table->dropped)
        len = appendf( line, len, " unbucketed %u.%u%%", share( table->dropped, target->samples ) / 10,
                       share( table->dropped, target->samples ) % 10 );
    wine_nx_runtime_trace( line );
}

static void report_fex_parts( const struct nx_prof_target *target, char *line )
{
    static const char *const names[NX_PROF_FEX_PARTS] = { "block-entry", "lookup", "link-compile", "divide",
                                                          "x87-math", "fallback", "dispatcher" };
    unsigned int k, total = 0;
    int len;

    for (k = 0; k < NX_PROF_FEX_PARTS; k++) total += target->fex_parts[k];
    if (!total) return;
    len = appendf( line, 0, "[PROF] %u%c fex outside blocks:", target->tid, target->kind );
    for (k = 0; k < NX_PROF_FEX_PARTS; k++)
        len = appendf( line, len, " %s %u.%u%%", names[k], share( target->fex_parts[k], target->samples ) / 10,
                       share( target->fex_parts[k], target->samples ) % 10 );
    wine_nx_runtime_trace( line );
}

static void report_stages( const struct nx_prof_target *target, char *line )
{
    unsigned int level, i, j, shown, top[NX_PROF_STAGE_SHOWN];
    char name[96];
    int len;

    len = appendf( line, 0, "[PROF] %u%c stages from the game's first frame on the stack, "
                            "%u.%u%% of samples reach it:", target->tid,
                   target->kind, share( target->rooted, target->samples ) / 10,
                   share( target->rooted, target->samples ) % 10 );
    for (level = 1; level <= NX_PROF_STAGE_LEVELS; level++)
    {
        shown = 0;
        for (i = 0; i < NX_PROF_BUCKETS; i++)
        {
            const struct nx_prof_bucket *bucket = &target->stages.buckets[i];

            if (!bucket->count || (bucket->key >> 32) != level) continue;
            j = shown < NX_PROF_STAGE_SHOWN ? shown++ : NX_PROF_STAGE_SHOWN;
            for (; j > 0 && target->stages.buckets[top[j - 1]].count < bucket->count; j--)
                if (j < NX_PROF_STAGE_SHOWN) top[j] = top[j - 1];
            if (j < NX_PROF_STAGE_SHOWN) top[j] = i;
        }
        if (!shown) break;
        len = appendf( line, len, " L%u", level );
        for (j = 0; j < shown; j++)
        {
            const struct nx_prof_bucket *bucket = &target->stages.buckets[top[j]];

            name_address( (uint32_t)bucket->key, name, sizeof(name) );
            len = appendf( line, len, " %s %u.%u%%", name, share( bucket->count, target->rooted ) / 10,
                           share( bucket->count, target->rooted ) % 10 );
        }
    }
    wine_nx_runtime_trace( line );
}

static void profile_report( const struct nx_prof_row *rows, unsigned int count )
{
    static const char *const table_names[NX_PROF_TABLES] = { "x86", "pe", "native", "svc", "callers", "x86 callers",
                                                             "x86 frames" };
    char line[NX_PROF_LINE + 24], name[96], outer[96];
    unsigned int i, j, k, shown, top[12];
    int len;

    pthread_mutex_lock( &profile_mutex );
    for (i = 0; i < NX_PROF_TARGETS; i++)
    {
        const struct nx_prof_target *target = &targets[i];

        if (!target->samples) continue;
        len = appendf( line, 0, "[PROF] %u%c samples=%u missed=%u", target->tid, target->kind,
                       target->samples, target->missed );
        for (k = 0; k < NX_PROF_KINDS; k++)
            len = appendf( line, len, " %s=%u.%u%%", table_names[k], share( target->kinds[k], target->samples ) / 10,
                           share( target->kinds[k], target->samples ) % 10 );
        wine_nx_runtime_trace( line );
        if (target->kinds[NX_PROF_X86]) report_modules( target, NX_PROF_X86, "x86", "unknown", line );
        if (target->kinds[NX_PROF_IMAGE]) report_modules( target, NX_PROF_IMAGE, "pe", "jit", line );
        report_fex_parts( target, line );
        for (k = 0; k < NX_PROF_TABLES; k++)
        {
            const struct nx_prof_table *table = &target->tables[k];
            int guest = k == NX_PROF_X86_CALLERS || k == NX_PROF_X86_FRAMES;

            if (k < NX_PROF_KINDS ? !target->kinds[k] : !nx_prof_top( table, top, 1 )) continue;
            shown = nx_prof_top( table, top, k == NX_PROF_IMAGE ? 6 : 12 );
            len = appendf( line, 0, "[PROF] %u%c %s:", target->tid, target->kind, table_names[k] );
            for (j = 0; j < shown; j++)
            {
                const struct nx_prof_bucket *bucket = &table->buckets[top[j]];

                if (k == NX_PROF_X86 || k == NX_PROF_IMAGE) name_address( bucket->key, name, sizeof(name) );
                else if (k == NX_PROF_NATIVE) snprintf( name, sizeof(name), "+%#llx", (unsigned long long)bucket->key );
                else if (k == NX_PROF_SVC)
                    snprintf( name, sizeof(name), "svc%#llx<+%#llx", (unsigned long long)(bucket->key >> 40),
                              (unsigned long long)(bucket->key & 0xffffffffffull) );
                else
                {
                    name_site( (uint32_t)(bucket->key >> 32), guest, name, sizeof(name) );
                    name_site( (uint32_t)bucket->key, guest, outer, sizeof(outer) );
                    snprintf( name + strlen( name ), sizeof(name) - strlen( name ), "<%s", outer );
                }
                len = appendf( line, len, " %s %u.%u%%", name, share( bucket->count, target->samples ) / 10,
                               share( bucket->count, target->samples ) % 10 );
            }
            if (table->dropped) len = appendf( line, len, " (+%u in no bucket)", table->dropped );
            wine_nx_runtime_trace( line );
        }
        if (target->rooted) report_stages( target, line );
    }
    /* The next interval samples the threads busiest in this one. */
    memset( targets, 0, sizeof(targets) );
    for (i = j = 0; i < count && j < NX_PROF_TARGETS && rows[i].permille >= NX_PROF_MIN_PERMILLE; i++, j++)
    {
        targets[j].handle = rows[i].handle;
        targets[j].tid = rows[i].tid;
        targets[j].kind = rows[i].kind;
        targets[j].teb = rows[i].teb;
    }
    pthread_mutex_unlock( &profile_mutex );
}

/* The server requests that took the most time since the last report, with
 * calls, average and total. A wait (select) includes the time it waited. */
static void server_report( void )
{
    static unsigned int last_calls[NX_PROF_MAX_REQUESTS], delta_calls[NX_PROF_MAX_REQUESTS];
    static unsigned long long last_ticks[NX_PROF_MAX_REQUESTS], delta_ticks[NX_PROF_MAX_REQUESTS];
    unsigned int count, i, j, top[8], shown = 0, calls = 0;
    unsigned long long ticks = 0;
    char line[NX_PROF_LINE + 24];
    int len;

    if (!&wine_nx_server_request_count || !&wine_nx_server_calls || !&wine_nx_server_ticks) return;
    count = wine_nx_server_request_count < NX_PROF_MAX_REQUESTS ? wine_nx_server_request_count : NX_PROF_MAX_REQUESTS;
    for (i = 0; i < count; i++)
    {
        unsigned int now_calls = __atomic_load_n( &wine_nx_server_calls[i], __ATOMIC_RELAXED );
        unsigned long long now_ticks = __atomic_load_n( &wine_nx_server_ticks[i], __ATOMIC_RELAXED );

        delta_calls[i] = now_calls - last_calls[i];
        delta_ticks[i] = now_ticks - last_ticks[i];
        last_calls[i] = now_calls;
        last_ticks[i] = now_ticks;
        calls += delta_calls[i];
        ticks += delta_ticks[i];
        if (!delta_calls[i]) continue;
        j = shown < 8 ? shown++ : 8;
        for (; j > 0 && delta_ticks[top[j - 1]] < delta_ticks[i]; j--)
            if (j < 8) top[j] = top[j - 1];
        if (j < 8) top[j] = i;
    }
    if (!calls) return;
    len = appendf( line, 0, "[SERVER] %u requests, %llu ms:", calls, (unsigned long long)armTicksToNs( ticks ) / 1000000 );
    for (i = 0; i < shown; i++)
    {
        unsigned long long ns = armTicksToNs( delta_ticks[top[i]] );
        const char *name = &wine_nx_server_names ? __atomic_load_n( &wine_nx_server_names[top[i]], __ATOMIC_RELAXED ) : NULL;

        len = appendf( line, len, " %s %u avg %lluus %llums", name ? name : "?", delta_calls[top[i]],
                       ns / delta_calls[top[i]] / 1000, ns / 1000000 );
    }
    wine_nx_runtime_trace( line );
}

static void jit_dump_hex( FILE *file, const char *label, uint64_t address, uint64_t size )
{
    static const char digits[] = "0123456789abcdef";
    char chunk[512];
    unsigned int n = 0;
    MemoryInfo info;
    uint64_t i;

    fprintf( file, "%s %#llx %#llx ", label, (unsigned long long)address, (unsigned long long)size );
    if (readable_size( address, size, &info ))
        for (i = 0; i < size; i++)
        {
            uint8_t byte = ((const volatile uint8_t *)(uintptr_t)address)[i];

            chunk[n++] = digits[byte >> 4];
            chunk[n++] = digits[byte & 15];
            if (n == sizeof(chunk))
            {
                fwrite( chunk, 1, n, file );
                n = 0;
            }
        }
    chunk[n++] = '\n';
    fwrite( chunk, 1, n, file );
}

static void jit_dump_block( FILE *file, const struct nx_prof_bucket *bucket )
{
    uint64_t block = bucket->key, tail, size, host = 0, guest, low, high, entry;
    uint32_t entries, i;
    MemoryInfo info;

    if (!readable_size( block, sizeof(uint32_t), &info )) return;
    tail = block + read_word32( block );
    if (!readable_size( tail, NX_PROF_FEX_TAIL_SIZE, &info )) return;
    size = read_word( tail );
    if (size > NX_PROF_JIT_HOST_MAX) size = NX_PROF_JIT_HOST_MAX;
    guest = low = high = read_word( tail + NX_PROF_FEX_TAIL_RIP );
    entries = read_word32( tail + NX_PROF_FEX_TAIL_ENTRIES );
    entry = tail + read_word32( tail + NX_PROF_FEX_TAIL_ENTRY_OFFSET );
    fprintf( file, "block %#llx samples %u rip %#llx size %#llx entries %u\nmap", (unsigned long long)block,
             bucket->count, (unsigned long long)guest, (unsigned long long)size, entries );
    for (i = 0; i < entries; i++)
    {
        uint64_t arm, rip;
        size_t step;

        if (!readable( entry, &info )) break;
        step = nx_prof_vl64pair_size( *(const uint8_t *)(uintptr_t)entry );
        if (!readable_size( entry, step, &info )) break;
        nx_prof_vl64pair( (const uint8_t *)(uintptr_t)entry, &arm, &rip );
        host += arm;
        guest += rip;
        entry += step;
        if (guest < low) low = guest;
        if (guest > high) high = guest;
        fprintf( file, " %#llx:%#llx", (unsigned long long)host, (unsigned long long)guest );
    }
    fputs( "\nhot", file );
    for (i = 0; i < NX_PROF_JIT_PCS; i++)
        if (jit_pcs[i].count && jit_pcs[i].key - block < size)
            fprintf( file, " %#llx:%u", (unsigned long long)(jit_pcs[i].key - block), jit_pcs[i].count );
    fputc( '\n', file );
    if (high - low + 16 > NX_PROF_JIT_GUEST_MAX) high = low + NX_PROF_JIT_GUEST_MAX - 16;
    jit_dump_hex( file, "guest", low, high - low + 16 );
    jit_dump_hex( file, "host", block, size );
}

static void jit_dump( void )
{
    static unsigned int top[NX_PROF_JIT_BLOCKS];
    static struct nx_prof_bucket blocks[NX_PROF_JIT_BLOCKS];
    unsigned int shown, i;
    uint32_t samples, dropped;
    char line[200];
    FILE *file;

    pthread_mutex_lock( &profile_mutex );
    shown = nx_prof_top( &jit_blocks, top, NX_PROF_JIT_BLOCKS );
    for (i = 0; i < shown; i++) blocks[i] = jit_blocks.buckets[top[i]];
    samples = jit_samples;
    dropped = jit_blocks.dropped;
    jit_next_dump = samples * 3;
    pthread_mutex_unlock( &profile_mutex );
    if (!(file = fopen( NX_PROF_JIT_FILE, "w" ))) return;
    fprintf( file, "jit-blocks 1 samples %u dropped %u blocks %u\n", samples, dropped, shown );
    for (i = 0; i < shown; i++) jit_dump_block( file, &blocks[i] );
    fclose( file );
    snprintf( line, sizeof(line), "[PROF] jit: the %u busiest blocks of %u samples in logs/jit-blocks.txt", shown,
              samples );
    wine_nx_runtime_trace( line );
}

/* The tick the shares are measured from, and the rows themselves: filled under
 * registry_mutex, which is held for the whole walk, so neither the [THREADS]
 * line nor the tasks panel races the registry. */
static u64 rows_last;
static struct nx_prof_row rows_all[NX_PROF_MAX_THREADS];

static unsigned int collect_rows( struct nx_prof_row *rows, unsigned int max, unsigned int *total )
{
    u64 now, interval, mask;
    unsigned int count = 0, i, j;
    struct nx_prof_row row;

    *total = 0;
    pthread_mutex_lock( &registry_mutex );
    now = armGetSystemTick();
    interval = rows_last ? now - rows_last : 0;
    rows_last = now;
    for (i = 0; i < NX_PROF_MAX_THREADS; i++)
    {
        struct nx_prof_thread *thread = &registry[i];
        uint64_t ticks;

        if (!thread->handle) continue;
        ticks = thread_ticks( thread->handle );
        if (!interval)
        {
            thread->last_ticks = ticks;
            continue;
        }
        row.handle = thread->handle;
        row.tid = thread->tid;
        row.kind = thread->kind;
        memcpy( row.name, thread->name, sizeof(row.name) );
        row.teb = thread->teb;
        row.permille = nx_prof_permille( ticks, thread->last_ticks, interval );
        if (R_FAILED( svcGetThreadCoreMask( &row.core, &mask, thread->handle ) )) row.core = thread->core;
        thread->last_ticks = ticks;
        for (j = count++; j > 0 && rows_all[j - 1].permille < row.permille; j--) rows_all[j] = rows_all[j - 1];
        rows_all[j] = row;
        *total += row.permille;
    }
    if (!interval)
    {
        pthread_mutex_unlock( &registry_mutex );
        return 0;
    }
    if (count > max) count = max;
    memcpy( rows, rows_all, count * sizeof(*rows) );
    pthread_mutex_unlock( &registry_mutex );
    return count;
}

unsigned int wine_nx_tasks_snapshot( struct wine_nx_task *tasks, unsigned int max, unsigned int *total )
{
    /* Twelve rows fill a panel that is one column, and two columns are 24: no
     * caller needs more than this, and the panel is drawn from a thread whose
     * stack is not the runtime's. */
    struct nx_prof_row rows[24];
    unsigned int count, i;

    if (max > sizeof(rows) / sizeof(rows[0])) max = sizeof(rows) / sizeof(rows[0]);
    count = collect_rows( rows, max, total );
    for (i = 0; i < count; i++)
    {
        tasks[i].tid = rows[i].tid;
        tasks[i].kind = rows[i].kind;
        tasks[i].core = (int)rows[i].core;
        tasks[i].permille = rows[i].permille;
        snprintf( tasks[i].name, sizeof(tasks[i].name), "%s", rows[i].name );
    }
    return count;
}

void wine_nx_thread_report( void )
{
    struct nx_prof_row rows[NX_PROF_MAX_THREADS];
    unsigned int count, total = 0, i;
    char line[NX_PROF_LINE + 24];
    int len;

    count = collect_rows( rows, NX_PROF_MAX_THREADS, &total );
    if (!count) return;

    /* tid, w (Wine) or s (server connection), @core, share of that core. */
    len = appendf( line, 0, "[THREADS] %u threads use %u.%02u cores:", count, total / 1000, total % 1000 / 10 );
    for (i = 0; i < count && i < 12 && rows[i].permille >= 5; i++)
        len = appendf( line, len, " %u%c%s%s@%d %u.%u%%", rows[i].tid, rows[i].kind,
                       rows[i].name[0] ? ":" : "", rows[i].name, (int)rows[i].core,
                       rows[i].permille / 10, rows[i].permille % 10 );
    wine_nx_runtime_trace( line );
    if (profiling) server_report();
    if (profiling) profile_report( rows, count );
    if (profiling && __atomic_load_n( &jit_samples, __ATOMIC_RELAXED ) >= jit_next_dump) jit_dump();
}

/* Names the threads that did not stop, so the wait they are in can be found. */
void wine_nx_threads_report_unparked( void )
{
    Handle self = threadGetCurHandle();
    char line[256];
    unsigned int i;
    int len;

    len = appendf( line, 0, "[QUIT] still running:" );
    for (i = 0; i < NX_PROF_MAX_THREADS && len < 200; i++)
    {
        if (!registry[i].handle || registry[i].handle == self || registry[i].kind == 'c') continue;
        if (registry[i].parked) continue;
        len = appendf( line, len, " %u%c", registry[i].tid, registry[i].kind );
    }
    wine_nx_runtime_trace( line );
}

void wine_nx_thread_affinity_fixed( void )
{
    Handle handle = threadGetCurHandle();
    unsigned int i;

    affinity_fixed = 1;
    pthread_mutex_lock( &registry_mutex );
    for (i = 0; i < NX_PROF_MAX_THREADS; i++)
        if (registry[i].handle == handle)
        {
            registry[i].fixed = 1;
            registry[i].migratable = 0;
        }
    pthread_mutex_unlock( &registry_mutex );
}

/* Threads start pinned round-robin (horizon_pin_current_thread), before anyone
 * knows which will be busy: in NFSU2 the Direct3D drawing thread shared its
 * core with a worker and server threads while another core idled, and the main
 * thread ran on any core. This measures the last two seconds and moves threads
 * when that clearly lowers the busiest core's load, or when a busy thread is on
 * no single core. A server connection thread's load counts for its client. */
void wine_nx_thread_balance( void )
{
    static u64 last;
    struct nx_balance_thread balance[NX_PROF_MAX_THREADS];
    unsigned int loads[NX_PROF_MAX_THREADS], owner[NX_PROF_MAX_THREADS], core_ids[NX_BALANCE_MAX_CORES];
    unsigned int cores = 0, count = 0, before, after, i, j;
    u64 now = armGetSystemTick(), interval = last ? now - last : 0, process_mask, mask;
    char line[NX_PROF_LINE + 24];
    int len, urgent = 0;

    last = now;
    if (!wine_nx_balance_enabled || R_FAILED( svcGetInfo( &process_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0 ) ))
        return;
    process_mask &= 7;
    for (i = 0; i < 64 && cores < NX_BALANCE_MAX_CORES; i++)
        if (process_mask & (1ull << i)) core_ids[cores++] = i;

    pthread_mutex_lock( &registry_mutex );
    for (i = 0; i < NX_PROF_MAX_THREADS; i++)
    {
        uint64_t ticks;

        if (!registry[i].handle) continue;
        ticks = thread_ticks( registry[i].handle );
        loads[i] = nx_prof_permille( ticks, registry[i].balance_ticks, interval );
        registry[i].balance_ticks = ticks;
    }
    for (i = 0; interval && cores > 1 && i < NX_PROF_MAX_THREADS; i++)
    {
        struct nx_prof_thread *thread = &registry[i];
        s32 core;

        if (!thread->handle || thread->kind != 'w' || thread->helper) continue;
        balance[count].load = loads[i];
        balance[count].fixed = thread->fixed || thread->migratable;
        balance[count].core = -1;
        if (R_SUCCEEDED( svcGetThreadCoreMask( &core, &mask, thread->handle ) ) && mask && !(mask & (mask - 1)))
            for (j = 0; j < cores; j++) if (mask == 1ull << core_ids[j]) balance[count].core = (int)j;
        for (j = 0; j < NX_PROF_MAX_THREADS; j++)
            if (registry[j].handle && registry[j].kind == 's' && registry[j].tid == thread->tid)
                balance[count].load += loads[j];
        if (balance[count].core < 0 && !balance[count].fixed && balance[count].load >= 2 * NX_BALANCE_LIGHT) urgent = 1;
        owner[count++] = i;
    }
    if (!count)
    {
        pthread_mutex_unlock( &registry_mutex );
        return;
    }
    after = nx_balance_assign( balance, count, cores, &before );
    if (!urgent && after + 5 * NX_BALANCE_SLACK / 3 >= before)
    {
        pthread_mutex_unlock( &registry_mutex );
        return;
    }
    len = appendf( line, 0, "[BALANCE] busiest core %u.%u%% -> %u.%u%%:", before / 10, before % 10, after / 10, after % 10 );
    for (i = 0; i < count; i++)
    {
        struct nx_prof_thread *thread = &registry[owner[i]];
        unsigned int core_id;

        if (balance[i].new_core < 0 || balance[i].new_core == balance[i].core) continue;
        core_id = core_ids[balance[i].new_core];
        if (R_FAILED( svcSetThreadCoreMask( thread->handle, (s32)core_id, 1u << core_id ) )) continue;
        /* Still registered, so its TEB is still there. */
        if (thread->teb && &horizon_follow_thread_cores)
            horizon_follow_thread_cores( (void *)(uintptr_t)thread->teb, 1u << core_id );
        if (balance[i].core >= 0)
            len = appendf( line, len, " %uw %u->%u %u.%u%%", thread->tid, core_ids[balance[i].core], core_id,
                           balance[i].load / 10, balance[i].load % 10 );
        else
            len = appendf( line, len, " %uw any->%u %u.%u%%", thread->tid, core_id, balance[i].load / 10,
                           balance[i].load % 10 );
    }
    pthread_mutex_unlock( &registry_mutex );
    wine_nx_runtime_trace( line );
}

struct wine_nx_thread_view
{
    unsigned int tid;
    char kind;
    char name[32];
    unsigned int permille;
    u64 mask;
    int movable;
    int side;
};

unsigned int wine_nx_thread_side_cores( void )
{
    return four_cores_enabled ? 8u : 0u;
}

unsigned int wine_nx_thread_views( struct wine_nx_thread_view *views, unsigned int max )
{
    static u64 last;
    u64 now = armGetSystemTick(), interval = last ? now - last : 0;
    unsigned int i, count = 0;
    s32 core;

    last = now;
    pthread_mutex_lock( &registry_mutex );
    for (i = 0; i < NX_PROF_MAX_THREADS; i++)
    {
        struct nx_prof_thread *thread = &registry[i];
        uint64_t ticks;

        if (!thread->handle) continue;
        ticks = thread_ticks( thread->handle );
        if (interval && count < max)
        {
            struct wine_nx_thread_view *view = &views[count++];

            view->tid = thread->tid;
            view->kind = thread->kind;
            memcpy( view->name, thread->name, sizeof(view->name) );
            view->permille = nx_prof_permille( ticks, thread->orchestrator_ticks, interval );
            if (R_FAILED( svcGetThreadCoreMask( &core, &view->mask, thread->handle ) )) view->mask = 0;
            view->movable = thread->kind == 'w' && !thread->fixed && !thread->migratable;
            view->side = thread->helper;
        }
        thread->orchestrator_ticks = ticks;
    }
    pthread_mutex_unlock( &registry_mutex );
    return count;
}

int wine_nx_thread_place( unsigned int tid, s32 core, u32 mask )
{
    unsigned int i;
    int placed = 0;

    pthread_mutex_lock( &registry_mutex );
    for (i = 0; i < NX_PROF_MAX_THREADS; i++)
    {
        struct nx_prof_thread *thread = &registry[i];

        if (!thread->handle || thread->kind != 'w' || thread->tid != tid) continue;
        if (thread->fixed || thread->migratable) break;
        if (mask == 8u)
            placed = thread->helper || offload_thread_locked( thread ) > 0;
        else if (thread->helper)
            placed = restore_thread_locked( thread, core, mask );
        else if (R_SUCCEEDED( svcSetThreadCoreMask( thread->handle, core, mask ) ))
        {
            if (thread->teb && &horizon_follow_thread_cores)
                horizon_follow_thread_cores( (void *)(uintptr_t)thread->teb, mask );
            placed = 1;
        }
        break;
    }
    pthread_mutex_unlock( &registry_mutex );
    return placed;
}

void wine_nx_thread_reserve_core( s32 core )
{
    unsigned int i;
    u64 process_mask, mask;

    if (R_FAILED( svcGetInfo( &process_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0 ) ) || !(process_mask &= 7))
        return;
    pthread_mutex_lock( &registry_mutex );
    reserved_cores = core >= 0 ? 1u << core : 0;
    mask = process_mask & ~reserved_cores ? process_mask & ~reserved_cores : process_mask;
    for (i = 0; i < NX_PROF_MAX_THREADS; i++)
    {
        struct nx_prof_thread *thread = &registry[i];

        if (!thread->handle || !thread->migratable || thread->fixed) continue;
        if (R_SUCCEEDED( svcSetThreadCoreMask( thread->handle, -1, mask ) ) && thread->teb &&
            &horizon_follow_thread_cores)
            horizon_follow_thread_cores( (void *)(uintptr_t)thread->teb, mask );
    }
    pthread_mutex_unlock( &registry_mutex );
}

unsigned int wine_nx_thread_tid( Handle handle, char *kind, char *name, size_t size )
{
    unsigned int i, tid = 0;

    pthread_mutex_lock( &registry_mutex );
    for (i = 0; i < NX_PROF_MAX_THREADS; i++)
    {
        if (registry[i].handle != handle) continue;
        tid = registry[i].tid;
        if (kind) *kind = registry[i].kind;
        if (name && size) snprintf( name, size, "%s", registry[i].name );
        break;
    }
    pthread_mutex_unlock( &registry_mutex );
    return tid;
}
