#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../source/swap_poc.c"

static VirtmemReservation reservation;
static __thread int thread_marker;
static unsigned int maps, unmaps, faults;
static char exception_stack[128 * 1024];

void *armGetTls(void) { return &thread_marker; }
Result svcGetInfo( uint64_t *size, unsigned int info, Handle process, uint64_t subtype )
{
    (void)info; (void)process; (void)subtype;
    *size = UINT64_C(1) << 39;
    return 0;
}

Result svcMapProcessCodeMemory( Handle process, uint64_t address, uint64_t source, uint64_t size )
{
    (void)process;
    assert( !mprotect( (void *)address, size, PROT_READ | PROT_WRITE ) );
    memcpy( (void *)address, (void *)source, size );
    assert( !mprotect( (void *)source, size, PROT_NONE ) );
    maps++;
    return 0;
}

Result svcUnmapProcessCodeMemory( Handle process, uint64_t address, uint64_t source, uint64_t size )
{
    (void)process;
    assert( !mprotect( (void *)source, size, PROT_READ | PROT_WRITE ) );
    memcpy( (void *)source, (void *)address, size );
    assert( !mprotect( (void *)address, size, PROT_NONE ) );
    assert( !madvise( (void *)address, size, MADV_DONTNEED ) );
    unmaps++;
    return 0;
}

Result svcSetProcessMemoryPermission( Handle process, uint64_t address, uint64_t size, unsigned int perm )
{
    (void)process; (void)address; (void)size;
    assert( perm == Perm_Rw );
    return 0;
}

void *virtmemFindCodeMemory( size_t size, size_t guard )
{
    void *address;
    (void)guard;
    address = mmap( NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
    assert( address != MAP_FAILED );
    return address;
}

VirtmemReservation *virtmemAddReservation( void *address, size_t size )
{
    reservation.address = address;
    reservation.size = size;
    return &reservation;
}

void virtmemRemoveReservation( VirtmemReservation *r )
{
    assert( !munmap( r->address, r->size ) );
    memset( r, 0, sizeof(*r) );
}

static void fault( int signal, siginfo_t *info, void *context )
{
    (void)signal; (void)context;
    faults++;
    assert( wine_nx_swap_fault( (uintptr_t)info->si_addr, (0x24u << 26) | 4u ) == 1 );
}

int wine_nx_fex_exception_attach(void)
{
    struct sigaction action = { .sa_sigaction = fault, .sa_flags = SA_SIGINFO | SA_ONSTACK };
    stack_t stack = { .ss_sp = exception_stack, .ss_size = sizeof(exception_stack) };
    assert( !sigaltstack( &stack, NULL ) );
    sigemptyset( &action.sa_mask );
    assert( !sigaction( SIGSEGV, &action, NULL ) );
    return 0;
}

void wine_nx_fex_exception_detach(void)
{
    stack_t stack = { .ss_flags = SS_DISABLE };
    assert( !sigaltstack( &stack, NULL ) );
    signal( SIGSEGV, SIG_DFL );
}

void wine_nx_runtime_trace( const char *line ) { puts( line ); }

int main(void)
{
    char directory[] = "/tmp/autorun-swap-faults.XXXXXX", path[512], result[512];
    assert( mkdtemp( directory ) );
    snprintf( path, sizeof(path), "%s/missing/swap", directory );
    assert( !wine_nx_swap_test( path, 1024, NULL, NULL, result, sizeof(result) ) );
    assert( strstr( result, "create directory" ) && !busy && !unavailable && !faults );
    assert( wine_nx_swap_test( directory, 1024, NULL, NULL, result, sizeof(result) ) );
    puts( result );
    assert( faults > 100 && maps == unmaps );
    assert( fault_arena == NULL && !busy && !unavailable && !reservation.address );
    assert( !swap_store_remove( directory ) && !rmdir( directory ) );
    puts( "swap POC: real CPU faults, data round trips and alias cleanup passed with mock Horizon SVCs" );
    return 0;
}
