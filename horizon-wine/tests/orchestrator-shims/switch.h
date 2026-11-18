/* What Carafe's orchestrator (source/orchestrator.c) uses of libnx, for
 * tests/orchestrator_switch.c: four cores whose idle time the test sets, and
 * the calling thread's own core. */
#ifndef ORCHESTRATOR_TEST_SWITCH_H
#define ORCHESTRATOR_TEST_SWITCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;
typedef u32 Handle;
typedef u32 Result;

#define R_FAILED(rc) ((rc) != 0)
#define R_SUCCEEDED(rc) ((rc) == 0)
#define CUR_THREAD_HANDLE 0xffff8000u
#define CUR_PROCESS_HANDLE 0xffff8001u
#define INVALID_HANDLE 0u

typedef enum { InfoType_CoreMask = 0, InfoType_IdleTickCount = 10 } InfoType;

u64 armGetSystemTick( void );
Result svcGetInfo( u64 *out, u32 type, Handle handle, u64 sub );
Result svcGetThreadCoreMask( s32 *core, u64 *mask, Handle handle );
u32 svcGetCurrentProcessorNumber( void );
void svcSleepThread( s64 ns );

#endif
