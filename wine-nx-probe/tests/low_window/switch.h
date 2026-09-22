#ifndef LOW_WINDOW_TEST_SWITCH_H
#define LOW_WINDOW_TEST_SWITCH_H
#include <stddef.h>
#include <stdint.h>
typedef uint32_t u32, Result, Handle;
typedef uint64_t u64;
typedef struct VirtmemReservation VirtmemReservation;
typedef struct { u64 addr, size; u32 type, perm; } MemoryInfo;
typedef enum { InfoType_AslrRegionAddress, InfoType_AslrRegionSize, InfoType_HeapRegionAddress,
               InfoType_AliasRegionAddress, InfoType_StackRegionAddress } InfoType;
#define CUR_PROCESS_HANDLE 1
#define MemType_Unmapped 0
#define Perm_Rx 5
#define Perm_Rw 3
#define R_FAILED(rc) ((rc) != 0)
#define R_SUCCEEDED(rc) ((rc) == 0)
Result svcGetInfo(u64 *value, InfoType type, Handle process, u64 sub);
Result svcQueryMemory(MemoryInfo *info, u32 *page_info, u64 address);
Result svcMapProcessCodeMemory(Handle process, u64 address, u64 backing, size_t size);
Result svcSetProcessMemoryPermission(Handle process, u64 address, size_t size, u32 perm);
Result svcUnmapProcessCodeMemory(Handle process, u64 address, u64 backing, size_t size);
void *armGetTls(void);
void armDCacheFlush(void *address, size_t size);
void armICacheInvalidate(void *address, size_t size);
Handle envGetOwnProcessHandle(void);
void virtmemLock(void);
void virtmemUnlock(void);
#endif
