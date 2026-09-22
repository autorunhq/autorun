#ifndef HORIZON_VIRTMEM_TEST_SWITCH_H
#define HORIZON_VIRTMEM_TEST_SWITCH_H
#include <stdint.h>
typedef uint32_t u32, Result;
typedef struct VirtmemReservation VirtmemReservation;
typedef struct { uint64_t addr, size; u32 type; } MemoryInfo;
#define MemType_Unmapped 0
#define R_FAILED(rc) ((rc) != 0)
Result svcQueryMemory(MemoryInfo *info, u32 *page_info, uint64_t address);
#endif
