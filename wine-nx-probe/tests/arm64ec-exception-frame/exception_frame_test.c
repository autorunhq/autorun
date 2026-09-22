#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int32_t NTSTATUS;
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef uint64_t ULONG64;
typedef uint64_t ULONG_PTR;
typedef uint64_t SIZE_T;
typedef void *HANDLE;

#define STATUS_UNSUCCESSFUL      ((NTSTATUS)0xc0000001)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#define STATUS_INVALID_ADDRESS   ((NTSTATUS)0xc0000141)
#define STATUS_PARTIAL_COPY      ((NTSTATUS)0x8000000d)
#define CONTEXT_ARM64_FULL       0x00400007
#define NtCurrentProcess()       ((HANDLE)(uintptr_t)-1)
#define NtCurrentTeb()           ((void *)(uintptr_t)0x77770000)
#define C_ASSERT(e)              _Static_assert((e), #e)
#define X18 X[18]

typedef struct __attribute__((aligned(16)))
{
    ULONG ContextFlags;
    ULONG Cpsr;
    uint64_t X[31];
    uint64_t Sp;
    uint64_t Pc;
    unsigned char tail[0x280];
} CONTEXT;

typedef struct
{
    LONG Offset;
    ULONG Length;
} CONTEXT_CHUNK;

typedef struct
{
    CONTEXT_CHUNK All;
    CONTEXT_CHUNK Legacy;
    CONTEXT_CHUNK XState;
    ULONG64 align;
} CONTEXT_EX;

#define EXCEPTION_MAXIMUM_PARAMETERS 15
typedef struct _EXCEPTION_RECORD
{
    ULONG ExceptionCode;
    ULONG ExceptionFlags;
    struct _EXCEPTION_RECORD *ExceptionRecord;
    void *ExceptionAddress;
    ULONG NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD;

struct test_peb { void *EcCodeBitMap; };
struct test_peb test_peb;
struct test_peb *peb = &test_peb;
void *pKiUserEmulationDispatcher;
void *pKiUserExceptionDispatcher;

C_ASSERT( sizeof(CONTEXT) == 0x390 );
C_ASSERT( offsetof(CONTEXT, Sp) == 0x100 );
C_ASSERT( offsetof(CONTEXT, Pc) == 0x108 );
C_ASSERT( sizeof(CONTEXT_EX) == 0x20 );
C_ASSERT( sizeof(EXCEPTION_RECORD) == 0x98 );

struct write_record
{
    void *address;
    SIZE_T size;
    unsigned char data[0x470];
};

static uint64_t ec_bitmap[64];
static struct write_record writes[4];
static unsigned int write_count, fail_write_call, partial_write_call;
static NTSTATUS fail_write_status;
static unsigned int continue_count, is_ec_count;
static CONTEXT continued;
static int arm64ec;
static uintptr_t address_start, address_end;
static unsigned int failures;

int is_arm64ec(void)
{
    return arm64ec;
}

void horizon_get_address_space_limits( void **start, void **end )
{
    *start = (void *)address_start;
    *end = (void *)address_end;
}

int is_ec_code( uintptr_t address )
{
    ++is_ec_count;
    return !!(ec_bitmap[address >> 18] & (1ull << ((address >> 12) & 63)));
}

NTSTATUS NtWriteVirtualMemory( HANDLE process, void *address, const void *buffer,
                               SIZE_T size, SIZE_T *written )
{
    struct write_record *record = &writes[write_count];
    unsigned int call = ++write_count;

    (void)process;
    record->address = address;
    record->size = size;
    memcpy( record->data, buffer, size );
    if (call == fail_write_call) return fail_write_status;
    if (call == partial_write_call)
    {
        *written = size - 1;
        return 0;
    }
    memcpy( address, buffer, size );
    *written = size;
    return 0;
}

void horizon_continue_context( const CONTEXT *context )
{
    ++continue_count;
    continued = *context;
}

void horizon_trace( const char *format, ... )
{
    (void)format;
}

#include "extracted_functions.inc"

static void set_ec( uintptr_t address, int value )
{
    uint64_t *word = &ec_bitmap[address >> 18];
    uint64_t bit = 1ull << ((address >> 12) & 63);
    if (value) *word |= bit;
    else *word &= ~bit;
}

static void reset_mocks(void)
{
    memset( writes, 0, sizeof(writes) );
    memset( &continued, 0, sizeof(continued) );
    write_count = fail_write_call = partial_write_call = continue_count = is_ec_count = 0;
    fail_write_status = 0;
}

static CONTEXT make_context( uintptr_t pc, uintptr_t sp )
{
    CONTEXT context;
    unsigned int i;

    memset( &context, 0, sizeof(context) );
    context.ContextFlags = CONTEXT_ARM64_FULL;
    context.Pc = pc;
    context.Sp = sp;
    context.X18 = 0x1818181818181818ull;
    for (i = 0; i < 18; ++i) context.X[i] = 0x1000000000000000ull + i;
    return context;
}

static void report( const char *name, int passed )
{
    printf( "%s %s\n", passed ? "PASS" : "FAIL", name );
    failures += !passed;
}

static void test_signal_validation(void)
{
    CONTEXT context = make_context( 0x20000, 0x100000 );

    reset_mocks();
    report( "context-null", signal_set_full_context( NULL ) == STATUS_INVALID_PARAMETER );
    context.ContextFlags = 0;
    report( "context-flags", signal_set_full_context( &context ) == STATUS_INVALID_PARAMETER );
    context = make_context( 0x20000, sizeof(CONTEXT) );
    arm64ec = 1;
    report( "context-low-stack", signal_set_full_context( &context ) == STATUS_INVALID_ADDRESS );
}

static void test_native_and_ec_routes(void)
{
    CONTEXT context = make_context( 0x22000, 0x100000 );

    reset_mocks();
    arm64ec = 0;
    report( "native-pc-direct", signal_set_full_context( &context ) == STATUS_UNSUCCESSFUL &&
            continue_count == 1 && continued.Pc == context.Pc && continued.Sp == context.Sp &&
            !write_count && !is_ec_count );

    reset_mocks();
    arm64ec = 1;
    set_ec( context.Pc, 1 );
    report( "arm64ec-ec-pc-direct", signal_set_full_context( &context ) == STATUS_UNSUCCESSFUL &&
            continue_count == 1 && continued.Pc == context.Pc && continued.Sp == context.Sp &&
            !write_count && is_ec_count == 1 );
    set_ec( context.Pc, 0 );
}

static void test_guest_route_and_writes(void)
{
    _Alignas(16) unsigned char stack[0x1000];
    CONTEXT context = make_context( 0x23000, (uintptr_t)(stack + sizeof(stack)) );
    uintptr_t expected = (context.Sp - sizeof(CONTEXT)) & ~(uintptr_t)15;
    NTSTATUS injected = (NTSTATUS)0xc0123456;

    reset_mocks();
    arm64ec = 1;
    report( "arm64ec-guest-pc-route", signal_set_full_context( &context ) == STATUS_UNSUCCESSFUL &&
            write_count == 1 && writes[0].address == (void *)expected &&
            writes[0].size == sizeof(CONTEXT) && !memcmp( writes[0].data, &context, sizeof(context) ) &&
            continue_count == 1 && continued.Pc == (uintptr_t)pKiUserEmulationDispatcher &&
            continued.Sp == expected );

    reset_mocks();
    fail_write_call = 1;
    fail_write_status = injected;
    report( "context-write-error", signal_set_full_context( &context ) == injected &&
            write_count == 1 && !continue_count );

    reset_mocks();
    partial_write_call = 1;
    report( "context-write-partial", signal_set_full_context( &context ) == STATUS_PARTIAL_COPY &&
            write_count == 1 && !continue_count );
}

static void test_arm64ec_bounds(void)
{
    CONTEXT context = make_context( 1ull << 39, 0x100000 );

    reset_mocks();
    arm64ec = 1;
    report( "context-39bit-bound", signal_set_full_context( &context ) == STATUS_INVALID_ADDRESS &&
            !is_ec_count && !write_count && !continue_count );
    address_end = 1ull << 40;
    report( "context-bitmap-bound", signal_set_full_context( &context ) == STATUS_INVALID_ADDRESS &&
            !is_ec_count && !write_count && !continue_count );
    address_end = 1ull << 39;
    context.Pc = address_start - 1;
    report( "context-lower-bound", signal_set_full_context( &context ) == STATUS_INVALID_ADDRESS );
    context.Pc = 0x22000;
    test_peb.EcCodeBitMap = NULL;
    report( "context-missing-bitmap", signal_set_full_context( &context ) == STATUS_INVALID_PARAMETER );
    test_peb.EcCodeBitMap = ec_bitmap;
}

static void init_exception( EXCEPTION_RECORD *record )
{
    unsigned int i;
    memset( record, 0, sizeof(*record) );
    record->ExceptionCode = 0xc0000005;
    record->ExceptionFlags = 2;
    record->ExceptionAddress = (void *)0x23450;
    record->NumberParameters = 15;
    for (i = 0; i < 15; ++i) record->ExceptionInformation[i] = 0x7000 + i;
}

static void test_exception_frame(void)
{
    _Alignas(16) unsigned char stack[0x2000];
    CONTEXT context = make_context( 0x25000, (uintptr_t)(stack + sizeof(stack) - 3) );
    EXCEPTION_RECORD record;
    CONTEXT_EX context_ex;
    uintptr_t frame = (context.Sp & ~(uintptr_t)15) - 0x470;
    ULONG64 value;

    init_exception( &record );
    reset_mocks();
    arm64ec = 1;
    set_ec( (uintptr_t)pKiUserExceptionDispatcher, 1 );
    report( "exception-frame-dispatch", call_user_exception_dispatcher( &record, &context ) == STATUS_UNSUCCESSFUL &&
            write_count == 1 && writes[0].address == (void *)frame && writes[0].size == 0x470 &&
            continue_count == 1 && continued.Pc == (uintptr_t)pKiUserExceptionDispatcher &&
            continued.Sp == frame && continued.X18 == (uintptr_t)NtCurrentTeb() );
    memcpy( &context_ex, writes[0].data + 0x390, sizeof(context_ex) );
    report( "exception-frame-offsets", !memcmp( writes[0].data, &context, sizeof(context) ) &&
            !memcmp( writes[0].data + 0x3b0, &record, sizeof(record) ) &&
            context_ex.Legacy.Offset == -0x390 && context_ex.Legacy.Length == 0x390 &&
            context_ex.XState.Offset == 0xd0 && context_ex.XState.Length == 0 &&
            context_ex.All.Offset == -0x390 && context_ex.All.Length == 0x460 );
    memcpy( &value, writes[0].data + 0x448, sizeof(value) );
    report( "exception-frame-trailer", value == 0 &&
            !memcmp( writes[0].data + 0x450, &context.Sp, sizeof(context.Sp) ) &&
            !memcmp( writes[0].data + 0x458, &context.Pc, sizeof(context.Pc) ) );
    set_ec( (uintptr_t)pKiUserExceptionDispatcher, 0 );
}

static void test_exception_errors_and_guest_route(void)
{
    _Alignas(16) unsigned char stack[0x2000];
    CONTEXT context = make_context( 0x25000, (uintptr_t)(stack + sizeof(stack)) );
    EXCEPTION_RECORD record;
    NTSTATUS injected = (NTSTATUS)0xc0234567;
    uintptr_t frame = (context.Sp & ~(uintptr_t)15) - 0x470;
    uintptr_t guest_context = (frame - sizeof(CONTEXT)) & ~(uintptr_t)15;

    init_exception( &record );
    reset_mocks();
    fail_write_call = 1;
    fail_write_status = injected;
    report( "exception-frame-write-error", call_user_exception_dispatcher( &record, &context ) == injected &&
            write_count == 1 && !continue_count );

    reset_mocks();
    partial_write_call = 1;
    report( "exception-frame-write-partial", call_user_exception_dispatcher( &record, &context ) == STATUS_PARTIAL_COPY &&
            write_count == 1 && !continue_count );

    reset_mocks();
    report( "exception-guest-dispatch-route", call_user_exception_dispatcher( &record, &context ) == STATUS_UNSUCCESSFUL &&
            write_count == 2 && writes[0].address == (void *)frame &&
            writes[1].address == (void *)guest_context && continue_count == 1 &&
            continued.Pc == (uintptr_t)pKiUserEmulationDispatcher && continued.Sp == guest_context &&
            ((CONTEXT *)writes[1].data)->Pc == (uintptr_t)pKiUserExceptionDispatcher &&
            ((CONTEXT *)writes[1].data)->Sp == frame );

    reset_mocks();
    fail_write_call = 2;
    fail_write_status = injected;
    report( "exception-guest-context-write-error", call_user_exception_dispatcher( &record, &context ) == injected &&
            write_count == 2 && !continue_count );
}

static void test_exception_validation_and_bounds(void)
{
    _Alignas(16) unsigned char stack[0x1000];
    CONTEXT context = make_context( 0x25000, (uintptr_t)(stack + sizeof(stack)) );
    EXCEPTION_RECORD record;

    init_exception( &record );
    reset_mocks();
    report( "exception-null-record", call_user_exception_dispatcher( NULL, &context ) == STATUS_INVALID_PARAMETER );
    report( "exception-null-context", call_user_exception_dispatcher( &record, NULL ) == STATUS_INVALID_PARAMETER );
    context.ContextFlags = 0;
    report( "exception-context-flags", call_user_exception_dispatcher( &record, &context ) == STATUS_INVALID_PARAMETER );
    context = make_context( 0x25000, 0x400 );
    report( "exception-low-stack", call_user_exception_dispatcher( &record, &context ) == STATUS_INVALID_ADDRESS );

    context = make_context( 0x25000, (uintptr_t)(stack + sizeof(stack)) );
    pKiUserExceptionDispatcher = (void *)(1ull << 39);
    reset_mocks();
    report( "exception-dispatcher-39bit-bound", call_user_exception_dispatcher( &record, &context ) == STATUS_INVALID_ADDRESS &&
            write_count == 1 && !continue_count && !is_ec_count );
    pKiUserExceptionDispatcher = (void *)0x24000;
}

int main(void)
{
    address_start = 0x10000;
    address_end = 1ull << 39;
    test_peb.EcCodeBitMap = ec_bitmap;
    pKiUserEmulationDispatcher = (void *)0x28000;
    pKiUserExceptionDispatcher = (void *)0x24000;

    test_signal_validation();
    test_native_and_ec_routes();
    test_guest_route_and_writes();
    test_arm64ec_bounds();
    test_exception_frame();
    test_exception_errors_and_guest_route();
    test_exception_validation_and_bounds();
    printf( "RESULT %s\n", failures ? "FAIL" : "PASS" );
    return failures != 0;
}
