#!/usr/bin/env python3
"""Take a context of this thread and continue it, with the two halves as they
are in dlls/ntdll/unix/horizon.c: horizon_capture_context and
horizon_continue_context, through libnx's exception restore.

A user APC leaves an alertable wait through this pair. wow64 gives the
program's completion routine a simulation of its own and leaves it by
continuing the context the wait was taken at, so anything the restore puts back
and the capture leaves out -- the callee-saved q registers above all -- comes
back as whatever was in the context's memory."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
horizon = (root / 'dlls/ntdll/unix/horizon.c').read_text()

def function(source, name):
    start = source.index(name)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

def macro(source, name):
    start = source.index('__ASM_GLOBAL_FUNC( ' + name)
    depth = 0
    end = source.index('(', start)
    while True:
        depth += (source[end] == '(') - (source[end] == ')')
        end += 1
        if depth == 0:
            break
    return source[start:end]

capture = macro(horizon, 'horizon_capture_context')
restore = function(horizon, 'static void horizon_restore_exception_context( ThreadExceptionDump *ctx )\n{')
continue_ = function(horizon, 'void horizon_continue_context( const CONTEXT *context )')

# Every register the restore puts back has to be in the capture.
for reg in ('q8,  q9', 'q10, q11', 'q12, q13', 'q14, q15', 'x19', 'x28'):
    assert reg.split(',')[0].strip() in capture, reg

fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WINAPI
#define C_ASSERT(x) _Static_assert((x), #x)
#define offsetof(t, f) __builtin_offsetof(t, f)
#define CONTEXT_ARM64_FULL 0x400007
/* A C name in assembly: with an underscore on macOS, as it is on Linux. */
#ifdef __APPLE__
#define ASM_NAME(name) "_" #name
#else
#define ASM_NAME(name) #name
#endif
#define __ASM_GLOBAL_FUNC(name, code) \
    __asm__( ".text\n.globl " ASM_NAME(name) "\n.p2align 2\n" ASM_NAME(name) ":\n" code );

typedef struct { uint64_t low, high; } NEON128;
typedef struct
{
    unsigned int ContextFlags;   /* 0x000 */
    unsigned int Cpsr;           /* 0x004 */
    union { uint64_t X[29]; struct { uint64_t X0_17[18], X18; }; };  /* 0x008; X18 is X[18], as in winnt.h */
    uint64_t Fp;                 /* 0x0f0 */
    uint64_t Lr;                 /* 0x0f8 */
    uint64_t Sp;                 /* 0x100 */
    uint64_t Pc;                 /* 0x108 */
    NEON128  V[32];              /* 0x110 */
    unsigned int Fpcr;           /* 0x310 */
    unsigned int Fpsr;           /* 0x314 */
    unsigned char rest[0x100];
} CONTEXT;
#define X0 X[0]
#define ContextFlags_ ContextFlags

/* libnx's, by the offsets horizon.c asserts. */
typedef union { uint64_t x; uint32_t w; } CpuRegister;
typedef struct { uint64_t low, high; } FpuRegister;
typedef struct
{
    uint32_t error_desc;                  /* 0 */
    uint32_t pad[3];
    CpuRegister cpu_gprs[29];             /* 16 */
    CpuRegister fp;                       /* 248 */
    CpuRegister lr;                       /* 256 */
    CpuRegister sp;                       /* 264 */
    CpuRegister pc;                       /* 272 */
    uint32_t pad2[2];                     /* 280 */
    FpuRegister fpu_gprs[32];             /* 288 */
    uint32_t fpcr, fpsr;                  /* 800? */
    uint32_t pstate;
} ThreadExceptionDump;
C_ASSERT( offsetof(CONTEXT, X18) == 0x98 && offsetof(CONTEXT, Fp) == 0xf0 && offsetof(CONTEXT, V) == 0x110 );
C_ASSERT( offsetof(CONTEXT, Fpcr) == 0x310 );
C_ASSERT( offsetof(ThreadExceptionDump, cpu_gprs) == 16 );
C_ASSERT( offsetof(ThreadExceptionDump, fp) == 248 );
C_ASSERT( offsetof(ThreadExceptionDump, pc) == 272 );
C_ASSERT( offsetof(ThreadExceptionDump, fpu_gprs) == 288 );

/* pstate sits at 800 in libnx; place it there for the restore. */
#define PSTATE(d) (*(uint32_t *)((char *)(d) + 800))

static void *current_teb( void )
{
    void *x18;
    __asm__ volatile( "mov %0, x18" : "=r"(x18) );
    return x18;
}
#define NtCurrentTeb current_teb
#define CONTEXT_ARM64 0x400000
#define CONTEXT_ARM64_X18 (CONTEXT_ARM64 | 0x00000008)
#define horizon_trace(...) ((void)0)
typedef uintptr_t ULONG_PTR;
/* No FEX here: the context is continued by the restore, as for a native thread. */
static int horizon_uses_fex( void ) { return 0; }
static void wine_nx_fex_continue_context( const CONTEXT *context ) { (void)context; assert( 0 ); }

extern int horizon_capture_context( CONTEXT *context );

@CAPTURE@

@RESTORE@

@CONTINUE@

static CONTEXT taken;
static volatile int rounds;

int main( void )
{
    volatile uint64_t marker = 0x1234567890abcdefULL;
    NEON128 v8_after;

    /* q8 is callee-saved and q31 is the last one: both have to be taken, or
     * the restore would put back whatever the context's memory happened to
     * hold. Set them where the capture will see them. */
    __asm__ volatile( "fmov d8, #1.0\n\tfmov d31, #2.0\n\t"
                      "str q8, %0\n\tbl 1f\n1:" : "=m"(v8_after) :: "d8", "d31", "x30" );

    if (!horizon_capture_context( &taken ))
    {
        assert( taken.ContextFlags == CONTEXT_ARM64_FULL );
        assert( taken.Pc && taken.Sp );
        assert( taken.X[0] == 1 );  /* the value the continue comes back with */
        assert( taken.V[8].low == 0x3ff0000000000000ULL );   /* 1.0: q8 was taken */
        assert( taken.V[31].low == 0x4000000000000000ULL );  /* 2.0: and so was q31 */
        assert( (uint64_t)&marker - taken.Sp < 0x400 );
        /* Say what the restore must put back. */
        taken.V[8].low = 0xfeedfacecafebeefULL;
        taken.V[8].high = 0;
        rounds++;
        horizon_continue_context( &taken );
        assert( 0 && "the restore returned" );
    }
    __asm__ volatile( "str q8, %0" : "=m"(v8_after) );
    assert( rounds == 1 );
    assert( marker == 0x1234567890abcdefULL );   /* the stack came back */
    assert( v8_after.low == 0xfeedfacecafebeefULL );  /* and so did q8 */
    printf( "context taken and continued; q8 and the stack came back\n" );
    return 0;
}
'''

source = (fixture.replace('@CAPTURE@', capture)
                 .replace('@RESTORE@', restore)
                 .replace('@CONTINUE@', continue_.replace('context->Cpsr', 'context->Cpsr')))
# horizon_continue_context writes dump.pstate; the test's struct puts it at 800.
source = source.replace('dump.pstate = context->Cpsr;', 'PSTATE(&dump) = context->Cpsr;')

with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / 'capture.c'
    path.write_text(source)
    binary = Path(tmp) / 'capture'
    subprocess.run(['cc', '-O1', '-o', str(binary), str(path)], check=True)
    print(subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout.strip())
