#include "swap_poc.h"
#include "swap_pager.h"
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#define BLOCK_SIZE (2 * 1024 * 1024)
#define RESIDENT_BLOCKS 16
#define TEST_BLOCKS 64

extern int wine_nx_fex_exception_attach(void);
extern void wine_nx_fex_exception_detach(void);
extern void wine_nx_runtime_trace( const char *line );

struct swap_arena
{
    struct swap_store store;
    struct swap_pager pager;
    void *memory, *address;
    VirtmemReservation *reservation;
    Handle process;
    jmp_buf recovery;
    Result map_error;
    uint64_t io_ticks, max_fault_ticks;
    int error, cleanup_failed;
};

static struct swap_arena arena;
static __thread struct swap_arena *fault_arena;
static int busy, unavailable;

static void *slot_address( struct swap_arena *a, unsigned int slot )
{
    return (char *)a->memory + (size_t)slot * BLOCK_SIZE;
}

static uintptr_t page_address( struct swap_arena *a, unsigned int page )
{
    return (uintptr_t)a->address + (uint64_t)page * BLOCK_SIZE;
}

static int map_page( void *context, unsigned int page, unsigned int slot )
{
    struct swap_arena *a = context;
    Result rc = svcMapProcessCodeMemory( a->process, page_address( a, page ),
                                        (uintptr_t)slot_address( a, slot ), BLOCK_SIZE );
    if (R_SUCCEEDED(rc))
    {
        rc = svcSetProcessMemoryPermission( a->process, page_address( a, page ), BLOCK_SIZE, Perm_Rw );
        if (R_FAILED(rc) && R_FAILED(svcUnmapProcessCodeMemory( a->process, page_address( a, page ),
                                                              (uintptr_t)slot_address( a, slot ), BLOCK_SIZE )))
            a->cleanup_failed = 1;
    }
    if (R_SUCCEEDED(rc)) return 0;
    a->map_error = rc;
    errno = EIO;
    return -1;
}

static int unmap_page( void *context, unsigned int page, unsigned int slot )
{
    struct swap_arena *a = context;
    Result rc = svcUnmapProcessCodeMemory( a->process, page_address( a, page ),
                                          (uintptr_t)slot_address( a, slot ), BLOCK_SIZE );
    if (R_SUCCEEDED(rc)) return 0;
    a->map_error = rc;
    errno = EIO;
    return -1;
}

static int read_page( void *context, unsigned int page, unsigned int slot )
{
    struct swap_arena *a = context;
    uint64_t start = armGetSystemTick();
    int result = swap_store_read( &a->store, (uint64_t)page * BLOCK_SIZE, slot_address( a, slot ), BLOCK_SIZE );
    a->io_ticks += armGetSystemTick() - start;
    return result;
}

static int write_page( void *context, unsigned int page, unsigned int slot )
{
    struct swap_arena *a = context;
    uint64_t start = armGetSystemTick();
    int result = swap_store_write( &a->store, (uint64_t)page * BLOCK_SIZE, slot_address( a, slot ), BLOCK_SIZE );
    a->io_ticks += armGetSystemTick() - start;
    return result;
}

static void zero_page( void *context, unsigned int slot )
{
    memset( slot_address( context, slot ), 0, BLOCK_SIZE );
}

int wine_nx_swap_fault( uint64_t address, uint32_t esr )
{
    struct swap_arena *a = fault_arena;
    unsigned int exception = esr >> 26;
    uint64_t start, elapsed;
    int failed;

    if (!a || exception != 0x24 || (esr & (1u << 10)) || ((esr & 0x3c) != 0x04) ||
        address < (uintptr_t)a->address || address - (uintptr_t)a->address >= a->store.size)
        return 0;
    start = armGetSystemTick();
    failed = swap_pager_fault( &a->pager, (address - (uintptr_t)a->address) / BLOCK_SIZE );
    if (failed) a->error = errno;
    elapsed = armGetSystemTick() - start;
    if (elapsed > a->max_fault_ticks) a->max_fault_ticks = elapsed;
    if (failed) longjmp( a->recovery, 1 );
    return 1;
}

static uint64_t pattern( unsigned int page, unsigned int word, unsigned int pass )
{
    uint64_t value = ((uint64_t)page << 32) ^ word ^ ((uint64_t)pass << 48) ^ UINT64_C(0xd6e8feb86659fd93);
    value ^= value >> 32;
    value *= UINT64_C(0xd6e8feb86659fd93);
    return value ^ (value >> 32);
}

static int exercise( struct swap_arena *a, swap_progress progress, void *context )
{
    unsigned int pages[TEST_BLOCKS], i, j, pass;
    uint64_t completed = 0, total = (uint64_t)TEST_BLOCKS * BLOCK_SIZE * 4;
    unsigned int last = a->pager.page_count - 1;

    /* Spread the working set across every segment, including the last block. */
    for (i = 0; i < TEST_BLOCKS; i++) pages[i] = (uint64_t)i * last / (TEST_BLOCKS - 1);
    for (pass = 0; pass < 2; pass++)
    {
        for (i = 0; i < TEST_BLOCKS; i++)
        {
            volatile uint64_t *data = (volatile uint64_t *)page_address( a, pages[i] );
            for (j = 0; j < BLOCK_SIZE / sizeof(*data); j++)
            {
                uint64_t expected = pass ? pattern( pages[i], j, pass - 1 ) : 0;
                if (data[j] != expected) { errno = EILSEQ; return -1; }
                data[j] = pattern( pages[i], j, pass );
            }
            completed += BLOCK_SIZE;
            if (progress && !progress( context, "Evicting private pages", completed, total ))
            { errno = ECANCELED; return -1; }
        }
        for (i = 0; i < TEST_BLOCKS; i++)
        {
            unsigned int page = pages[(i * 17) % TEST_BLOCKS];
            volatile uint64_t *data = (volatile uint64_t *)page_address( a, page );
            for (j = 0; j < BLOCK_SIZE / sizeof(*data); j++)
                if (data[j] != pattern( page, j, pass )) { errno = EILSEQ; return -1; }
            completed += BLOCK_SIZE;
            if (progress && !progress( context, "Verifying restored pages", completed, total ))
            { errno = ECANCELED; return -1; }
        }
    }
    return 0;
}

static int run_exercise( struct swap_arena *a, swap_progress progress, void *context )
{
    if (setjmp( a->recovery )) return -1;
    /* This arena is private to its worker, never a Wine, IPC or GPU buffer. */
    fault_arena = a;
    if (!exercise( a, progress, context )) return 0;
    a->error = errno;
    return -1;
}

int wine_nx_swap_test( const char *directory, unsigned int megabytes, swap_progress progress,
                       void *context, char *result, size_t result_size )
{
    static const struct swap_pager_ops ops = { map_page, unmap_page, read_page, write_page, zero_page };
    struct swap_arena *a = &arena;
    uint64_t address_size, faults = 0, reads = 0, writes = 0;
    int attached = 0, pager_ready = 0, passed = 0;
    char line[512];

    if (__atomic_exchange_n( &busy, 1, __ATOMIC_ACQUIRE ))
    { snprintf( result, result_size, "A paging test is already running." ); return 0; }
    if (unavailable)
    {
        snprintf( result, result_size, "Restart Autorun before running another test." );
        __atomic_store_n( &busy, 0, __ATOMIC_RELEASE );
        return 0;
    }
    if (megabytes < 1024 || megabytes > 8192 || (megabytes & 1) ||
        R_FAILED(svcGetInfo( &address_size, InfoType_AslrRegionSize, CUR_PROCESS_HANDLE, 0 )) ||
        address_size <= UINT32_MAX || !(envGetOwnProcessHandle()))
    {
        snprintf( result, result_size, "Use the main 39-bit Autorun forwarder." );
        __atomic_store_n( &busy, 0, __ATOMIC_RELEASE );
        return 0;
    }
    memset( a, 0, sizeof(*a) );
    a->process = envGetOwnProcessHandle();
    snprintf( line, sizeof(line), "[SWAP] preparing capacity=%uMiB path=%s", megabytes, directory );
    wine_nx_runtime_trace( line );
    int store_error = swap_store_open( &a->store, directory, megabytes, progress, context );
    if (!store_error)
    {
        swap_store_close( &a->store );
        store_error = swap_store_open_existing( &a->store, directory, megabytes );
    }
    if (store_error)
    {
        int error = errno;
        snprintf( line, sizeof(line), "[SWAP] file failed operation=%s offset=%llu errno=%d fs=0x%x",
                  a->store.operation, (unsigned long long)a->store.offset, error, a->store.fs_error );
        wine_nx_runtime_trace( line );
        snprintf( result, result_size, "Swap file (%s): %s\nOffset %llu MiB; FS 0x%x",
                  a->store.operation, strerror(error), (unsigned long long)(a->store.offset / 1048576),
                  a->store.fs_error );
        __atomic_store_n( &busy, 0, __ATOMIC_RELEASE );
        return 0;
    }
    wine_nx_runtime_trace( "[SWAP] files ready; starting paging test" );
    if (wine_nx_fex_exception_attach()) { a->error = ENOMEM; goto done; }
    attached = 1;
    a->memory = aligned_alloc( BLOCK_SIZE, (size_t)RESIDENT_BLOCKS * BLOCK_SIZE );
    if (!a->memory) { a->error = ENOMEM; goto done; }
    virtmemLock();
    a->address = virtmemFindCodeMemory( a->store.size, BLOCK_SIZE );
    if (a->address) a->reservation = virtmemAddReservation( a->address, a->store.size );
    virtmemUnlock();
    if (!a->address || !a->reservation) { a->error = ENOMEM; goto done; }
    if (swap_pager_init( &a->pager, a->store.size / BLOCK_SIZE, RESIDENT_BLOCKS, &ops, a ))
    { a->error = errno; goto done; }
    pager_ready = 1;
    passed = !run_exercise( a, progress, context );
    fault_arena = NULL;
    faults = a->pager.faults;
    reads = a->pager.reads;
    writes = a->pager.writes;
    if (!reads || !writes) passed = 0;
done:
    if (pager_ready && swap_pager_close( &a->pager )) a->cleanup_failed = 1;
    if (a->cleanup_failed)
    {
        unavailable = 1;
        passed = 0;
        a->error = EIO;
    }
    else
    {
        if (a->reservation)
        {
            virtmemLock();
            virtmemRemoveReservation( a->reservation );
            virtmemUnlock();
        }
        free( a->memory );
    }
    if (attached) wine_nx_fex_exception_detach();
    swap_store_close( &a->store );
    snprintf( line, sizeof(line),
              "[SWAP] %s capacity=%uMiB touched=128MiB resident=32MiB faults=%llu reads=%llu writes=%llu io_ms=%llu max_fault_ms=%llu errno=%d svc=0x%x cleanup=%s",
              passed ? "PASS" : "FAIL", megabytes, (unsigned long long)faults,
              (unsigned long long)reads, (unsigned long long)writes,
              (unsigned long long)(armTicksToNs( a->io_ticks ) / 1000000),
              (unsigned long long)(armTicksToNs( a->max_fault_ticks ) / 1000000),
              a->error, a->map_error, a->cleanup_failed ? "failed" : "ok" );
    wine_nx_runtime_trace( line );
    if (passed)
        snprintf( result, result_size, "Passed: 128 MiB verified with 32 MiB resident.\n"
                  "Restored %llu blocks from SD; longest fault %llu ms.\nIn-game swap is a separate experimental option.",
                  (unsigned long long)reads, (unsigned long long)(armTicksToNs( a->max_fault_ticks ) / 1000000) );
    else snprintf( result, result_size, "Test failed: %s (SVC 0x%x).%s", strerror(a->error), a->map_error,
                   a->cleanup_failed ? " Restart Autorun; an alias could not be released." : "" );
    __atomic_store_n( &busy, 0, __ATOMIC_RELEASE );
    return passed;
}
