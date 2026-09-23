/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later. */
#include <fenv.h>
#include <pthread.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __SWITCH__
#include <switch/arm/counter.h>
#endif
#include "wow64_box64_engine.h"
#include "amd64_box64_engine.h"
#include "../../dlls/winebox64/cpuid.h"
#include "box64context.h"
#include "box64cpu.h"
#include "debug.h"
#include "emit_signals.h"
#include "freq.h"
#include "my_cpuid.h"
#include "x64emu.h"
#include "x64emu_private.h"
#include "x87emu_private.h"
#include "x64_signals.h"
#ifdef WINE_NX_BOX64_DYNAREC
extern int wine_nx_box64_dynarec_init(void);
extern void wine_nx_box64_dynarec_add_stop( uintptr_t address );
extern void wine_nx_box64_dynarec_add_gate( uint32_t address );
extern void wine_nx_box64_invalidate( uintptr_t address, size_t size, int destroy );

_Static_assert( offsetof(x64emu_t, win64_teb) == 3104, "Box64 host TLS offset changed" );

static inline uint64_t current_x18(void)
{
    register uint64_t x18 __asm__("x18");
    return x18;
}
#endif

struct nx_engine
{
    x64emu_t emu;
#ifdef __SWITCH__
    jmp_buf escape;
#else
    sigjmp_buf escape;
#endif
    const struct wine_nx_wow64_gates *gates;
    const struct wine_nx_wow64_host *host;
    const struct wine_nx_amd64_host *amd64_host;
    void *opaque;
    ULONG_PTR fs_base, gs_base, completion, address_limit;
    ULONG_PTR code_page; /* page of the last checked instruction fetch; 1 = none */
    ULONGLONG remaining, executed;
    NTSTATUS status;
    pthread_mutex_t *held_mutex;
    int dynarec;    /* running under Box64's dynarec (EmuRun) rather than Run */
    int is32bits;
    I386_CONTEXT *context;          /* the run's context, which unix calls publish to */
    AMD64_CONTEXT *amd64_context;
    struct nx_engine *previous;     /* active_engine outside the run */
    unsigned int native_fpcr;       /* the host's FPCR, restored for unix calls */
    int context_replaced;           /* a unix call in EmuRun replaced the context */
};

static __thread struct nx_engine *active_engine;
/* A run's engine. Allocating and freeing one per run cost Direct3D's command
 * thread in NFSU2 ~17% of its time (a run per OpenGL call, behind malloc's
 * lock), so each thread keeps two: one for an outer run and one for a run
 * nested in a callback. A slot stays busy if its run is left by a longjmp;
 * runs then allocate, as deeper ones do. */
#define NX_CACHED_ENGINES 2
static __thread struct nx_engine cached_engines[NX_CACHED_ENGINES];
static __thread unsigned char cached_engine_busy[NX_CACHED_ENGINES];
/* Engines in use, reported by the lifecycle log. */
LONG wine_nx_box64_live_engines;
/* Throughput, logged periodically by the runtime. */
ULONGLONG wine_nx_box64_executed_total, wine_nx_box64_runs_total;

/* The first fetch from each code page goes through the checked host read;
 * later bytes on it are read directly. A page unmapped meanwhile faults inside
 * Run and unwinds through the same boundary as an operand fault. Checked reads
 * are Wine __TRY frames on Horizon and cost far more than the instruction. */
static BOOL guest_address_valid( const struct nx_engine *engine, ULONG_PTR address, SIZE_T size )
{
    return address < engine->address_limit && size <= engine->address_limit - address;
}

static NTSTATUS read_guest( struct nx_engine *engine, ULONG_PTR address, void *buffer, SIZE_T size )
{
    if (!guest_address_valid( engine, address, size )) return STATUS_ACCESS_VIOLATION;
    if (engine->is32bits) return engine->host->read( engine->opaque, address, buffer, size );
    return engine->amd64_host->read( engine->opaque, address, buffer, size );
}

static BOOL amd64_stop_address( const struct nx_engine *engine, ULONG_PTR address )
{
    return !engine->is32bits &&
           ((engine->completion && address == engine->completion) ||
            engine->amd64_host->is_native( engine->opaque, address ));
}

int wine_nx_box64_translate_allowed( uintptr_t address )
{
    struct nx_engine *engine = active_engine;
    unsigned char opcode;
    unsigned int prefix;

    if (!engine || engine->is32bits) return 1;
    if (amd64_stop_address( engine, address )) return 0;
    for (prefix = 0; prefix < 15; prefix++)
    {
        if (read_guest( engine, address + prefix, &opcode, 1 )) return 1;
        if (opcode != 0x26 && opcode != 0x2e && opcode != 0x36 && opcode != 0x3e &&
            opcode != 0x64 && opcode != 0x65 && opcode != 0x66 && opcode != 0x67 &&
            opcode != 0xf0 && opcode != 0xf2 && opcode != 0xf3 &&
            (opcode < 0x40 || opcode > 0x4f)) break;
    }
    return opcode != 0x62 && opcode != 0xc4 && opcode != 0xc5;
}

static NTSTATUS fetch_code_byte( struct nx_engine *engine, ULONG_PTR address, unsigned char *byte )
{
    NTSTATUS status;

    if ((address & ~(uintptr_t)0xfff) == engine->code_page)
    {
        *byte = *(volatile const unsigned char *)(uintptr_t)address;
        return STATUS_SUCCESS;
    }
    if ((status = read_guest( engine, address, byte, 1 ))) return status;
    engine->code_page = address & ~(uintptr_t)0xfff;
    return STATUS_SUCCESS;
}
static box64context_t core_context = { .mutex_lock = PTHREAD_MUTEX_INITIALIZER };
box64context_t *my_context = &core_context;
/* No Linux environment/loader initialization. Do not advertise optional CPU
 * features before the corresponding context and exception paths are supported. */
box64env_t box64env;
int box64_wine = 1;
int box64_is32bits = 1;
int box64_unittest_mode;
uint8_t box64_rdtsc_shift;

/* The address the last guest access violation was about and how it was
 * accessed -- 0 read, 1 write, 8 execute, as EXCEPTION_RECORD's
 * ExceptionInformation gives them -- for the exception raised into the guest. */
static __thread ULONG fault_address, fault_access;

void wine_nx_box64_last_fault( ULONG *address, ULONG *access )
{
    *address = fault_address;
    *access = fault_access;
}

static void stop_engine( x64emu_t *emu, NTSTATUS status );

static void stop_fault( x64emu_t *emu, ULONG address, ULONG access )
{
    fault_address = address;
    fault_access = access;
    stop_engine( emu, STATUS_ACCESS_VIOLATION );
}

uint32_t wine_nx_box64_guest_protection( uintptr_t address )
{
    if (!active_engine) return address <= UINT32_MAX ? 5 : 0;
    if (!guest_address_valid( active_engine, address, 1 )) return 0;
    return amd64_stop_address( active_engine, address ) ? 1 : 5;
}

#ifndef WINE_NX_BOX64_DYNAREC
uint32_t getProtection_fast( uintptr_t addr ) { return wine_nx_box64_guest_protection( addr ); }
#endif

static void stop_engine( x64emu_t *emu, NTSTATUS status )
{
    struct nx_engine *engine = (struct nx_engine *)emu;
    engine->status = status;
#ifdef __SWITCH__
    longjmp( engine->escape, 1 );
#else
    /* A host SIGSEGV/SIGBUS handler may enter this boundary. Restore its signal
     * mask as well, otherwise a second guest fault would terminate the host. */
    siglongjmp( engine->escape, 1 );
#endif
}


#ifdef WINE_NX_BOX64_DYNAREC
extern int wine_nx_box64_pc_to_x86( uintptr_t pc, uintptr_t *x86 );
extern int wine_nx_box64_is_translated_pc( uintptr_t pc );

/* Box64's adjustregs for 32-bit code: an instruction that faults part-way has
 * already moved a register -- POP to memory has popped, MOVS has advanced ESI
 * with its post-indexed load -- which Windows reports as not yet done. */
static void adjust_partial_instruction( x64emu_t *emu, ULONG_PTR pc )
{
    const unsigned char *code = (const unsigned char *)(uintptr_t)emu->ip.dword[0];
    unsigned int prefix = 0, operand16 = 0;

    while (prefix < 4 && (code[prefix] == 0xf2 || code[prefix] == 0xf3 || code[prefix] == 0x66))
        if (code[prefix++] == 0x66) operand16 = 1;
    if (code[prefix] == 0xa4 || code[prefix] == 0xa5)
    {
        uint32_t opcode = *(const uint32_t *)pc;

        /* STR (post-index) writing the byte MOVS read: undo ESI's step. */
        if ((opcode & 0x3fe00c00) == 0x38000400)
        {
            int offset = (int)(opcode << 11) >> 23;

            emu->regs[_SI].dword[0] -= offset;
        }
    }
    else if (code[prefix] == 0x8f && (code[prefix + 1] & 0xc0) != 0xc0)
        emu->regs[_SP].dword[0] -= operand16 ? 2 : 4;
}

static void recover_translated_state( x64emu_t *emu, ULONG_PTR pc, const unsigned long long *x )
{
    uintptr_t x86;
    unsigned int i;

    if (!wine_nx_box64_is_translated_pc( pc ) || !wine_nx_box64_pc_to_x86( pc, &x86 ) || x86 > 0xffffffffu)
        return;
    for (i = 0; i < 8; i++) emu->regs[i].q[0] = x[10 + i];  /* EAX ECX EDX EBX ESP EBP ESI EDI */
    emu->eflags.x64 = x[26];
    emu->df = d_none;
    emu->ip.q[0] = x86;
    adjust_partial_instruction( emu, pc );
}
#endif

BOOL wine_nx_box64_handle_fault( ULONG_PTR address, ULONG access, ULONG_PTR pc,
                                 const unsigned long long *x )
{
    if (!active_engine || !guest_address_valid( active_engine, address, 1 )) return FALSE;
#ifdef WINE_NX_BOX64_DYNAREC
    extern void *current_helper;
    extern int fillblock_active;

    /* A compiler can read past the code the guest will actually execute.
     * Enter Box64's FillBlock recovery while its jump buffer is live. The lock
     * alone is insufficient: hash validation also runs under it. */
    if (active_engine->held_mutex == &core_context.mutex_dyndump && current_helper && fillblock_active)
    {
        extern void cancelFillBlock(void);
#ifdef __SWITCH__
        extern void wine_nx_runtime_trace( const char *msg ) __attribute__((weak));
        static unsigned int reports;

        if (wine_nx_runtime_trace && __atomic_add_fetch( &reports, 1, __ATOMIC_RELAXED ) <= 8)
        {
            char msg[192];
            snprintf( msg, sizeof(msg), "[BOX64] compiler read fault at %08lx; interpreting from x86=%08x esp=%08x",
                      (unsigned long)address, (unsigned int)active_engine->emu.ip.q[0],
                      (unsigned int)active_engine->emu.regs[_SP].q[0] );
            wine_nx_runtime_trace( msg );
        }
#endif
        cancelFillBlock();
    }
    /* The guest's own fault, which its exception handlers are to see: in
     * translated code the x86 state is in the native registers, as Box64's
     * copyUCTXreg2Emu takes it, and the instruction is the one the block maps
     * the native pc to. */
    if (active_engine->is32bits && x && active_engine->dynarec)
        recover_translated_state( &active_engine->emu, pc, x );
#else
    (void)pc; (void)x;  /* the interpreter keeps each instruction's state as it goes */
#endif
    if (active_engine->is32bits) stop_fault( &active_engine->emu, address, access );
    stop_engine( &active_engine->emu, STATUS_ACCESS_VIOLATION );
    return TRUE;
}

/* Only interpreter objects redirect pthread mutex calls here. A memory XCHG
 * or LOCK instruction can fault while holding this mutex; release it back on
 * the normal stack after the fault boundary unwinds. No PE callbacks execute
 * while active_engine points at the engine holding the mutex. */
/* For [PROGRESS]: how often the dynarec's global translator lock was taken. */
unsigned int wine_nx_box64_translator_locks;

int wine_nx_box64_mutex_lock( pthread_mutex_t *mutex )
{
    int ret;

#ifdef WINE_NX_BOX64_DYNAREC
    if (mutex == &core_context.mutex_dyndump)
        __atomic_add_fetch( &wine_nx_box64_translator_locks, 1, __ATOMIC_RELAXED );
#endif
    ret = pthread_mutex_lock( mutex );
    if (!ret && active_engine) active_engine->held_mutex = mutex;
    return ret;
}

#ifdef WINE_NX_BOX64_DYNAREC
/* Whether this thread is the translator: a purge frees blocks, which only the
 * holder of Box64's translator lock may do without taking it again. */
int wine_nx_box64_holds_translator_lock(void)
{
    return active_engine && active_engine->held_mutex == &core_context.mutex_dyndump;
}
#endif

int wine_nx_box64_mutex_unlock( pthread_mutex_t *mutex )
{
    int ret = pthread_mutex_unlock( mutex );
    if (!ret && active_engine && active_engine->held_mutex == mutex)
        active_engine->held_mutex = NULL;
    return ret;
}

/* Hook added to a generated copy of the pinned interpreter. Called before each
 * main instruction, including before any guest gate bytes are fetched. */
int wine_nx_box64_before_instruction( x64emu_t *emu, uintptr_t pc )
{
    struct nx_engine *engine = (struct nx_engine *)emu;
    unsigned char opcode = 0;
    unsigned int prefix;
    NTSTATUS status;
    emu->ip.q[0] = pc;
    if ((engine->is32bits && emu->segs[_CS] != 0x23) ||
        (!engine->is32bits && emu->segs[_CS] != 0x33))
        stop_engine( emu, STATUS_NOT_SUPPORTED );
    if (!guest_address_valid( engine, pc, 1 ))
    {
        if (engine->is32bits) stop_fault( emu, pc > UINT32_MAX ? UINT32_MAX : pc, 8 );
        stop_engine( emu, STATUS_ACCESS_VIOLATION );
    }
    if ((engine->is32bits &&
         (pc == engine->gates->syscall || pc == engine->gates->unix_call ||
          (engine->completion && pc == engine->completion))) || amd64_stop_address( engine, pc ))
    {
        /* Box64's EmuRun would only ask for the next block again: end the run. */
        if (engine->dynarec) stop_engine( emu, STATUS_SUCCESS );
        return 1;
    }
    if (!engine->remaining) stop_engine( emu, STATUS_TIMEOUT );
    CheckExec( emu, pc );
    /* Reject state families not represented by this initial adapter before
     * executing them, including when an instruction has legacy prefixes. */
    for (prefix = 0; prefix < 15; ++prefix)
    {
        if (!guest_address_valid( engine, pc, prefix + 1 ))
        {
            if (engine->is32bits)
                stop_fault( emu, pc + prefix > UINT32_MAX ? UINT32_MAX : pc + prefix, 8 );
            stop_engine( emu, STATUS_ACCESS_VIOLATION );
        }
        status = fetch_code_byte( engine, pc + prefix, &opcode );
        if (status == STATUS_ACCESS_VIOLATION) stop_fault( emu, pc + prefix, 8 );
        if (status) stop_engine( emu, status );
        if (opcode != 0x26 && opcode != 0x2e && opcode != 0x36 && opcode != 0x3e &&
            opcode != 0x64 && opcode != 0x65 && opcode != 0x66 && opcode != 0x67 &&
            opcode != 0xf0 && opcode != 0xf2 && opcode != 0xf3 &&
            (engine->is32bits || opcode < 0x40 || opcode > 0x4f)) break;
    }
    if (prefix == 15) stop_engine( emu, STATUS_ILLEGAL_INSTRUCTION );
    /* VEX (AVX) is not advertised; LES/LDS share these opcodes in 32-bit mode. */
    if (opcode == 0xc4 || opcode == 0xc5 || (!engine->is32bits && opcode == 0x62))
        stop_engine( emu, STATUS_NOT_SUPPORTED );
    --engine->remaining;
    ++engine->executed;
    return 0;
}

void CheckExec( x64emu_t *emu, uintptr_t pc )
{
    struct nx_engine *engine = (struct nx_engine *)emu;
    unsigned char byte;
    NTSTATUS status;
    if (!guest_address_valid( engine, pc, 1 ))
    {
        if (engine->is32bits) stop_fault( emu, pc > UINT32_MAX ? UINT32_MAX : pc, 8 );
        stop_engine( emu, STATUS_ACCESS_VIOLATION );
    }
    if ((engine->is32bits &&
         (pc == engine->gates->syscall || pc == engine->gates->unix_call ||
          (engine->completion && pc == engine->completion))) || amd64_stop_address( engine, pc )) return;
    status = fetch_code_byte( engine, pc, &byte );
    if (status == STATUS_ACCESS_VIOLATION) stop_fault( emu, pc, 8 );
    if (status) stop_engine( emu, status );
}

void EmitSignal( x64emu_t *emu, int sig, void *addr, int code )
{
    (void)addr; (void)code;
    stop_engine( emu, sig == X64_SIGTRAP ? STATUS_BREAKPOINT :
                      sig == X64_SIGSEGV ? STATUS_ACCESS_VIOLATION : STATUS_ILLEGAL_INSTRUCTION );
}
void EmitDiv0( x64emu_t *emu, void *addr, int code )
{
    (void)addr; (void)code; stop_engine( emu, STATUS_INTEGER_DIVIDE_BY_ZERO );
}
void EmitInterruption( x64emu_t *emu, int num, void *addr )
{
    (void)num; (void)addr; stop_engine( emu, STATUS_ILLEGAL_INSTRUCTION );
}
void EmuX64Syscall( void *emu )
{
    struct nx_engine *engine = emu;
    stop_engine( emu, engine->is32bits ? STATUS_NOT_SUPPORTED : STATUS_EMULATION_SYSCALL );
}
void EmuX86Syscall( void *emu ) { stop_engine( emu, STATUS_NOT_SUPPORTED ); }
void EmuInt3( void *emu, void *addr ) { (void)addr; stop_engine( emu, STATUS_BREAKPOINT ); }
void *EmuFork( void *emu, int type )
{
    (void)type; stop_engine( emu, STATUS_NOT_SUPPORTED ); return NULL;
}
void *getAlternate( void *address ) { return address; }
uintptr_t getAlternateJump( void *address, int is32bits )
{
    (void)address; (void)is32bits; return 0;
}
void *getAlternateData( void *address ) { (void)address; return (void *)-1LL; }
void setAlternateData( void *address, void *data ) { (void)address; (void)data; }
int GetTID(void) { return 0; } /* interpreter diagnostics only */
void PrintfFtrace( int prefix, const char *format, ... )
{
    va_list args;
    (void)prefix;
    va_start( args, format ); vfprintf( stderr, format, args ); va_end( args );
}
void *GetSegmentBase( void *opaque, uint32_t selector )
{
    struct nx_engine *engine = opaque;
    if (engine->is32bits && selector == engine->emu.segs[_FS]) return (void *)engine->fs_base;
    if (!engine->is32bits && selector == engine->emu.segs[_GS]) return (void *)engine->gs_base;
    if (selector == engine->emu.segs[_FS]) return (void *)engine->fs_base;
    if (selector == engine->emu.segs[_GS]) return (void *)engine->gs_base;
    stop_engine( opaque, STATUS_NOT_SUPPORTED );
    return NULL;
}
void *GetSeg43Base( void *emu ) { stop_engine( emu, STATUS_NOT_SUPPORTED ); return NULL; }

/* The CPU identity is shared with winebox64 (dlls/winebox64/cpuid.h), so CPUID,
 * IsProcessorFeaturePresent and GetSystemInfo describe the same processor. */
void my_cpuid( x64emu_t *emu )
{
    struct nx_engine *engine = (struct nx_engine *)emu;
    static const char vendor[12] = {'G','e','n','u','i','n','e','I','n','t','e','l'};
    static const char brand32[48] = "Wine-NX Box64 i386 interpreter";
    static const char brand64[48] = "Wine-NX Box64 AMD64 engine";
    const char *brand = engine->is32bits ? brand32 : brand64;
    uint32_t regs[4] = {0}; /* eax, ebx, ecx, edx */
    uint32_t leaf = emu->regs[_AX].dword[0];

    switch (leaf)
    {
    case 0:
        regs[0] = 1;
        memcpy( &regs[1], vendor, 4 );
        memcpy( &regs[3], vendor + 4, 4 );
        memcpy( &regs[2], vendor + 8, 4 );
        break;
    case 1:
        regs[0] = WINEBOX64_CPUID_SIGNATURE;
        regs[1] = (1u << 16) | (8u << 8); /* one logical CPU, 64-byte CLFLUSH line */
        regs[3] = WINEBOX64_CPUID_EDX;
        break;
    case 0x80000000:
        regs[0] = 0x80000004;
        break;
    case 0x80000001:
        if (!engine->is32bits) regs[3] = 1u << 29;
        break;
    case 0x80000002: case 0x80000003: case 0x80000004:
        memcpy( regs, brand + (leaf - 0x80000002) * 16, 16 );
        break;
    default:
        break;
    }
    emu->regs[_AX].q[0] = regs[0];
    emu->regs[_BX].q[0] = regs[1];
    emu->regs[_CX].q[0] = regs[2];
    emu->regs[_DX].q[0] = regs[3];
}
uint32_t helper_getcpu( x64emu_t *emu ) { stop_engine( emu, STATUS_NOT_SUPPORTED ); return 0; }

ULONGLONG wine_nx_box64_tsc_reads;

/* A monotonic nanosecond count; CPUID advertises TSC. */
uint64_t ReadTSC( x64emu_t *emu )
{
    (void)emu;
    __atomic_add_fetch( &wine_nx_box64_tsc_reads, 1, __ATOMIC_RELAXED );
#ifdef __SWITCH__
    return armTicksToNs( armGetSystemTick() );
#else
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
#endif
}
uint32_t get_random32(void) { stop_engine( &active_engine->emu, STATUS_NOT_SUPPORTED ); return 0; }
uint64_t get_random64(void) { stop_engine( &active_engine->emu, STATUS_NOT_SUPPORTED ); return 0; }

/* x87, MMX and SSE state crosses runs in the context's FXSAVE area, in the
 * layout Box64 also uses for the guest's own FXSAVE/FXRSTOR instructions:
 * registers as doubles, tags relative to TOP. The FNSAVE area (FloatSave) gets
 * an architectural image with 80-bit registers for other readers; it is not
 * read back. */
static void import_fpu( x64emu_t *emu, const I386_CONTEXT *ctx )
{
    XMM_SAVE_AREA32 fx;
    unsigned int depth = 0;

    memcpy( &fx, ctx->ExtendedRegisters, sizeof(fx) );
    fpu_fxrstor32( emu, &fx );
    /* FXRSTOR leaves Box64's push counter alone; FXAM consults it. */
    while (depth < 8 && !((emu->fpu_tags >> (2 * depth)) & 3)) depth++;
    emu->fpu_stack = depth;
}

static void export_fpu( x64emu_t *emu, I386_CONTEXT *ctx )
{
    XMM_SAVE_AREA32 fx;
    unsigned int i, top = emu->top & 7, tags = 0;

    memcpy( &fx, ctx->ExtendedRegisters, sizeof(fx) );
    fpu_fxsave32( emu, &fx );
    memcpy( ctx->ExtendedRegisters, &fx, sizeof(fx) );

    ctx->FloatSave.ControlWord = emu->cw.x16;
    ctx->FloatSave.StatusWord = emu->sw.x16; /* fxsave32 stored TOP in it */
    ctx->FloatSave.ErrorOffset = ctx->FloatSave.ErrorSelector = 0;
    ctx->FloatSave.DataOffset = ctx->FloatSave.DataSelector = 0;
    memset( ctx->FloatSave.RegisterArea, 0, sizeof(ctx->FloatSave.RegisterArea) );
    for (i = 0; i < 8; i++)
    {
        unsigned int empty = (emu->fpu_tags >> (2 * i)) & 3;
        /* FNSAVE tags are indexed by physical register, ST(i) = R((TOP + i) & 7). */
        tags |= (empty ? 3u : 0u) << (2 * ((top + i) & 7));
        if (!empty) D2LD( &emu->x87[(top + i) & 7].d, &ctx->FloatSave.RegisterArea[i * 10] );
    }
    ctx->FloatSave.TagWord = tags;
}

static NTSTATUS import_context( struct nx_engine *engine, const I386_CONTEXT *ctx )
{
    x64emu_t *emu = &engine->emu;
    /* Only the low 16 bits of a selector field are meaningful. */
    if ((WORD)ctx->SegCs != 0x23 || (ctx->ContextFlags & CONTEXT_I386_XSTATE) == CONTEXT_I386_XSTATE)
        return STATUS_NOT_SUPPORTED;
    emu->regs[_AX].q[0] = ctx->Eax; emu->regs[_BX].q[0] = ctx->Ebx;
    emu->regs[_CX].q[0] = ctx->Ecx; emu->regs[_DX].q[0] = ctx->Edx;
    emu->regs[_SI].q[0] = ctx->Esi; emu->regs[_DI].q[0] = ctx->Edi;
    emu->regs[_SP].q[0] = ctx->Esp; emu->regs[_BP].q[0] = ctx->Ebp;
    emu->ip.q[0] = ctx->Eip; emu->eflags.x64 = ctx->EFlags;
    emu->df = d_none;
    emu->segs[_CS] = (WORD)ctx->SegCs; emu->segs[_SS] = (WORD)ctx->SegSs;
    emu->segs[_DS] = (WORD)ctx->SegDs; emu->segs[_ES] = (WORD)ctx->SegEs;
    emu->segs[_FS] = (WORD)ctx->SegFs; emu->segs[_GS] = (WORD)ctx->SegGs;
    emu->segs_offs[_FS] = engine->fs_base;
    import_fpu( emu, ctx );
    return STATUS_SUCCESS;
}

static NTSTATUS export_context( struct nx_engine *engine, I386_CONTEXT *ctx )
{
    x64emu_t *emu = &engine->emu;
    UpdateFlags( emu );
    ctx->Eax = emu->regs[_AX].dword[0]; ctx->Ebx = emu->regs[_BX].dword[0];
    ctx->Ecx = emu->regs[_CX].dword[0]; ctx->Edx = emu->regs[_DX].dword[0];
    ctx->Esi = emu->regs[_SI].dword[0]; ctx->Edi = emu->regs[_DI].dword[0];
    ctx->Esp = emu->regs[_SP].dword[0]; ctx->Ebp = emu->regs[_BP].dword[0];
    ctx->Eip = emu->ip.dword[0]; ctx->EFlags = emu->eflags.x64;
    ctx->SegCs = emu->segs[_CS]; ctx->SegSs = emu->segs[_SS];
    ctx->SegDs = emu->segs[_DS]; ctx->SegEs = emu->segs[_ES];
    ctx->SegFs = emu->segs[_FS]; ctx->SegGs = emu->segs[_GS];
    export_fpu( emu, ctx );
    return STATUS_SUCCESS;
}

static void import_fpu_amd64( x64emu_t *emu, const AMD64_CONTEXT *ctx )
{
    unsigned int logical, top = (ctx->FltSave.StatusWord >> 11) & 7;

    emu->cw.x16 = ctx->FltSave.ControlWord;
    emu->sw.x16 = ctx->FltSave.StatusWord;
    emu->top = top;
    emu->fpu_tags = TAGS_EMPTY;
    emu->fpu_stack = 0;
    emu->mxcsr.x32 = ctx->MxCsr;
    for (logical = 0; logical < 8; logical++)
    {
        unsigned int physical = (top + logical) & 7;

        if (!(ctx->FltSave.TagWord & (1u << physical))) continue;
        emu->fpu_tags &= ~(UINT64_C(3) << (logical * 2));
        LD2D( (void *)&ctx->FltSave.FloatRegisters[logical], &emu->x87[physical].d );
        fpu_ld80_clear( emu, logical );
        emu->fpu_stack++;
    }
    memcpy( emu->xmm, ctx->FltSave.XmmRegisters, sizeof(emu->xmm) );
}

static void export_fpu_amd64( x64emu_t *emu, AMD64_CONTEXT *ctx )
{
    unsigned int logical, top = emu->top & 7;
    BYTE tags = 0;

    emu->sw.f.F87_TOP = top;
    ctx->FltSave.ControlWord = emu->cw.x16;
    ctx->FltSave.StatusWord = emu->sw.x16;
    ctx->FltSave.MxCsr = ctx->MxCsr = emu->mxcsr.x32;
    for (logical = 0; logical < 8; logical++)
    {
        unsigned int physical;

        if ((emu->fpu_tags >> (logical * 2)) & 3) continue;
        physical = (top + logical) & 7;
        tags |= 1u << physical;
        D2LD( &emu->x87[physical].d, &ctx->FltSave.FloatRegisters[logical] );
    }
    ctx->FltSave.TagWord = tags;
    memcpy( ctx->FltSave.XmmRegisters, emu->xmm, sizeof(emu->xmm) );
}

static NTSTATUS import_context_amd64( struct nx_engine *engine, const AMD64_CONTEXT *ctx )
{
    x64emu_t *emu = &engine->emu;

    if ((WORD)ctx->SegCs != 0x33 ||
        (ctx->ContextFlags & CONTEXT_AMD64_XSTATE) == CONTEXT_AMD64_XSTATE)
        return STATUS_NOT_SUPPORTED;
    emu->regs[_AX].q[0] = ctx->Rax; emu->regs[_BX].q[0] = ctx->Rbx;
    emu->regs[_CX].q[0] = ctx->Rcx; emu->regs[_DX].q[0] = ctx->Rdx;
    emu->regs[_SI].q[0] = ctx->Rsi; emu->regs[_DI].q[0] = ctx->Rdi;
    emu->regs[_SP].q[0] = ctx->Rsp; emu->regs[_BP].q[0] = ctx->Rbp;
    emu->regs[_R8].q[0] = ctx->R8; emu->regs[_R9].q[0] = ctx->R9;
    emu->regs[_R10].q[0] = ctx->R10; emu->regs[_R11].q[0] = ctx->R11;
    emu->regs[_R12].q[0] = ctx->R12; emu->regs[_R13].q[0] = ctx->R13;
    emu->regs[_R14].q[0] = ctx->R14; emu->regs[_R15].q[0] = ctx->R15;
    emu->ip.q[0] = ctx->Rip; emu->eflags.x64 = ctx->EFlags;
    emu->df = d_none;
    emu->segs[_CS] = ctx->SegCs; emu->segs[_SS] = ctx->SegSs;
    emu->segs[_DS] = ctx->SegDs; emu->segs[_ES] = ctx->SegEs;
    emu->segs[_FS] = ctx->SegFs; emu->segs[_GS] = ctx->SegGs;
    emu->segs_offs[_FS] = engine->fs_base;
    emu->segs_offs[_GS] = engine->gs_base;
    import_fpu_amd64( emu, ctx );
    return STATUS_SUCCESS;
}

static NTSTATUS export_context_amd64( struct nx_engine *engine, AMD64_CONTEXT *ctx )
{
    x64emu_t *emu = &engine->emu;

    UpdateFlags( emu );
    ctx->Rax = emu->regs[_AX].q[0]; ctx->Rbx = emu->regs[_BX].q[0];
    ctx->Rcx = emu->regs[_CX].q[0]; ctx->Rdx = emu->regs[_DX].q[0];
    ctx->Rsi = emu->regs[_SI].q[0]; ctx->Rdi = emu->regs[_DI].q[0];
    ctx->Rsp = emu->regs[_SP].q[0]; ctx->Rbp = emu->regs[_BP].q[0];
    ctx->R8 = emu->regs[_R8].q[0]; ctx->R9 = emu->regs[_R9].q[0];
    ctx->R10 = emu->regs[_R10].q[0]; ctx->R11 = emu->regs[_R11].q[0];
    ctx->R12 = emu->regs[_R12].q[0]; ctx->R13 = emu->regs[_R13].q[0];
    ctx->R14 = emu->regs[_R14].q[0]; ctx->R15 = emu->regs[_R15].q[0];
    ctx->Rip = emu->ip.q[0]; ctx->EFlags = emu->eflags.x64;
    ctx->SegCs = emu->segs[_CS]; ctx->SegSs = emu->segs[_SS];
    ctx->SegDs = emu->segs[_DS]; ctx->SegEs = emu->segs[_ES];
    ctx->SegFs = emu->segs[_FS]; ctx->SegGs = emu->segs[_GS];
    export_fpu_amd64( emu, ctx );
    return STATUS_SUCCESS;
}

#ifdef WINE_NX_BOX64_DYNAREC
/* For [PROGRESS]: unix calls made without leaving Box64's EmuRun. */
unsigned int wine_nx_box64_inline_unix_calls;

static void get_integer_state( const I386_CONTEXT *ctx, ULONG state[10] )
{
    state[0] = ctx->Eax; state[1] = ctx->Ebx; state[2] = ctx->Ecx; state[3] = ctx->Edx;
    state[4] = ctx->Esi; state[5] = ctx->Edi; state[6] = ctx->Ebp; state[7] = ctx->Esp;
    state[8] = ctx->Eip; state[9] = ctx->EFlags;
}

/* A unix call from translated code, made in EmuRun. Leaving the run for it
 * (export_context, wine_nx_wow64_dispatch_gate, import_context) copied the FPU
 * state twice and switched fenv and EmuRun for each of ~250,000 OpenGL calls a
 * second from Direct3D's drawing thread in NFSU2, about 13% of that thread.
 * As the dispatch does, the continuation is published first, so a callback or
 * NtContinue during the call sees the live integer state and control words;
 * the x87 stack and XMM registers stay in the emulator (the i386 ABI leaves the
 * x87 stack empty and XMM registers unpreserved across a call). A call that
 * changed the context goes on from its integer state; one that replaced it is
 * imported whole by the run loop. FALSE leaves the gate to the loop. */
static BOOL inline_unix_call( struct nx_engine *engine )
{
    x64emu_t *emu = &engine->emu;
    I386_CONTEXT *ctx = engine->context;
    ULONG esp = emu->regs[_SP].dword[0], stack[5], published[10], now[10];
    WORD cw = emu->cw.x16;
    unsigned int fpcr;
    NTSTATUS status;

    if (esp > 0xffffffffu - 20u) return FALSE;
    /* The call to the gate has just stored these. */
    memcpy( stack, (void *)(uintptr_t)esp, sizeof(stack) );
    UpdateFlags( emu );
    ctx->Eax = emu->regs[_AX].dword[0]; ctx->Ebx = emu->regs[_BX].dword[0];
    ctx->Ecx = emu->regs[_CX].dword[0]; ctx->Edx = emu->regs[_DX].dword[0];
    ctx->Esi = emu->regs[_SI].dword[0]; ctx->Edi = emu->regs[_DI].dword[0];
    ctx->Ebp = emu->regs[_BP].dword[0]; ctx->EFlags = emu->eflags.x64;
    ctx->Eip = stack[0]; ctx->Esp = esp + 20;
    ctx->FloatSave.ControlWord = cw;
    memcpy( ctx->ExtendedRegisters + offsetof( XMM_SAVE_AREA32, ControlWord ), &cw, sizeof(cw) );
    memcpy( ctx->ExtendedRegisters + offsetof( XMM_SAVE_AREA32, MxCsr ), &emu->mxcsr.x32, sizeof(emu->mxcsr.x32) );
    get_integer_state( ctx, published );

    /* As outside the run: faults are not the guest's, the FPU is the host's. */
    active_engine = engine->previous;
    fpcr = __builtin_aarch64_get_fpcr();
    if (fpcr != engine->native_fpcr) __builtin_aarch64_set_fpcr( engine->native_fpcr );
    status = engine->host->unix_call( engine->opaque, (ULONGLONG)stack[1] | ((ULONGLONG)stack[2] << 32),
                                      stack[3], stack[4] );
    if (fpcr != engine->native_fpcr) __builtin_aarch64_set_fpcr( fpcr );
    active_engine = engine;
    __atomic_add_fetch( &wine_nx_box64_inline_unix_calls, 1, __ATOMIC_RELAXED );

    if (engine->host->context_replaced && engine->host->context_replaced( engine->opaque ))
    {
        /* As wow64cpu's unix_call_32to64: the result in Eax, the rest as replaced. */
        ctx->Eax = status;
        engine->context_replaced = 1;
        return FALSE;
    }
    get_integer_state( ctx, now );
    if (memcmp( now, published, sizeof(now) ))
    {
        emu->regs[_BX].q[0] = ctx->Ebx; emu->regs[_CX].q[0] = ctx->Ecx;
        emu->regs[_DX].q[0] = ctx->Edx; emu->regs[_SI].q[0] = ctx->Esi;
        emu->regs[_DI].q[0] = ctx->Edi; emu->regs[_BP].q[0] = ctx->Ebp;
        emu->regs[_SP].q[0] = ctx->Esp; emu->ip.q[0] = ctx->Eip;
        emu->eflags.x64 = ctx->EFlags;
    }
    else
    {
        emu->regs[_SP].q[0] = esp + 20;
        emu->ip.q[0] = stack[0];
    }
    ctx->Eax = status;
    emu->regs[_AX].q[0] = status;
    return TRUE;
}

/* Called by Box64's EmuRun (Box64Core.cmake) before it looks up a block. A
 * unix call the host takes is made there; other gates and the completion
 * address end the run, as the interpreter hook would, without a failed lookup
 * and an interpreter start first. */
int wine_nx_box64_stop_at( x64emu_t *emu, uintptr_t pc )
{
    struct nx_engine *engine = (struct nx_engine *)emu;

    if (!engine->dynarec) return 0;
    if (!engine->is32bits) return amd64_stop_address( engine, pc );
    if (pc == engine->gates->unix_call && engine->host->unix_call) return !inline_unix_call( engine );
    return pc == engine->gates->syscall || pc == engine->gates->unix_call ||
           (engine->completion && pc == engine->completion);
}

int wine_nx_box64_link_stop_at( x64emu_t *emu, uintptr_t pc )
{
    struct nx_engine *engine = (struct nx_engine *)emu;

    return engine->dynarec && amd64_stop_address( engine, pc );
}
#endif

NTSTATUS wine_nx_box64_run( I386_CONTEXT *context, ULONG fs_base,
                          const struct wine_nx_wow64_gates *gates,
                          const struct wine_nx_wow64_host *host, void *opaque,
                          ULONG completion_pc, ULONGLONG budget, ULONGLONG *executed )
{
    static uint32_t parity[8] = {0x96696996,0x69969669,0x69969669,0x96696996,
                                0x69969669,0x96696996,0x96696996,0x69969669};
    struct nx_engine *engine, *previous = active_engine;
    NTSTATUS status;
    fenv_t native_fenv;
    unsigned int slot;
    int i;
#ifdef WINE_NX_BOX64_DYNAREC
    int use_dynarec;
#endif
    if (executed) *executed = 0;
#ifdef WINE_NX_BOX64_DYNAREC
    {
        /* Every return from a gate moves the dynarec's purge clock on. */
        extern void wine_nx_box64_purge_clock( void );
        wine_nx_box64_purge_clock();
    }
#endif
    if (!context || !gates || !host || !host->read || !gates->syscall ||
        !gates->unix_call || gates->syscall == gates->unix_call || !budget ||
        completion_pc == gates->syscall || completion_pc == gates->unix_call)
        return STATUS_INVALID_PARAMETER;
    for (slot = 0; slot < NX_CACHED_ENGINES && cached_engine_busy[slot]; slot++) continue;
    if (slot < NX_CACHED_ENGINES)
    {
        cached_engine_busy[slot] = 1;
        engine = &cached_engines[slot];
        /* As calloc did, but Box64's scratch area is only scratch: 1.6 of the
         * 4 KB zeroed on every run (memset was 6% of the command thread). */
        memset( engine, 0, offsetof( struct nx_engine, emu.scratch ) );
        memset( &engine->emu.scratch[N_SCRATCH], 0,
                sizeof(*engine) - offsetof( struct nx_engine, emu.scratch[N_SCRATCH] ) );
    }
    else if (!(engine = calloc( 1, sizeof(*engine) ))) return STATUS_NO_MEMORY;
    __atomic_add_fetch( &wine_nx_box64_live_engines, 1, __ATOMIC_RELAXED );
    engine->gates = gates; engine->host = host; engine->opaque = opaque;
    engine->context = context; engine->previous = previous;
    engine->is32bits = 1; engine->address_limit = UINT64_C(0x100000000);
    engine->fs_base = fs_base; engine->completion = completion_pc; engine->remaining = budget;
    engine->code_page = 1;
    engine->emu.context = &core_context;
    engine->emu.x64emu_parity_tab = parity;
    for (i = 0; i < 16; ++i) engine->emu.sbiidx[i] = &engine->emu.regs[i];
    engine->emu.sbiidx[4] = &engine->emu.zero;
    reset_fpu( &engine->emu );
#ifdef WINE_NX_BOX64_DYNAREC
    use_dynarec = wine_nx_box64_dynarec_init();
    /* Gates hold INT3 sentinels; the dynarec must leave them to the hook. */
    wine_nx_box64_dynarec_add_gate( gates->syscall );
    wine_nx_box64_dynarec_add_gate( gates->unix_call );
    wine_nx_box64_dynarec_add_stop( completion_pc );
#endif
    for (;;)
    {
        status = import_context( engine, context );
        if (status) break;
#ifdef WINE_NX_BOX64_DYNAREC
        engine->emu.win64_teb = current_x18();
#endif
        fegetenv( &native_fenv );
#ifdef WINE_NX_BOX64_DYNAREC
        engine->native_fpcr = __builtin_aarch64_get_fpcr();
#endif
        active_engine = engine;
#ifdef __SWITCH__
        if (!setjmp( engine->escape ))
#else
        if (!sigsetjmp( engine->escape, 1 ))
#endif
        {
#ifdef WINE_NX_BOX64_DYNAREC
            engine->dynarec = use_dynarec;
            if (use_dynarec) DynaRun( &engine->emu );
            else
#endif
            Run( &engine->emu, 0 );
        }
        active_engine = previous;
        if (engine->held_mutex)
        {
            pthread_mutex_unlock( engine->held_mutex );
            engine->held_mutex = NULL;
        }
        fesetenv( &native_fenv );
#ifdef WINE_NX_BOX64_DYNAREC
        if (engine->context_replaced)
        {
            /* The replaced context is the state to run from, Eax included. */
            engine->context_replaced = 0;
            continue;
        }
#endif
        status = export_context( engine, context );
        if (engine->status) status = engine->status;
        if (status || (completion_pc && context->Eip == completion_pc)) break;
        /* A gate the host has no callback for goes back across the PE/Unix
         * boundary: run_guest takes unix calls here and leaves system calls,
         * which need wow64.dll, to BTCpuSimulate. */
        if (!(context->Eip == gates->unix_call ? host->unix_call != NULL
                                               : context->Eip == gates->syscall && host->syscall != NULL))
            break;
        status = wine_nx_wow64_dispatch_gate( context, gates, host, opaque );
        if (status) break;
    }
    if (executed) *executed = engine->executed;
    __atomic_add_fetch( &wine_nx_box64_executed_total, engine->executed, __ATOMIC_RELAXED );
    __atomic_add_fetch( &wine_nx_box64_runs_total, 1, __ATOMIC_RELAXED );
    if (slot < NX_CACHED_ENGINES) cached_engine_busy[slot] = 0;
    else free( engine );
    __atomic_sub_fetch( &wine_nx_box64_live_engines, 1, __ATOMIC_RELAXED );
    return status;
}

NTSTATUS wine_nx_box64_run_amd64( AMD64_CONTEXT *context, ULONG_PTR gs_base,
                                 struct wine_nx_amd64_state *state,
                                 const struct wine_nx_amd64_host *host, void *opaque,
                                 ULONG_PTR completion, ULONGLONG budget, ULONGLONG *executed )
{
    static uint32_t parity[8] = {0x96696996,0x69969669,0x69969669,0x96696996,
                                0x69969669,0x96696996,0x96696996,0x69969669};
    struct nx_engine *engine, *previous = active_engine;
    NTSTATUS status;
    fenv_t native_fenv;
    unsigned int slot;
    int i;
#ifdef WINE_NX_BOX64_DYNAREC
    int use_dynarec;
#endif

    if (executed) *executed = 0;
    if (!context || !state || !host || !host->read || !host->is_native ||
        !host->address_limit || !budget ||
        gs_base >= host->address_limit || (completion && completion >= host->address_limit))
        return STATUS_INVALID_PARAMETER;
    for (slot = 0; slot < NX_CACHED_ENGINES && cached_engine_busy[slot]; slot++) continue;
    if (slot < NX_CACHED_ENGINES)
    {
        cached_engine_busy[slot] = 1;
        engine = &cached_engines[slot];
        memset( engine, 0, offsetof( struct nx_engine, emu.scratch ) );
        memset( &engine->emu.scratch[N_SCRATCH], 0,
                sizeof(*engine) - offsetof( struct nx_engine, emu.scratch[N_SCRATCH] ) );
    }
    else if (!(engine = calloc( 1, sizeof(*engine) ))) return STATUS_NO_MEMORY;
    __atomic_add_fetch( &wine_nx_box64_live_engines, 1, __ATOMIC_RELAXED );
    engine->amd64_host = host; engine->opaque = opaque; engine->amd64_context = context;
    engine->previous = previous; engine->gs_base = gs_base; engine->completion = completion;
    engine->address_limit = host->address_limit; engine->remaining = budget; engine->code_page = 1;
    engine->emu.context = &core_context;
    engine->emu.x64emu_parity_tab = parity;
    for (i = 0; i < 16; ++i) engine->emu.sbiidx[i] = &engine->emu.regs[i];
    engine->emu.sbiidx[4] = &engine->emu.zero;
    reset_fpu( &engine->emu );
#ifdef WINE_NX_BOX64_DYNAREC
    use_dynarec = wine_nx_box64_dynarec_init();
    if (completion) wine_nx_box64_invalidate( completion, 1, 1 );
#endif
    status = import_context_amd64( engine, context );
    memcpy( engine->emu.mmx, state->mmx, sizeof(state->mmx) );
    if (!status)
    {
#ifdef WINE_NX_BOX64_DYNAREC
        engine->emu.win64_teb = current_x18();
#endif
        fegetenv( &native_fenv );
#ifdef WINE_NX_BOX64_DYNAREC
        engine->native_fpcr = __builtin_aarch64_get_fpcr();
#endif
        active_engine = engine;
#ifdef __SWITCH__
        if (!setjmp( engine->escape ))
#else
        if (!sigsetjmp( engine->escape, 1 ))
#endif
        {
#ifdef WINE_NX_BOX64_DYNAREC
            engine->dynarec = use_dynarec;
            if (use_dynarec) DynaRun( &engine->emu );
            else
#endif
            Run( &engine->emu, 0 );
        }
        active_engine = previous;
        if (engine->held_mutex)
        {
            pthread_mutex_unlock( engine->held_mutex );
            engine->held_mutex = NULL;
        }
        fesetenv( &native_fenv );
        status = export_context_amd64( engine, context );
        memcpy( state->mmx, engine->emu.mmx, sizeof(state->mmx) );
        if (engine->status) status = engine->status;
    }
    if (executed) *executed = engine->executed;
    __atomic_add_fetch( &wine_nx_box64_executed_total, engine->executed, __ATOMIC_RELAXED );
    __atomic_add_fetch( &wine_nx_box64_runs_total, 1, __ATOMIC_RELAXED );
    if (slot < NX_CACHED_ENGINES) cached_engine_busy[slot] = 0;
    else free( engine );
    __atomic_sub_fetch( &wine_nx_box64_live_engines, 1, __ATOMIC_RELAXED );
    return status;
}
