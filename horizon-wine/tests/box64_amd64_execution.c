/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "amd64_box64_engine.h"

#define BASE UINT64_C(0x1010000000)
#define SIZE UINT64_C(0x20000)
#define NATIVE (BASE + 0x10000)
#define STACK (BASE + 0xf000)

struct fixture
{
    unsigned char *memory;
    BOOL native[SIZE / 0x1000];
    struct wine_nx_amd64_state state;
};

static NTSTATUS read_guest( void *opaque, ULONG_PTR address, void *buffer, SIZE_T size )
{
    struct fixture *fixture = opaque;

    if (address < BASE || address - BASE >= SIZE || size > SIZE - (address - BASE))
        return STATUS_ACCESS_VIOLATION;
    memcpy( buffer, fixture->memory + address - BASE, size );
    return STATUS_SUCCESS;
}

static BOOL is_native( void *opaque, ULONG_PTR address )
{
    struct fixture *fixture = opaque;

    return address >= BASE && address - BASE < SIZE && fixture->native[(address - BASE) >> 12];
}

static const struct wine_nx_amd64_host host = {read_guest, is_native, BASE + SIZE};

#ifdef WINE_NX_BOX64_DYNAREC
extern void wine_nx_box64_invalidate( uintptr_t address, size_t size, int destroy );
#endif

static void put_code( struct fixture *fixture, size_t offset, const void *code, size_t size )
{
    memcpy( fixture->memory + offset, code, size );
#ifdef WINE_NX_BOX64_DYNAREC
    wine_nx_box64_invalidate( BASE + offset, size, 0 );
#endif
}

static void init_context( AMD64_CONTEXT *context, ULONG_PTR rip )
{
    memset( context, 0, sizeof(*context) );
    context->ContextFlags = CONTEXT_AMD64_ALL;
    context->Rip = rip;
    context->Rsp = STACK;
    context->EFlags = 0x202;
    context->SegCs = 0x33;
    context->SegSs = context->SegDs = context->SegEs = 0x2b;
    context->SegFs = context->SegGs = 0x53;
    context->MxCsr = context->FltSave.MxCsr = 0x1f80;
    context->FltSave.ControlWord = 0x37f;
}

static NTSTATUS run( struct fixture *fixture, AMD64_CONTEXT *context, ULONG_PTR completion )
{
    ULONGLONG executed = 0;
    return wine_nx_box64_run_amd64( context, BASE + 0x6000, &fixture->state, &host, fixture,
                                    completion, 10000, &executed );
}

static void emit8( unsigned char **cursor, unsigned int value )
{
    *(*cursor)++ = value;
}

static void emit32( unsigned char **cursor, uint32_t value )
{
    memcpy( *cursor, &value, sizeof(value) );
    *cursor += sizeof(value);
}

static void emit64( unsigned char **cursor, uint64_t value )
{
    memcpy( *cursor, &value, sizeof(value) );
    *cursor += sizeof(value);
}

static void mov_imm64( unsigned char **cursor, unsigned int reg, uint64_t value )
{
    emit8( cursor, reg < 8 ? 0x48 : 0x49 );
    emit8( cursor, 0xb8 + (reg & 7) );
    emit64( cursor, value );
}

static void jump_native( unsigned char **cursor )
{
    mov_imm64( cursor, 0, NATIVE );
    emit8( cursor, 0xff );
    emit8( cursor, 0xe0 );
}

static void test_registers_gs_sse_call( struct fixture *fixture )
{
    static const uint64_t values[8] =
    {
        UINT64_C(0x0808080808080808), UINT64_C(0x0909090909090909),
        UINT64_C(0x1010101010101010), UINT64_C(0x1111111111111111),
        UINT64_C(0x1212121212121212), UINT64_C(0x1313131313131313),
        UINT64_C(0x1414141414141414), UINT64_C(0x1515151515151515)
    };
    const uint64_t gs_value = UINT64_C(0xfedcba9876543210);
    unsigned char code[256], *cursor = code, *call_disp, *subroutine;
    AMD64_CONTEXT context;
    size_t i;

    for (i = 0; i < 7; i++) mov_imm64( &cursor, 8 + i, values[i] );
    emit8( &cursor, 0x65 ); emit8( &cursor, 0x4c ); emit8( &cursor, 0x8b );
    emit8( &cursor, 0x34 ); emit8( &cursor, 0x25 ); emit32( &cursor, 0 );
    emit8( &cursor, 0x66 ); emit8( &cursor, 0x45 ); emit8( &cursor, 0x0f );
    emit8( &cursor, 0xd4 ); emit8( &cursor, 0xc1 );
    emit8( &cursor, 0xe8 ); call_disp = cursor; emit32( &cursor, 0 );
    jump_native( &cursor );
    subroutine = cursor;
    mov_imm64( &cursor, 15, values[7] );
    emit8( &cursor, 0xc3 );
    {
        int32_t disp = subroutine - (call_disp + 4);
        memcpy( call_disp, &disp, sizeof(disp) );
    }
    put_code( fixture, 0x100, code, cursor - code );
    memcpy( fixture->memory + 0x6000, &gs_value, sizeof(gs_value) );
    init_context( &context, BASE + 0x100 );
    context.FltSave.XmmRegisters[8].Low = 3;
    context.FltSave.XmmRegisters[8].High = 4;
    context.FltSave.XmmRegisters[9].Low = 5;
    context.FltSave.XmmRegisters[9].High = 6;
    assert( run( fixture, &context, 0 ) == STATUS_SUCCESS );
    assert( context.Rip == NATIVE && context.Rsp == STACK );
    assert( context.R8 == values[0] && context.R9 == values[1] );
    assert( context.R10 == values[2] && context.R11 == values[3] );
    assert( context.R12 == values[4] && context.R13 == values[5] );
    assert( context.R14 == gs_value && context.R15 == values[7] );
    assert( context.FltSave.XmmRegisters[8].Low == 8 );
    assert( context.FltSave.XmmRegisters[8].High == 10 );
    assert( context.FltSave.XmmRegisters[9].Low == 5 );
    assert( context.FltSave.XmmRegisters[9].High == 6 );
}

static void test_cpuid_and_stops( struct fixture *fixture )
{
    static const unsigned char cpuid[] =
    {
        0xb8, 0x01, 0x00, 0x00, 0x80, 0x0f, 0xa2,
        0x48, 0xbe, 0x00, 0x00, 0x01, 0x10, 0x10, 0x00, 0x00, 0x00,
        0xff, 0xe6
    };
    static const unsigned char jump_completion[] =
    {
        0x48, 0xb8, 0x00, 0x30, 0x00, 0x10, 0x10, 0x00, 0x00, 0x00, 0xff, 0xe0
    };
    AMD64_CONTEXT context;

    put_code( fixture, 0x200, cpuid, sizeof(cpuid) );
    init_context( &context, BASE + 0x200 );
    assert( run( fixture, &context, 0 ) == STATUS_SUCCESS );
    assert( context.Rip == NATIVE && (context.Rdx & (UINT64_C(1) << 29)) );

    fixture->memory[0x204] = 0;
#ifdef WINE_NX_BOX64_DYNAREC
    wine_nx_box64_invalidate( BASE + 0x200, sizeof(cpuid), 0 );
#endif
    init_context( &context, BASE + 0x200 );
    assert( run( fixture, &context, 0 ) == STATUS_SUCCESS );
    assert( context.Rcx == 0x00980201 );

    put_code( fixture, 0x280, jump_completion, sizeof(jump_completion) );
    init_context( &context, BASE + 0x280 );
    assert( run( fixture, &context, BASE + 0x3000 ) == STATUS_SUCCESS );
    assert( context.Rip == BASE + 0x3000 );

    fixture->memory[0x2ff] = 0x90;
#ifdef WINE_NX_BOX64_DYNAREC
    wine_nx_box64_invalidate( BASE + 0x2ff, 1, 0 );
#endif
    init_context( &context, BASE + 0x2ff );
    assert( run( fixture, &context, BASE + 0x300 ) == STATUS_SUCCESS );
    assert( context.Rip == BASE + 0x300 );

    fixture->memory[0xffff] = 0x90;
#ifdef WINE_NX_BOX64_DYNAREC
    wine_nx_box64_invalidate( BASE + 0xffff, 1, 0 );
#endif
    init_context( &context, BASE + 0xffff );
    assert( run( fixture, &context, 0 ) == STATUS_SUCCESS );
    assert( context.Rip == NATIVE );
}

static void test_popcnt( struct fixture *fixture )
{
    static const struct
    {
        unsigned char code[5];
        unsigned int size, bits;
    } forms[] =
    {
        { {0x66,0xf3,0x0f,0xb8,0xc1}, 5, 16 },
        { {0xf3,0x0f,0xb8,0xc1}, 4, 32 },
        { {0xf3,0x48,0x0f,0xb8,0xc1}, 5, 64 },
    };
    static const uint64_t values[] = { 0, UINT64_MAX, UINT64_C(0xffffffff00000000),
                                      UINT64_C(0x8000000100010001) };
    const uint64_t initial = UINT64_C(0x1122334455667788);
    unsigned char code[32], *cursor;
    AMD64_CONTEXT context;
    unsigned int i, j;

    for (i = 0; i < sizeof(forms) / sizeof(forms[0]); i++)
    {
        const uint64_t mask = UINT64_MAX >> (64 - forms[i].bits);

        memcpy( code, forms[i].code, forms[i].size );
        cursor = code + forms[i].size;
        mov_imm64( &cursor, 2, NATIVE );
        emit8( &cursor, 0xff ); emit8( &cursor, 0xe2 );
        put_code( fixture, 0x400 + i * 0x40, code, cursor - code );
        for (j = 0; j < sizeof(values) / sizeof(values[0]); j++)
        {
            uint64_t expected = __builtin_popcountll( values[j] & mask );
            const ULONG flags = expected ? 0 : 0x40;

            if (forms[i].bits == 16) expected |= initial & ~mask;
            init_context( &context, BASE + 0x400 + i * 0x40 );
            context.Rax = initial;
            context.Rcx = values[j];
            context.EFlags |= 0x8d5;
            assert( run( fixture, &context, 0 ) == STATUS_SUCCESS );
            assert( context.Rip == NATIVE && context.Rax == expected );
            assert( (context.EFlags & 0x8d5) == flags );
        }
    }
}

static void test_syscall_state( struct fixture *fixture )
{
    static const uint64_t values[16] =
    {
        0x1234, 0x1111, 0x2222, 0x3333, 0, 0x5555, 0x6666, 0x7777,
        0x8888, 0x9999, 0xaaaa, 0xbbbb, 0xcccc, 0xdddd, 0xeeee, 0xffff
    };
    static const unsigned int regs[] = {0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    unsigned char code[256], *cursor = code;
    AMD64_CONTEXT context;
    size_t i;

    for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) mov_imm64( &cursor, regs[i], values[regs[i]] );
    emit8( &cursor, 0x0f ); emit8( &cursor, 0x05 );
    put_code( fixture, 0x500, code, cursor - code );
    init_context( &context, BASE + 0x500 );
    context.FltSave.XmmRegisters[0].Low = UINT64_C(0x0123456789abcdef);
    context.FltSave.XmmRegisters[0].High = UINT64_C(0xfedcba9876543210);
    context.FltSave.XmmRegisters[15].Low = UINT64_C(0x5555555555555555);
    context.FltSave.XmmRegisters[15].High = UINT64_C(0xaaaaaaaaaaaaaaaa);
    assert( run( fixture, &context, 0 ) == STATUS_EMULATION_SYSCALL );
    assert( context.Rip == BASE + 0x500 + (cursor - code) );
    assert( context.Rax == values[0] && context.Rcx == values[1] && context.Rdx == values[2] );
    assert( context.Rbx == values[3] && context.Rsp == STACK && context.Rbp == values[5] );
    assert( context.Rsi == values[6] && context.Rdi == values[7] );
    assert( context.R8 == values[8] && context.R9 == values[9] );
    assert( context.R10 == values[10] && context.R11 == values[11] );
    assert( context.R12 == values[12] && context.R13 == values[13] );
    assert( context.R14 == values[14] && context.R15 == values[15] );
    assert( context.FltSave.XmmRegisters[0].Low == UINT64_C(0x0123456789abcdef) );
    assert( (ULONGLONG)context.FltSave.XmmRegisters[0].High == UINT64_C(0xfedcba9876543210) );
    assert( context.FltSave.XmmRegisters[15].Low == UINT64_C(0x5555555555555555) );
    assert( (ULONGLONG)context.FltSave.XmmRegisters[15].High == UINT64_C(0xaaaaaaaaaaaaaaaa) );
}

static void test_sse_features( struct fixture *fixture )
{
    static const struct
    {
        const char *name;
        unsigned char code[6];
        unsigned int size;
        uint32_t left[4], right[4], expected[4];
    } cases[] =
    {
        { "HADDPS", {0xf2,0x0f,0x7c,0xc1}, 4,
          {0x3f800000,0x40000000,0x40400000,0x40800000},
          {0x40a00000,0x40c00000,0x40e00000,0x41000000},
          {0x40400000,0x40e00000,0x41300000,0x41700000} },
        { "PSHUFB", {0x66,0x0f,0x38,0x00,0xc1}, 5,
          {0x03020100,0x07060504,0x0b0a0908,0x0f0e0d0c},
          {0x0c0d0e0f,0x08090a0b,0x04050607,0x80010203},
          {0x0c0d0e0f,0x08090a0b,0x04050607,0x00010203} },
        { "PMULLD", {0x66,0x0f,0x38,0x40,0xc1}, 5,
          {2,0xffffffff,100000,0x80000000}, {3,2,100000,2}, {6,0xfffffffe,1410065408,0} },
        { "PCMPGTQ", {0x66,0x0f,0x38,0x37,0xc1}, 5,
          {0,1,0xffffffff,0xffffffff}, {0xffffffff,0,1,0}, {0xffffffff,0xffffffff,0,0} },
        { "PCMPISTRI", {0x66,0x0f,0x3a,0x63,0xc1,0}, 6,
          {0x00006361,0,0,0}, {0x637a7978,0x61,0,0}, {0x00006361,0,0,0} },
    };
    AMD64_CONTEXT context;
    unsigned char code[32], *cursor;

    for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        memcpy( code, cases[i].code, cases[i].size );
        cursor = code + cases[i].size;
        jump_native( &cursor );
        put_code( fixture, 0x1800 + i * 0x40, code, cursor - code );
        init_context( &context, BASE + 0x1800 + i * 0x40 );
        memcpy( &context.FltSave.XmmRegisters[0], cases[i].left, 16 );
        memcpy( &context.FltSave.XmmRegisters[1], cases[i].right, 16 );
        assert( run( fixture, &context, 0 ) == STATUS_SUCCESS );
        assert( !memcmp( &context.FltSave.XmmRegisters[0], cases[i].expected, 16 ) );
        assert( !memcmp( &context.FltSave.XmmRegisters[1], cases[i].right, 16 ) );
        if (i == 4) assert( context.Rcx == 3 );
        printf( "PASS %s\n", cases[i].name );
    }
    /* CRC32 eax, ecx; leave eax intact when returning to the native gate. */
    memcpy( code, "\xf2\x0f\x38\xf1\xc1", 5 );
    cursor = code + 5;
    mov_imm64( &cursor, 6, NATIVE );
    emit8( &cursor, 0xff ); emit8( &cursor, 0xe6 );
    put_code( fixture, 0x1a00, code, cursor - code );
    for (unsigned int i = 0; i < 128; i++)
    {
        uint32_t input = 0x12345678u * (i + 1), expected = 0xabcdef01u ^ input;
        for (unsigned int bit = 0; bit < 32; bit++)
            expected = (expected >> 1) ^ (0x82f63b78u & (0u - (expected & 1)));
        init_context( &context, BASE + 0x1a00 );
        context.Rax = 0xabcdef01u;
        context.Rcx = input;
        assert( run( fixture, &context, 0 ) == STATUS_SUCCESS );
        assert( context.Rax == expected && context.Rcx == input );
    }
    puts( "PASS CRC32" );
}

static void test_x87_context( struct fixture *fixture )
{
    unsigned char load[32], store[32], *cursor;
    AMD64_CONTEXT context;
    const double input = 3.141592653589793;
    double output = 0.0;
    size_t i;
    int32_t disp;

    cursor = load;
    emit8( &cursor, 0xdd ); emit8( &cursor, 0x05 );
    disp = (BASE + 0x7000) - (BASE + 0x700 + 6); emit32( &cursor, disp );
    jump_native( &cursor );
    put_code( fixture, 0x700, load, cursor - load );
    memcpy( fixture->memory + 0x7000, &input, sizeof(input) );
    init_context( &context, BASE + 0x700 );
    memset( context.FltSave.FloatRegisters, 0xa5, sizeof(context.FltSave.FloatRegisters) );
    assert( run( fixture, &context, 0 ) == STATUS_SUCCESS );
    assert( context.FltSave.TagWord == 0x80 );
    for (i = 10; i < sizeof(M128A); i++)
        assert( ((unsigned char *)&context.FltSave.FloatRegisters[0])[i] == 0xa5 );
    for (i = 0; i < sizeof(M128A); i++)
        assert( ((unsigned char *)&context.FltSave.FloatRegisters[1])[i] == 0xa5 );

    cursor = store;
    emit8( &cursor, 0xdd ); emit8( &cursor, 0x1d );
    disp = (BASE + 0x7010) - (BASE + 0x780 + 6); emit32( &cursor, disp );
    jump_native( &cursor );
    put_code( fixture, 0x780, store, cursor - store );
    context.Rip = BASE + 0x780;
    assert( run( fixture, &context, 0 ) == STATUS_SUCCESS );
    memcpy( &output, fixture->memory + 0x7010, sizeof(output) );
    assert( output == input && context.FltSave.TagWord == 0 );
}

static void test_mmx_context( struct fixture *fixture )
{
    const uint64_t left = UINT64_C(0x1020304050607080);
    const uint64_t right = UINT64_C(0x0102030405060708);
    unsigned char load[64], add[64], *cursor;
#ifndef WINE_NX_BOX64_DYNAREC
    unsigned char timeout[64];
#endif
    AMD64_CONTEXT context;
    uint64_t output, native_output;
    int32_t disp;

    memcpy( fixture->memory + 0x7200, &left, sizeof(left) );
    memcpy( fixture->memory + 0x7210, &right, sizeof(right) );

    cursor = load;
    emit8( &cursor, 0x0f ); emit8( &cursor, 0x6f ); emit8( &cursor, 0x05 );
    disp = (BASE + 0x7200) - (BASE + 0xa80 + 7); emit32( &cursor, disp );
    emit8( &cursor, 0x0f ); emit8( &cursor, 0x6f ); emit8( &cursor, 0x0d );
    disp = (BASE + 0x7210) - (BASE + 0xa80 + 14); emit32( &cursor, disp );
    jump_native( &cursor );
    put_code( fixture, 0xa80, load, cursor - load );

    cursor = add;
    emit8( &cursor, 0x0f ); emit8( &cursor, 0xd4 ); emit8( &cursor, 0xc1 );
    emit8( &cursor, 0x0f ); emit8( &cursor, 0x7f ); emit8( &cursor, 0x05 );
    disp = (BASE + 0x7220) - (BASE + 0xb00 + 10); emit32( &cursor, disp );
    emit8( &cursor, 0x0f ); emit8( &cursor, 0x77 );
    jump_native( &cursor );
    put_code( fixture, 0xb00, add, cursor - add );

    init_context( &context, BASE + 0xa80 );
    memset( context.FltSave.FloatRegisters, 0xa5, sizeof(context.FltSave.FloatRegisters) );
    assert( run( fixture, &context, 0 ) == STATUS_SUCCESS );
#ifndef WINE_NX_BOX64_DYNAREC
    timeout[0] = timeout[1] = 0x90;
    put_code( fixture, 0xc00, timeout, 2 );
    context.Rip = BASE + 0xc00;
    assert( wine_nx_box64_run_amd64( &context, BASE + 0x6000, &fixture->state, &host, fixture,
                                     0, 1, NULL ) == STATUS_TIMEOUT );
    assert( context.Rip == BASE + 0xc01 );
#endif
    context.Rip = BASE + 0xb00;
    assert( run( fixture, &context, 0 ) == STATUS_SUCCESS );
    memcpy( &output, fixture->memory + 0x7220, sizeof(output) );
    native_output = output;
#ifndef WINE_NX_BOX64_DYNAREC
    cursor = timeout;
    emit8( &cursor, 0x0f ); emit8( &cursor, 0x6f ); emit8( &cursor, 0x05 );
    disp = (BASE + 0x7200) - (BASE + 0xb80 + 7); emit32( &cursor, disp );
    emit8( &cursor, 0x0f ); emit8( &cursor, 0x6f ); emit8( &cursor, 0x0d );
    disp = (BASE + 0x7210) - (BASE + 0xb80 + 14); emit32( &cursor, disp );
    emit8( &cursor, 0x0f ); emit8( &cursor, 0xd4 ); emit8( &cursor, 0xc1 );
    emit8( &cursor, 0x0f ); emit8( &cursor, 0x7f ); emit8( &cursor, 0x05 );
    disp = (BASE + 0x7230) - (BASE + 0xb80 + 24); emit32( &cursor, disp );
    jump_native( &cursor );
    put_code( fixture, 0xb80, timeout, cursor - timeout );
    init_context( &context, BASE + 0xb80 );
    memset( context.FltSave.FloatRegisters, 0x5a, sizeof(context.FltSave.FloatRegisters) );
    assert( wine_nx_box64_run_amd64( &context, BASE + 0x6000, &fixture->state, &host, fixture,
                                     0, 2, NULL ) == STATUS_TIMEOUT );
    assert( context.Rip == BASE + 0xb80 + 14 );
    assert( run( fixture, &context, 0 ) == STATUS_SUCCESS );
    memcpy( &output, fixture->memory + 0x7230, sizeof(output) );
    assert( output == left + right );
#endif
    assert( native_output == left + right );
}

static void test_unsupported( struct fixture *fixture )
{
    static const unsigned char avx[] = {0xc5, 0xf8, 0x77};
    AMD64_CONTEXT context;

    put_code( fixture, 0x900, avx, sizeof(avx) );
    init_context( &context, BASE + 0x900 );
    assert( run( fixture, &context, 0 ) == STATUS_NOT_SUPPORTED );
    assert( context.Rip == BASE + 0x900 );
    init_context( &context, BASE + 0x900 );
    context.ContextFlags |= CONTEXT_AMD64_XSTATE;
    assert( wine_nx_box64_run_amd64( &context, BASE + 0x6000, &fixture->state, &host, fixture,
                                     0, 100, NULL ) == STATUS_NOT_SUPPORTED );
}

#ifdef WINE_NX_BOX64_DYNAREC
extern void wine_nx_box64_dynarec_add_stop( uintptr_t address );
extern unsigned long long wine_nx_box64_native_entries;
static void test_high_invalidation( struct fixture *fixture )
{
    unsigned char code[32], *cursor = code;
    AMD64_CONTEXT context;
    uint64_t value = 1;
    unsigned long long entries;

    wine_nx_box64_dynarec_add_stop( (uintptr_t)(BASE & UINT32_MAX) );
    mov_imm64( &cursor, 0, value );
    emit8( &cursor, 0x48 ); emit8( &cursor, 0xba ); emit64( &cursor, NATIVE );
    emit8( &cursor, 0xff ); emit8( &cursor, 0xe2 );
    put_code( fixture, 0xa00, code, cursor - code );
    init_context( &context, BASE + 0xa00 );
    entries = wine_nx_box64_native_entries;
    assert( run( fixture, &context, 0 ) == STATUS_SUCCESS && context.Rax == 1 );
    assert( wine_nx_box64_native_entries > entries );
    value = 2;
    memcpy( fixture->memory + 0xa02, &value, sizeof(value) );
    wine_nx_box64_invalidate( BASE + 0xa00, cursor - code, 0 );
    init_context( &context, BASE + 0xa00 );
    assert( run( fixture, &context, 0 ) == STATUS_SUCCESS && context.Rax == 2 );
}

extern uint64_t wine_nx_box64_dynarec_bytes;

/* A call into native code leaves the engine, which comes back at the return
 * site. That site must be an entry point of the block made for the call, or
 * the rest of the block is translated again for every such call. */
static void test_native_return_sites( struct fixture *fixture )
{
    unsigned char code[512], *cursor = code;
    AMD64_CONTEXT context;
    uint64_t before, first, all;
    unsigned int i, calls = 24;
    int32_t disp;

    for (i = 0; i < calls; i++)
    {
        emit8( &cursor, 0xe8 );
        disp = (NATIVE + 0x100) - (BASE + 0xd00 + (cursor - code) + 4);
        emit32( &cursor, disp );
        emit8( &cursor, 0x48 ); emit8( &cursor, 0x83 ); emit8( &cursor, 0xc3 ); emit8( &cursor, 0x01 );
    }
    jump_native( &cursor );
    put_code( fixture, 0xd00, code, cursor - code );
    init_context( &context, BASE + 0xd00 );
    before = wine_nx_box64_dynarec_bytes;
    assert( run( fixture, &context, 0 ) == STATUS_SUCCESS );
    first = wine_nx_box64_dynarec_bytes - before;
    for (i = 0; i < calls; i++)
    {
        assert( context.Rip == NATIVE + 0x100 && context.Rsp == STACK - 8 );
        memcpy( &context.Rip, fixture->memory + (context.Rsp - BASE), sizeof(context.Rip) );
        context.Rsp += sizeof(uint64_t);
        assert( run( fixture, &context, 0 ) == STATUS_SUCCESS );
    }
    assert( context.Rip == NATIVE && context.Rbx == calls );
    all = wine_nx_box64_dynarec_bytes - before;
    assert( all < 2 * first + 4096 );
}
#endif

int main(void)
{
    struct fixture fixture = {0};

    fixture.memory = mmap( (void *)(uintptr_t)BASE, SIZE, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0 );
    if (fixture.memory == MAP_FAILED)
    {
        perror( "mmap" );
        return 77;
    }
    fixture.native[(NATIVE - BASE) >> 12] = TRUE;
    test_registers_gs_sse_call( &fixture );
    test_cpuid_and_stops( &fixture );
    test_popcnt( &fixture );
    test_sse_features( &fixture );
    test_syscall_state( &fixture );
    test_x87_context( &fixture );
    test_mmx_context( &fixture );
    test_unsupported( &fixture );
#ifdef WINE_NX_BOX64_DYNAREC
    test_high_invalidation( &fixture );
    test_native_return_sites( &fixture );
#endif
    assert( !munmap( fixture.memory, SIZE ) );
    puts( "box64 amd64 execution: ok" );
    return 0;
}
