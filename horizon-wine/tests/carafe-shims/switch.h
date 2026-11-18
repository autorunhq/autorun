/* What Carafe's reports (source/carafe_waits.c, carafe_lock_boost.c,
 * carafe_fex_stats.c) use of libnx, for tests/carafe_reports.c: a clock the
 * test moves and one thread whose priority it reads back. */
#ifndef CARAFE_REPORTS_TEST_SWITCH_H
#define CARAFE_REPORTS_TEST_SWITCH_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef u32 Handle;
typedef u32 Result;

#define R_FAILED(rc) ((rc) != 0)
#define R_SUCCEEDED(rc) ((rc) == 0)
#define CUR_THREAD_HANDLE 0xffff8000u

extern u64 test_tick;
extern Handle test_thread;
extern s32 test_priority;
extern unsigned int test_priority_sets;

/* 19.2 MHz, the console's counter. */
static inline u64 armGetSystemTick( void ) { return test_tick; }
static inline u64 armTicksToNs( u64 tick ) { return tick * 625 / 12; }
static inline Handle threadGetCurHandle( void ) { return test_thread; }

static inline Result svcGetThreadPriority( s32 *priority, Handle handle )
{
    (void)handle;
    *priority = test_priority;
    return 0;
}

static inline Result svcSetThreadPriority( Handle handle, u32 priority )
{
    (void)handle;
    test_priority = (s32)priority;
    test_priority_sets++;
    return 0;
}

#endif
