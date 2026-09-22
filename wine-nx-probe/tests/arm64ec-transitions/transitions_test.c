#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { MODE_CAPTURE = 1, MODE_ENTRY, MODE_INVALID };

_Alignas(16) unsigned char fake_teb[0x1790];
_Alignas(16) static unsigned char fake_peb[0x370];
_Alignas(16) static unsigned char cpu_area[0x58];
_Alignas(16) static unsigned char context[0x4d0];
_Alignas(16) static unsigned char emulator_stack[0x10000];
_Alignas(16) static unsigned char guest_stack[0x100];
static uint64_t ec_bitmap[1u << 21];

uint64_t input_gpr[15], output_gpr[15], observed_gpr[17];
_Alignas(16) unsigned char input_vec[16][16], output_vec[16][16], observed_vec[16][16];
uint64_t saved_host[14];
_Alignas(16) unsigned char saved_host_vec[8][16];
uint64_t initial_target, observed_entry[4];
unsigned int observed_kind;
void *x64_return_instr;

static unsigned int mode, run_calls, capture_ok;
static uint64_t requested_sp, requested_target;
static unsigned int failures;

extern void invoke_dispatch(void);
extern void invoke_ret(void);
extern void invoke_begin(void);
extern char direct_target, guest_target, ret_sentinel;

static const unsigned int context_gpr_offsets[15] =
{
    0x078, 0x080, 0x088, 0x090, 0x0a0, 0x0a8, 0x0b0, 0x0b8,
    0x0c0, 0x0c8, 0x0d0, 0x0d8, 0x0e0, 0x0e8, 0x0f0
};

static uint64_t load64( const void *base, unsigned int offset )
{
    uint64_t value;
    memcpy( &value, (const unsigned char *)base + offset, sizeof(value) );
    return value;
}

static uint16_t load16( const void *base, unsigned int offset )
{
    uint16_t value;
    memcpy( &value, (const unsigned char *)base + offset, sizeof(value) );
    return value;
}

static void store64( void *base, unsigned int offset, uint64_t value )
{
    memcpy( (unsigned char *)base + offset, &value, sizeof(value) );
}

static void reset_observed(void)
{
    memset( observed_gpr, 0, sizeof(observed_gpr) );
    memset( observed_vec, 0, sizeof(observed_vec) );
    memset( observed_entry, 0, sizeof(observed_entry) );
    observed_kind = run_calls = capture_ok = 0;
}

void winebox64ec_run_context_returning( void *area, unsigned int entry_kind )
{
    unsigned char *ctx = (unsigned char *)(uintptr_t)load64( area, 0x18 );
    unsigned int i, j;

    ++run_calls;
    if (mode == MODE_CAPTURE)
    {
        capture_ok = entry_kind == 1 && load64( ctx, 0x098 ) == saved_host[0] - 8 &&
                     load64( ctx, 0x0f8 ) == initial_target;
        for (i = 0; i < 15; ++i)
            capture_ok &= load64( ctx, context_gpr_offsets[i] ) == input_gpr[i];
        for (i = 0; i < 16; ++i)
            for (j = 0; j < 16; ++j) capture_ok &= ctx[0x1a0 + i * 16 + j] == input_vec[i][j];
    }
    else capture_ok = entry_kind == (mode == MODE_ENTRY ? 2u : 1u);
    capture_ok &= load16( ctx, 0x038 ) == 0x33;

    for (i = 0; i < 15; ++i) store64( ctx, context_gpr_offsets[i], output_gpr[i] );
    memcpy( ctx + 0x1a0, output_vec, sizeof(output_vec) );
    store64( ctx, 0x098, mode == MODE_ENTRY ? requested_sp : (uint64_t)(guest_stack + 0x80) );
    store64( ctx, 0x0f8, mode == MODE_ENTRY ? requested_target : (uint64_t)&direct_target );
}

static void report( const char *name, int passed )
{
    printf( "%s %s\n", passed ? "PASS" : "FAIL", name );
    failures += !passed;
}

static void set_bitmap( uintptr_t address, int value )
{
    uint64_t bit = (address >> 12) & 63;
    uint64_t *word = &ec_bitmap[address >> 18];
    if (value) *word |= 1ull << bit;
    else *word &= ~(1ull << bit);
}

static void initialize(void)
{
    unsigned int i, j;

    store64( fake_teb, 0x60, (uint64_t)fake_peb );
    store64( fake_teb, 0x1788, (uint64_t)cpu_area );
    store64( fake_peb, 0x368, (uint64_t)ec_bitmap );
    store64( cpu_area, 0x08, (uint64_t)(emulator_stack + sizeof(emulator_stack)) );
    store64( cpu_area, 0x18, (uint64_t)context );
    context[0x038] = 0x33;
    x64_return_instr = &ret_sentinel;
    for (i = 0; i < 15; ++i)
    {
        input_gpr[i] = 0x1100000000000000ull + i * 0x0101010101010101ull;
        output_gpr[i] = 0x8800000000000000ull + i * 0x0011223344556677ull;
    }
    for (i = 0; i < 16; ++i)
        for (j = 0; j < 16; ++j)
        {
            input_vec[i][j] = i * 16 + j;
            output_vec[i][j] = 255 - (i * 16 + j);
        }
}

static void test_capture_restore(void)
{
    mode = MODE_CAPTURE;
    initial_target = (uint64_t)&guest_target;
    reset_observed();
    invoke_dispatch();
    report( "mapped-gpr-xmm-capture", run_calls == 1 && capture_ok );
    report( "mapped-gpr-restore", observed_kind == 1 &&
            !memcmp( observed_gpr, output_gpr, sizeof(output_gpr) ) &&
            observed_gpr[15] == (uint64_t)(guest_stack + 0x80) &&
            observed_gpr[16] == saved_host[6] );
    report( "mapped-xmm-restore", !memcmp( observed_vec, output_vec, sizeof(output_vec) ) );
    report( "direct-continuation-r10", observed_gpr[9] == output_gpr[9] );
}

static void test_bitmap_fast_path(void)
{
    mode = MODE_INVALID;
    initial_target = (uint64_t)&direct_target;
    set_bitmap( initial_target, 1 );
    reset_observed();
    invoke_dispatch();
    report( "native-bitmap-fast-path", run_calls == 0 && observed_kind == 1 &&
            observed_gpr[9] == input_gpr[9] && observed_gpr[15] == saved_host[0] - 8 &&
            observed_gpr[16] == saved_host[6] );
    reset_observed();
    invoke_ret();
    report( "ret-bitmap-no-push", run_calls == 0 && observed_kind == 1 &&
            observed_gpr[9] == input_gpr[9] && observed_gpr[15] == saved_host[0] );
    set_bitmap( initial_target, 0 );
}

static void test_bitmap_bounds(void)
{
    mode = MODE_INVALID;
    initial_target = 1ull << 40;
    reset_observed();
    invoke_dispatch();
    report( "bitmap-39bit-bounds", run_calls == 1 && capture_ok && observed_kind == 1 );
}

static void test_entry_stack( int aligned_after_pop )
{
    uint64_t *slot = (uint64_t *)(guest_stack + (aligned_after_pop ? 0x78 : 0x70));
    uint64_t return_address = 0x123456789abcdef0ull;

    mode = MODE_ENTRY;
    requested_target = (uint64_t)&guest_target;
    requested_sp = (uint64_t)slot;
    *slot = return_address;
    reset_observed();
    invoke_begin();
    if (aligned_after_pop)
    {
        int passed = run_calls == 1 && capture_ok && observed_kind == 2 &&
                     observed_entry[0] == requested_sp + 8 && observed_entry[1] == return_address &&
                     observed_entry[2] == requested_sp + 8 && observed_entry[3] == saved_host[6];
        if (!passed) printf( "INFO entry=%#llx sp=%#llx lr=%#llx x4=%#llx\n",
                             (unsigned long long)requested_sp,
                             (unsigned long long)observed_entry[0],
                             (unsigned long long)observed_entry[1],
                             (unsigned long long)observed_entry[2] );
        report( "entry-stack-pop", passed );
    }
    else
        report( "entry-stack-synthetic-ret", run_calls == 1 && capture_ok && observed_kind == 2 &&
                observed_entry[0] == requested_sp && observed_entry[1] == (uint64_t)&ret_sentinel &&
                observed_entry[2] == requested_sp && observed_entry[3] == saved_host[6] );
}

int main(void)
{
    initialize();
    test_capture_restore();
    test_bitmap_fast_path();
    test_bitmap_bounds();
    test_entry_stack( 1 );
    test_entry_stack( 0 );
    printf( "RESULT %s\n", failures ? "FAIL" : "PASS" );
    return failures != 0;
}
