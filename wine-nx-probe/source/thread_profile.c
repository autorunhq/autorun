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
#define NX_PROF_TARGETS      4
#define NX_PROF_PERIOD_NS    2000000
#define NX_PROF_MIN_PERMILLE 30  /* sample threads that used at least 3% of a core */
#define NX_PROF_DEPTH        8
#define NX_PROF_LINE         1000
#define NX_PROF_MAX_REQUESTS 1024

extern void wine_nx_runtime_trace( const char *msg );
extern int wine_nx_box64_pc_to_x86( uintptr_t pc, uintptr_t *x86 ) __attribute__((weak));
extern int wine_nx_image_at( const void *addr, void **base, char *name, size_t size ) __attribute__((weak));
extern char __end__[];
/* dlls/ntdll/unix/server.c: round trips to the in-process server by request. */
extern unsigned int wine_nx_server_request_count __attribute__((weak));
extern unsigned int wine_nx_server_calls[] __attribute__((weak));
extern unsigned long long wine_nx_server_ticks[] __attribute__((weak));
extern const char *wine_nx_server_names[] __attribute__((weak));

struct nx_prof_thread
{
    Handle handle;  /* 0: free */
    unsigned int tid;
    char kind;
    int parked;     /* stopped at a quit point, waiting to hear whether to end */
    s32 core;       /* as the thread saw itself when it registered */
    int fixed;      /* the program chose its cores */
    int helper;
    char name[32];
    s32 app_core, app_priority;
    u64 app_mask;
    uint64_t teb;
    uint64_t last_ticks, balance_ticks;
};

/* horizon.c: a moved Wine thread's server connection thread follows it. */
extern void horizon_follow_thread_cores( void *teb, unsigned int mask ) __attribute__((weak));
static __thread int affinity_fixed;
int wine_nx_balance_enabled = 1;
static int four_cores_enabled;

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
    struct nx_prof_table tables[NX_PROF_TABLES];
};

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
    return 1;
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
        snprintf( thread->name, sizeof(thread->name), "%s", name );
        if (nx_thread_graphics_worker( name ))
        {
            if ((moved = offload_thread_locked( thread )))
                snprintf( line, sizeof(line), "[CORES] %u %s: %s", tid, name,
                          moved > 0 ? "core 3, priority 63" : "could not offload graphics worker" );
        }
        else if (thread->helper) restore_thread_locked( thread, thread->app_core, thread->app_mask );
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
        {
            thread->fixed = 1;
            if (thread->teb && &horizon_follow_thread_cores)
                horizon_follow_thread_cores( (void *)(uintptr_t)thread->teb, mask );
        }
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
        registry[slot].last_ticks = registry[slot].balance_ticks = thread_ticks( handle );
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
        if (registry[i].handle == handle) registry[i].handle = 0;
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

static unsigned int walk_x86( uint64_t teb, uint32_t *callers )
{
    MemoryInfo info;
    uint64_t cpu, context;

    if (!teb || !(cpu = read_word( teb + NX_PROF_TEB_CPU_AREA ))) return 0;
    context = cpu + NX_PROF_CPU_CONTEXT;
    if (!readable( read_word32( context + NX_PROF_I386_ESP ), &info )) return 0;
    return nx_prof_x86_callers( read_word32( context + NX_PROF_I386_ESP ), read_word32( context + NX_PROF_I386_EBP ),
                                info.addr, info.addr + info.size, read_word32, callers, NX_PROF_DEPTH );
}

/* Inside translated code, the x86 frame chain from the guest's EBP and ESP. */
static unsigned int walk_x86_frames( const ThreadContext *ctx, uint32_t *callers )
{
    uint32_t esp = (uint32_t)ctx->cpu_gprs[14].x, ebp = (uint32_t)ctx->cpu_gprs[15].x;
    MemoryInfo info;

    if (!readable( esp, &info )) return 0;
    return nx_prof_x86_frames( esp, ebp, info.addr, info.addr + info.size, read_word32, callers, NX_PROF_DEPTH );
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
    unsigned int i, tries, count, x86_count;
    uintptr_t x86 = 0;
    int translated;

    (void)arg;
    for (;;)
    {
        svcSleepThread( NX_PROF_PERIOD_NS );
        /* It reads other threads' stacks and contexts, so it must stop before
         * they and the memory they ran in are taken away. */
        if (&wine_nx_quit_requested && wine_nx_quit_requested) break;
        pthread_mutex_lock( &profile_mutex );
        for (i = 0; i < NX_PROF_TARGETS; i++)
        {
            struct nx_prof_target *target = &targets[i];
            ThreadContext ctx;
            Result rc;

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
            translated = 0;
            if (R_SUCCEEDED( rc ))
            {
                count = walk_callers( &ctx, callers );
                if (&wine_nx_box64_pc_to_x86) translated = wine_nx_box64_pc_to_x86( ctx.pc.x, &x86 );
                x86_count = translated ? walk_x86_frames( &ctx, x86_callers ) :
                                          walk_x86( target->teb, x86_callers );
            }
            svcSetThreadActivity( target->handle, ThreadActivity_Runnable );
            if (R_SUCCEEDED( rc )) record( target, &ctx, translated, x86, callers, count, x86_callers, x86_count );
            else target->missed++;
        }
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

static void name_address( uint64_t address, char *buf, size_t size )
{
    char module[64];
    void *base;

    if (image_at( address, &base, module, sizeof(module) ))
        snprintf( buf, size, "%s+%#lx", module[0] ? module : "exe", (unsigned long)(address - (uintptr_t)base) );
    else
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
        uint64_t callers[NX_PROF_DEPTH];
        char line[NX_PROF_LINE], where[96], kind;
        Handle handle = registry[i].handle;
        unsigned int frames, f, tid;
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
        svcSetThreadActivity( handle, ThreadActivity_Runnable );
        pthread_mutex_unlock( &profile_mutex );
        if (R_FAILED( rc )) continue;
        name_address( ctx.pc.x, where, sizeof(where) );
        len = snprintf( line, sizeof(line), "[STALL] thread %u%c at %s sp=%#lx lr=%#lx",
                        tid, kind, where,
                        (unsigned long)ctx.sp, (unsigned long)ctx.lr );
        for (f = 0; f < frames; f++)
        {
            name_address( callers[f], where, sizeof(where) );
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
static void report_modules( const struct nx_prof_target *target, char *line )
{
    struct { void *base; char name[48]; uint32_t count; } modules[16], swap;
    const struct nx_prof_table *table = &target->tables[NX_PROF_X86];
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
    len = appendf( line, 0, "[PROF] %u%c x86 by module:", target->tid, target->kind );
    for (i = 0; i < count && i < 8; i++)
        len = appendf( line, len, " %s %u.%u%%", modules[i].name, share( modules[i].count, target->samples ) / 10,
                       share( modules[i].count, target->samples ) % 10 );
    if (unknown) len = appendf( line, len, " unknown %u.%u%%", share( unknown, target->samples ) / 10,
                                share( unknown, target->samples ) % 10 );
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
        if (target->kinds[NX_PROF_X86]) report_modules( target, line );
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

void wine_nx_thread_report( void )
{
    static u64 last;
    struct nx_prof_row rows[NX_PROF_MAX_THREADS], row;
    u64 now = armGetSystemTick(), interval = last ? now - last : 0, mask;
    unsigned int count = 0, total = 0, i, j;
    char line[NX_PROF_LINE + 24];
    int len;

    last = now;
    pthread_mutex_lock( &registry_mutex );
    for (i = 0; i < NX_PROF_MAX_THREADS; i++)
    {
        struct nx_prof_thread *thread = &registry[i];
        uint64_t ticks;

        if (!thread->handle) continue;
        ticks = thread_ticks( thread->handle );
        row.handle = thread->handle;
        row.tid = thread->tid;
        row.kind = thread->kind;
        memcpy( row.name, thread->name, sizeof(row.name) );
        row.teb = thread->teb;
        row.permille = nx_prof_permille( ticks, thread->last_ticks, interval );
        if (R_FAILED( svcGetThreadCoreMask( &row.core, &mask, thread->handle ) )) row.core = thread->core;
        thread->last_ticks = ticks;
        for (j = count++; j > 0 && rows[j - 1].permille < row.permille; j--) rows[j] = rows[j - 1];
        rows[j] = row;
        total += row.permille;
    }
    pthread_mutex_unlock( &registry_mutex );
    if (!interval) return;

    /* tid, w (Wine) or s (server connection), @core, share of that core. */
    len = appendf( line, 0, "[THREADS] %u threads use %u.%02u cores:", count, total / 1000, total % 1000 / 10 );
    for (i = 0; i < count && i < 12 && rows[i].permille >= 5; i++)
        len = appendf( line, len, " %u%c%s%s@%d %u.%u%%", rows[i].tid, rows[i].kind,
                       rows[i].name[0] ? ":" : "", rows[i].name, (int)rows[i].core,
                       rows[i].permille / 10, rows[i].permille % 10 );
    wine_nx_runtime_trace( line );
    if (profiling) server_report();
    if (profiling) profile_report( rows, count );
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
        if (registry[i].handle == handle) registry[i].fixed = 1;
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
        balance[count].fixed = thread->fixed;
        balance[count].core = -1;
        if (R_SUCCEEDED( svcGetThreadCoreMask( &core, &mask, thread->handle ) ) && mask && !(mask & (mask - 1)))
            for (j = 0; j < cores; j++) if (mask == 1ull << core_ids[j]) balance[count].core = (int)j;
        for (j = 0; j < NX_PROF_MAX_THREADS; j++)
            if (registry[j].handle && registry[j].kind == 's' && registry[j].tid == thread->tid)
                balance[count].load += loads[j];
        if (balance[count].core < 0 && !thread->fixed && balance[count].load >= 2 * NX_BALANCE_LIGHT) urgent = 1;
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
