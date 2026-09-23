/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 *
 * The runtime's thread profiler (thread_profile.c): its entry points, and the
 * parts that need no kernel, shared with tests/thread_profile.c.
 */
#ifndef WINE_NX_THREAD_PROFILE_H
#define WINE_NX_THREAD_PROFILE_H

#include <stdint.h>
#include <string.h>

int wine_nx_four_cores_available( void );
void wine_nx_thread_configure_cores( int enabled );
void wine_nx_thread_set_name( unsigned int tid, const char *name );
void wine_nx_thread_set_affinity( unsigned int tid, unsigned int mask );

static inline int nx_thread_graphics_worker( const char *name )
{
    return !strcmp( name, "dxvk-cs" ) || !strcmp( name, "wined3d_cs" ) || !strcmp( name, "vkd3d_queue" );
}

static inline int nx_thread_helper_available( uint64_t cores, uint64_t priorities )
{
    return !!(cores & 8) && !!(priorities & (UINT64_C(1) << 63));
}

/* Called by a thread as it starts and ends; kind is 'w' for Wine threads and
 * 's' for server connection threads (tid is then their client's). teb is a
 * Wine thread's own TEB, through which the sampler finds its x86 context. */
void wine_nx_thread_register( char kind, unsigned int tid, void *teb );
void wine_nx_thread_unregister( void );
/* Starts the sampler behind the [PROF] lines. */
void wine_nx_profile_start( void );
/* Waits for the sampler and gives its stack back, on the way to the launcher. */
void wine_nx_profile_stop( void );
/* With each [PROGRESS] report: the [THREADS] line, and [PROF] when sampling. */
void wine_nx_thread_report( void );
/* Interrupts every registered thread's wait but this one's, so each reaches its
 * next check; returns how many it interrupted. */
unsigned int wine_nx_threads_wake( void );
/* Registered threads other than this one. */
unsigned int wine_nx_threads_other( void );
/* The same, without the runtime's presenter: the threads of the program itself. */
unsigned int wine_nx_threads_program( void );
/* A thread stopping at a quit point, or going back to work. */
void wine_nx_thread_parked( int parked );
/* Where every thread is standing: for a program that has stopped making progress
 * without stopping. One line each, with the frames that could be walked. */
void wine_nx_threads_report_stalled( void );
/* Writes which threads have not stopped, by id and kind. */
void wine_nx_threads_report_unparked( void );
/* Called by a thread whose cores the program chose; the balancer leaves it. */
void wine_nx_thread_affinity_fixed( void );
/* Every two seconds: moves threads so the busiest ones get cores of their own
 * ([BALANCE]). Cleared by sdmc:/switch/wine/no-balance.txt. */
void wine_nx_thread_balance( void );
extern int wine_nx_balance_enabled;

/* The balancer's view of a thread: load in thousandths of a core, core an index
 * into the process's cores or -1 where the thread may run on any of them. */
struct nx_balance_thread
{
    unsigned int load;
    int core, fixed, new_core;
};

#define NX_BALANCE_MAX_CORES 8
#define NX_BALANCE_LIGHT     10  /* a thread below 1% of a core stays where it is */
#define NX_BALANCE_SLACK     30  /* a core must be 3% less loaded to be preferred */

/* Threads the program pinned, and light ones already on a core, stay; the rest
 * go heaviest first to the least loaded core, keeping their own unless another
 * is clearly less loaded. Sets new_core (-1 for a pinned thread on no single
 * core) and returns the busiest core's load after; *before is the busiest
 * before, not counting threads on no single core. */
static inline unsigned int nx_balance_assign( struct nx_balance_thread *threads, unsigned int count,
                                              unsigned int cores, unsigned int *before )
{
    unsigned int load_before[NX_BALANCE_MAX_CORES] = {0}, load_after[NX_BALANCE_MAX_CORES] = {0};
    unsigned int i, c, best, after = 0;

    if (cores > NX_BALANCE_MAX_CORES) cores = NX_BALANCE_MAX_CORES;
    for (i = 0; i < count; i++)
    {
        struct nx_balance_thread *thread = &threads[i];
        int on_core = thread->core >= 0 && (unsigned int)thread->core < cores;

        if (on_core) load_before[thread->core] += thread->load;
        if (thread->fixed && !on_core) thread->new_core = -1;
        else if (on_core && (thread->fixed || thread->load < NX_BALANCE_LIGHT))
        {
            thread->new_core = thread->core;
            load_after[thread->core] += thread->load;
        }
        else thread->new_core = -2;  /* to place */
    }
    for (;;)
    {
        struct nx_balance_thread *heaviest = NULL;

        for (i = 0; i < count; i++)
            if (threads[i].new_core == -2 && (!heaviest || threads[i].load > heaviest->load)) heaviest = &threads[i];
        if (!heaviest) break;
        best = heaviest->core >= 0 && (unsigned int)heaviest->core < cores ? (unsigned int)heaviest->core : 0;
        for (c = 0; c < cores; c++)
            if (load_after[c] + NX_BALANCE_SLACK <= load_after[best]) best = c;
        heaviest->new_core = (int)best;
        load_after[best] += heaviest->load;
    }
    *before = 0;
    for (c = 0; c < cores; c++)
    {
        if (load_before[c] > *before) *before = load_before[c];
        if (load_after[c] > after) after = load_after[c];
    }
    return after;
}

/* The number of an AArch64 "svc #imm" instruction, or -1. A thread blocked in
 * the kernel reports the instruction after its svc as its pc. */
static inline int nx_prof_svc_number( uint32_t insn )
{
    return (insn & 0xffe0001f) == 0xd4000001 ? (int)((insn >> 5) & 0xffff) : -1;
}

/* A thread paused in a system call reports its pc at the svc instruction or
 * just after it (hardware showed the former); the svc number, or -1. */
static inline int nx_prof_svc_at( uint32_t insn, uint32_t insn_before )
{
    int svc = nx_prof_svc_number( insn );

    return svc >= 0 ? svc : nx_prof_svc_number( insn_before );
}

/* The return addresses that led to a sample, innermost first: the link
 * register, which names the caller of a function that has not saved it (as
 * libnx's svc stubs and memcpy never do), then the AArch64 frame records from
 * fp while they stay inside the stack mapping [lo, hi) and move up it. read
 * returns the 8 bytes at an address inside that range. */
static inline unsigned int nx_prof_callers( uint64_t lr, uint64_t fp, uint64_t lo, uint64_t hi,
                                            uint64_t (*read)( uint64_t ), uint64_t *out, unsigned int max )
{
    unsigned int n = 0;

    if (max && lr) out[n++] = lr;
    while (n < max && fp >= lo && hi >= 16 && fp <= hi - 16 && !(fp & 7))
    {
        uint64_t next = read( fp ), ret = read( fp + 8 );

        if (ret && (!n || out[n - 1] != ret)) out[n++] = ret;
        if (next <= fp) break;
        fp = next;
    }
    return n;
}

/* A WoW64 thread's x86 context while it is outside translated code, at a system
 * call or unix call: its TEB's WOW64_TLS_CPURESERVED slot points at the CPU
 * area, whose I386_CONTEXT follows the 4-byte header. runtime.c checks these
 * against Wine's headers. */
#define NX_PROF_TEB_CPU_AREA 0x1488
#define NX_PROF_CPU_CONTEXT  4
#define NX_PROF_I386_EBP     0xb4
#define NX_PROF_I386_ESP     0xc4

/* Return addresses on that x86 stack, innermost first: the two words at esp
 * (at a gate, the returns into the stub and into its caller), then the frame
 * chain from ebp ([ebp] the saved ebp, [ebp + 4] the return address) while it
 * stays in the stack mapping [lo, hi) and moves up it. */
static inline unsigned int nx_prof_x86_callers( uint32_t esp, uint32_t ebp, uint64_t lo, uint64_t hi,
                                                uint32_t (*read)( uint64_t ), uint32_t *out, unsigned int max )
{
    unsigned int n = 0, i;

    for (i = 0; i < 2 && n < max; i++)
        if (esp >= lo && (uint64_t)esp + 4 * (i + 1) <= hi) out[n++] = read( (uint64_t)esp + 4 * i );
    while (n < max && ebp >= lo && ebp >= esp && (uint64_t)ebp + 8 <= hi && !(ebp & 3))
    {
        uint32_t next = read( ebp ), ret = read( (uint64_t)ebp + 4 );

        if (ret && (!n || out[n - 1] != ret)) out[n++] = ret;
        if (next <= ebp) break;
        ebp = next;
    }
    return n;
}

/* The frame chain alone, for a thread inside translated code, where Box64 keeps
 * the guest's ESP and EBP in x14 and x15 and the words at esp are not return
 * addresses in the middle of a function. */
static inline unsigned int nx_prof_x86_frames( uint32_t esp, uint32_t ebp, uint64_t lo, uint64_t hi,
                                               uint32_t (*read)( uint64_t ), uint32_t *out, unsigned int max )
{
    unsigned int n = 0;

    while (n < max && ebp >= lo && ebp >= esp && (uint64_t)ebp + 8 <= hi && !(ebp & 3))
    {
        uint32_t next = read( ebp ), ret = read( (uint64_t)ebp + 4 );

        if (ret && (!n || out[n - 1] != ret)) out[n++] = ret;
        if (next <= ebp) break;
        ebp = next;
    }
    return n;
}

/* Two call sites as one key; each is an offset into the runtime, or
 * NX_PROF_NO_SITE where the chain left it (into translated code, say). */
#define NX_PROF_NO_SITE 0xffffffffu

static inline uint64_t nx_prof_pair_key( uint32_t inner, uint32_t outer )
{
    return ((uint64_t)inner << 32) | outer;
}

enum nx_prof_kind
{
    NX_PROF_X86,     /* translated guest code */
    NX_PROF_IMAGE,   /* ARM64 PE modules: wow64.dll's thunks, ntdll.dll */
    NX_PROF_NATIVE,  /* the runtime itself: Mesa, Wine's unix side, the dynarec's helpers */
    NX_PROF_SVC,     /* in a system call, blocked or not */
    NX_PROF_KINDS
};
/* The tables of callers, after the kinds': the runtime's own for native and
 * system call samples, the x86 code those came from, and the x86 callers of
 * translated code. */
#define NX_PROF_CHAIN       NX_PROF_KINDS
#define NX_PROF_X86_CALLERS (NX_PROF_KINDS + 1)
#define NX_PROF_X86_FRAMES  (NX_PROF_KINDS + 2)
#define NX_PROF_TABLES      (NX_PROF_KINDS + 3)

static inline enum nx_prof_kind nx_prof_classify( int translated, int in_runtime, int svc )
{
    if (translated) return NX_PROF_X86;
    if (!in_runtime) return NX_PROF_IMAGE;
    return svc >= 0 ? NX_PROF_SVC : NX_PROF_NATIVE;
}

/* A system call sample, by number and by the caller of libnx's svc stub. */
static inline uint64_t nx_prof_svc_key( int svc, uint64_t caller )
{
    return ((uint64_t)svc << 40) | (caller & 0xffffffffffull);
}

#define NX_PROF_BUCKETS 512  /* a power of two */
#define NX_PROF_PROBES  64

struct nx_prof_bucket
{
    uint64_t key;
    uint32_t count;  /* 0: free */
};

struct nx_prof_table
{
    struct nx_prof_bucket buckets[NX_PROF_BUCKETS];
    uint32_t dropped;  /* samples that found neither their key nor a free bucket */
};

static inline void nx_prof_add( struct nx_prof_table *table, uint64_t key )
{
    unsigned int start = (unsigned int)((key * 0x9e3779b97f4a7c15ull) >> 55), i;

    for (i = 0; i < NX_PROF_PROBES; i++)
    {
        struct nx_prof_bucket *bucket = &table->buckets[(start + i) & (NX_PROF_BUCKETS - 1)];

        if (bucket->count && bucket->key != key) continue;
        bucket->key = key;
        bucket->count++;
        return;
    }
    table->dropped++;
}

/* The indices of up to k fullest buckets, fullest first; returns how many. */
static inline unsigned int nx_prof_top( const struct nx_prof_table *table, unsigned int *top, unsigned int k )
{
    unsigned int n = 0, i, j;

    for (i = 0; i < NX_PROF_BUCKETS; i++)
    {
        uint32_t count = table->buckets[i].count;

        if (!count) continue;
        j = n < k ? n++ : k;
        for (; j > 0 && table->buckets[top[j - 1]].count < count; j--)
            if (j < k) top[j] = top[j - 1];
        if (j < k) top[j] = i;
    }
    return n;
}

/* Thousandths of a core a thread used between two readings of its tick count. */
static inline unsigned int nx_prof_permille( uint64_t ticks, uint64_t last, uint64_t interval )
{
    if (!interval || ticks < last) return 0;
    return (unsigned int)((ticks - last) * 1000 / interval);
}

#endif
