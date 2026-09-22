#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { UNIX_STATUS = 0x23456789 };

#define SYSCALL_RESULT UINT64_C(0x7ffd12345678)

uint64_t saved_host[14];
_Alignas(16) unsigned char saved_host_vec[8][16];
uint64_t input_nonvolatile[11], observed_nonvolatile[11];
_Alignas(16) unsigned char input_vec[10][16], observed_vec[10][16];
uint64_t input_args[8], input_stack_args[8];
unsigned int input_syscall_id;
uint64_t unix_inputs[3];
uint64_t observed_state[4];
uint64_t callback_args[3];
unsigned int syscall_x9_returned, unix_lr_returned;
unsigned int dispatch_ret_called;
unsigned char stale_teb[16], authoritative_teb[0x68], fake_peb[0x370];
uint64_t native_bitmap[32];
void *wine_nx_arm64ec_dispatch_ret;

static uint64_t recorded_args[8], recorded_stack_args[8], recorded_stack_ptr;
static uint64_t recorded_unix[3];
static unsigned int recorded_syscall_id;
static unsigned int failures;

extern void invoke_syscall_dispatcher(void);
extern void invoke_unix_dispatcher(void);
extern void invoke_callback(void);
extern void syscall_return_target(void);
extern void syscall_dispatch_ret(void);
extern void clobber_vectors(void);

uint64_t wine_nx_do_syscall( uint64_t *stack_args,
                             uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3,
                             uint64_t x4, uint64_t x5, uint64_t x6, uint64_t x7,
                             unsigned int syscall_id )
{
    uint64_t args[8] = { x0, x1, x2, x3, x4, x5, x6, x7 };

    recorded_stack_ptr = (uint64_t)stack_args;
    memcpy( recorded_args, args, sizeof(args) );
    memcpy( recorded_stack_args, stack_args, sizeof(recorded_stack_args) );
    recorded_syscall_id = syscall_id;
    clobber_vectors();
    return SYSCALL_RESULT;
}

int32_t __wine_unix_call_dispatcher_impl( uint64_t handle, unsigned int index, void *args )
{
    recorded_unix[0] = handle;
    recorded_unix[1] = index;
    recorded_unix[2] = (uint64_t)args;
    clobber_vectors();
    return UNIX_STATUS;
}

void *wine_nx_current_teb(void)
{
    clobber_vectors();
    return authoritative_teb;
}

static void report( const char *name, int passed )
{
    printf( "%s %s\n", passed ? "PASS" : "FAIL", name );
    failures += !passed;
}

static void initialize(void)
{
    uint64_t peb = (uint64_t)fake_peb, bitmap = (uint64_t)native_bitmap;
    unsigned int i, j;

    memcpy( authoritative_teb + 0x60, &peb, sizeof(peb) );
    memcpy( fake_peb + 0x368, &bitmap, sizeof(bitmap) );
    for (i = 0; i < 11; ++i)
        input_nonvolatile[i] = 0x1900000000000000ull + i * 0x0102030405060708ull;
    for (i = 0; i < 10; ++i)
        for (j = 0; j < 16; ++j) input_vec[i][j] = 0x31 + i * 17 + j;
    for (i = 0; i < 8; ++i)
    {
        input_args[i] = 0x4100000000000000ull + i;
        input_stack_args[i] = 0x5100000000000000ull + i;
    }
    input_syscall_id = 0x1abc;
    unix_inputs[0] = 0x6100000000000001ull;
    unix_inputs[1] = 0x762;
    unix_inputs[2] = 0x6100000000000003ull;
}

static void clear_results(void)
{
    memset( observed_nonvolatile, 0, sizeof(observed_nonvolatile) );
    memset( observed_vec, 0, sizeof(observed_vec) );
    memset( observed_state, 0, sizeof(observed_state) );
    syscall_x9_returned = unix_lr_returned = 0;
    dispatch_ret_called = 0;
}

static void run_syscall(void)
{
    clear_results();
    wine_nx_arm64ec_dispatch_ret = syscall_dispatch_ret;
    invoke_syscall_dispatcher();
    if (memcmp( recorded_args, input_args, sizeof(input_args) ) || recorded_syscall_id != input_syscall_id)
        printf( "INFO syscall a0=%#llx/%#llx a7=%#llx/%#llx id=%#x/%#x\n",
                (unsigned long long)recorded_args[0], (unsigned long long)input_args[0],
                (unsigned long long)recorded_args[7], (unsigned long long)input_args[7],
                recorded_syscall_id, input_syscall_id );
    report( "syscall-register-args", !memcmp( recorded_args, input_args, sizeof(input_args) ) &&
            recorded_syscall_id == input_syscall_id );
    report( "syscall-stack-args", recorded_stack_ptr == saved_host[0] - 128 &&
            !memcmp( recorded_stack_args, input_stack_args, sizeof(input_stack_args) ) );
    report( "syscall-nonvolatile-gpr", !memcmp( observed_nonvolatile, input_nonvolatile,
                                                 sizeof(input_nonvolatile) ) );
    report( "syscall-q6-q15", !memcmp( observed_vec, input_vec, sizeof(input_vec) ) );
    report( "syscall-authoritative-x18", observed_state[1] == (uint64_t)authoritative_teb &&
            observed_state[2] == saved_host[0] - 128 );
}

static void test_syscall(void)
{
    uint64_t target = (uint64_t)&syscall_return_target;

    run_syscall();
    report( "syscall-guest-return", observed_state[0] == SYSCALL_RESULT && syscall_x9_returned &&
            dispatch_ret_called && observed_state[3] == target );

    native_bitmap[target >> 18] |= 1ull << ((target >> 12) & 63);
    run_syscall();
    report( "syscall-native-return", observed_state[0] == SYSCALL_RESULT && syscall_x9_returned &&
            !dispatch_ret_called && observed_state[3] == target );
}

static void test_unix(void)
{
    clear_results();
    wine_nx_arm64ec_dispatch_ret = NULL;
    invoke_unix_dispatcher();
    report( "unix-call-args", !memcmp( recorded_unix, unix_inputs, sizeof(unix_inputs) ) );
    report( "unix-status-lr", observed_state[0] == UNIX_STATUS && unix_lr_returned );
    report( "unix-nonvolatile-gpr", !memcmp( observed_nonvolatile, input_nonvolatile,
                                              sizeof(input_nonvolatile) ) );
    report( "unix-q6-q15", !memcmp( observed_vec, input_vec, sizeof(input_vec) ) );
    report( "unix-authoritative-x18", observed_state[1] == (uint64_t)authoritative_teb &&
            observed_state[2] == saved_host[0] );
}

static void test_callback(void)
{
    clear_results();
    invoke_callback();
    report( "callback-args-teb", callback_args[0] == (uintptr_t)input_args && callback_args[1] == 64 &&
            callback_args[2] == (uintptr_t)authoritative_teb );
    report( "callback-result-stack", observed_state[0] == 42 && observed_state[2] == saved_host[0] );
    report( "callback-native-x18", observed_state[1] == (uintptr_t)stale_teb );
    report( "callback-native-nonvolatile", !memcmp( observed_nonvolatile, input_nonvolatile,
                                                    sizeof(input_nonvolatile) ) );
}

int main(void)
{
    initialize();
    test_syscall();
    test_unix();
    test_callback();
    printf( "RESULT %s\n", failures ? "FAIL" : "PASS" );
    return failures != 0;
}
