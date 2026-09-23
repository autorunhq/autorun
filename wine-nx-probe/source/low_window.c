#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <switch.h>
#include "low_window.h"

extern VirtmemReservation *__real_virtmemAddReservation( void *address, size_t size );
extern void *__real_virtmemFindAslr( size_t size, size_t guard );
extern void *__real_virtmemFindCodeMemory( size_t size, size_t guard );

static int profile_checked, low_window;
static VirtmemReservation *guest_reservation;

int wine_nx_low_window_reserve(void)
{
    u64 base, size;

    if (!profile_checked)
    {
        if (R_FAILED(svcGetInfo( &base, InfoType_AslrRegionAddress, CUR_PROCESS_HANDLE, 0 )) ||
            R_FAILED(svcGetInfo( &size, InfoType_AslrRegionSize, CUR_PROCESS_HANDLE, 0 ))) return 0;
        low_window = wine_nx_is_low_window( base, size );
        profile_checked = 1;
    }
    if (!low_window) return 1;
    if (!guest_reservation)
        guest_reservation = __real_virtmemAddReservation( (void *)WINE_NX_GUEST_BASE,
                                                         WINE_NX_NATIVE_BASE - WINE_NX_GUEST_BASE );
    return guest_reservation != NULL;
}

void *__wrap_virtmemFindAslr( size_t size, size_t guard )
{
    if (!wine_nx_low_window_reserve()) return NULL;
    return __real_virtmemFindAslr( size, guard );
}

void *__wrap_virtmemFindCodeMemory( size_t size, size_t guard )
{
    if (!wine_nx_low_window_reserve()) return NULL;
    return __real_virtmemFindCodeMemory( size, guard );
}

static int check_native_layout(void)
{
    static const InfoType regions[] = { InfoType_HeapRegionAddress, InfoType_AliasRegionAddress,
                                       InfoType_StackRegionAddress };
    u64 base;
    unsigned int i;

    if ((uintptr_t)check_native_layout < WINE_NX_NATIVE_BASE ||
        (uintptr_t)&base < WINE_NX_NATIVE_BASE || (uintptr_t)armGetTls() < WINE_NX_NATIVE_BASE) return 0;
    for (i = 0; i < sizeof(regions) / sizeof(regions[0]); i++)
        if (R_FAILED(svcGetInfo( &base, regions[i], CUR_PROCESS_HANDLE, 0 )) ||
            base < WINE_NX_NATIVE_BASE) return 0;
    return 1;
}

static int check_guest_window(void)
{
    MemoryInfo info;
    u32 page_info;
    uintptr_t address = WINE_NX_GUEST_BASE;

    while (address < WINE_NX_NATIVE_BASE)
    {
        if (R_FAILED(svcQueryMemory( &info, &page_info, address )) || info.type != MemType_Unmapped ||
            info.addr > address || !info.size || info.size > UINT64_MAX - info.addr ||
            info.addr + info.size <= address) return 0;
        address = info.addr + info.size;
    }
    return 1;
}

static int probe_page( uintptr_t address, int executable, char *message, size_t message_size )
{
    const size_t size = 0x1000;
    Handle process = envGetOwnProcessHandle();
    MemoryInfo info;
    u32 page_info, *backing = memalign( size, size );
    Result rc, unmap_rc;
    int passed = 0;

    if (!backing || (uintptr_t)backing < WINE_NX_NATIVE_BASE)
    {
        free( backing );
        snprintf( message, message_size, "[LOWVA] high backing allocation failed" );
        return 0;
    }
    backing[0] = 0x528009a0; /* mov w0, #77 */
    backing[1] = 0xd65f03c0; /* ret */
    armDCacheFlush( backing, size );
    rc = svcMapProcessCodeMemory( process, address, (u64)backing, size );
    if (R_SUCCEEDED(rc))
    {
        rc = svcSetProcessMemoryPermission( process, address, size, executable ? Perm_Rx : Perm_Rw );
        if (R_SUCCEEDED(rc)) rc = svcQueryMemory( &info, &page_info, address );
        if (R_SUCCEEDED(rc) && info.perm == (u32)(executable ? Perm_Rx : Perm_Rw))
        {
            if (executable)
            {
                armICacheInvalidate( (void *)address, size );
                passed = ((unsigned int (*)(void))address)() == 77;
            }
            else
            {
                *(volatile u32 *)address = 0x12345678;
                passed = *(volatile u32 *)address == 0x12345678;
            }
        }
        unmap_rc = svcUnmapProcessCodeMemory( process, address, (u64)backing, size );
        if (R_FAILED(unmap_rc))
        {
            snprintf( message, message_size, "[LOWVA] unmap %08llx failed: 0x%x",
                      (unsigned long long)address, unmap_rc );
            return 0;
        }
        if (!executable && passed) passed = backing[0] == 0x12345678;
    }
    free( backing );
    snprintf( message, message_size, "[LOWVA] %s at %08llx: %s (rc=0x%x)",
              executable ? "RX execute" : "RW alias", (unsigned long long)address,
              passed ? "PASS" : "FAIL", rc );
    return passed;
}

int wine_nx_low_window_probe( void (*report)( const char * ) )
{
    int reserved, passed;
    char execute_message[192] = "", alias_message[192] = "";

    virtmemLock();
    reserved = wine_nx_low_window_reserve();
    if (!low_window)
    {
        virtmemUnlock();
        report( "[LOWVA] stock layout; fixed-low Win32 titles require the Atmosphere low-address patch" );
        return 0;
    }
    if (!reserved || !check_native_layout() || !check_guest_window())
    {
        virtmemUnlock();
        report( "[LOWVA] FAIL: native layout or guest reservation; reinstall the main forwarder" );
        return 0;
    }
    passed = probe_page( 0x00400000, 1, execute_message, sizeof(execute_message) ) &&
             probe_page( 0xffff0000, 0, alias_message, sizeof(alias_message) );
    passed = passed && check_guest_window();
    virtmemUnlock();
    if (execute_message[0]) report( execute_message );
    if (alias_message[0]) report( alias_message );
    report( passed ? "[LOWVA] PASS: low window reserved for Wine; native allocations above 4 GiB" :
                     "[LOWVA] FAIL: fixed-low Win32 titles cannot start in this process" );
    return passed;
}
