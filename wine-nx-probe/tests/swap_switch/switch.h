#ifndef SWAP_TEST_SWITCH_H
#define SWAP_TEST_SWITCH_H
#include <stdint.h>
#include <stddef.h>
#include <time.h>

typedef unsigned int Result;
typedef unsigned int Handle;
typedef struct VirtmemReservation { void *address; size_t size; } VirtmemReservation;
enum { InfoType_AslrRegionSize, CUR_PROCESS_HANDLE = 1, Perm_Rw = 3 };
#define R_SUCCEEDED(rc) ((rc) == 0)
#define R_FAILED(rc) ((rc) != 0)

Result svcMapProcessCodeMemory( Handle process, uint64_t address, uint64_t source, uint64_t size );
Result svcUnmapProcessCodeMemory( Handle process, uint64_t address, uint64_t source, uint64_t size );
Result svcSetProcessMemoryPermission( Handle process, uint64_t address, uint64_t size, unsigned int perm );
Result svcGetInfo( uint64_t *size, unsigned int info, Handle process, uint64_t subtype );
void *virtmemFindCodeMemory( size_t size, size_t guard );
VirtmemReservation *virtmemAddReservation( void *address, size_t size );
void virtmemRemoveReservation( VirtmemReservation *reservation );
void *armGetTls(void);
static inline Handle envGetOwnProcessHandle(void) { return 1; }
static inline void virtmemLock(void) {}
static inline void virtmemUnlock(void) {}
static inline uint64_t armGetSystemTick(void)
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static inline uint64_t armTicksToNs( uint64_t ticks ) { return ticks; }
#endif
