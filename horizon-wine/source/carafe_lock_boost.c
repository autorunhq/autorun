#include <switch.h>

#include "carafe_waits.h"

#define CARAFE_PROGRAM_PRIORITY 59
#define CARAFE_LOCK_WAIT_PRIORITY 58

Result __real_svcArbitrateLock(u32 wait_tag, u32* tag_location, u32 self_tag);

/* Off unless a program's settings ask for it (runtime.c): the wait is timed
 * for the [WAITS] line either way. */
int carafe_lock_boost_enabled;
unsigned long long carafe_lock_boosts;

Result __wrap_svcArbitrateLock(u32 wait_tag, u32* tag_location, u32 self_tag) {
    s32 priority;
    Result rc;
    u64 start = armGetSystemTick();

    if (!__atomic_load_n(&carafe_lock_boost_enabled, __ATOMIC_RELAXED) ||
        R_FAILED(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE)) || priority != CARAFE_PROGRAM_PRIORITY ||
        R_FAILED(svcSetThreadPriority(CUR_THREAD_HANDLE, CARAFE_LOCK_WAIT_PRIORITY))) {
        rc = __real_svcArbitrateLock(wait_tag, tag_location, self_tag);
        carafeWaitNote(CarafeWaitLock, armGetSystemTick() - start);
        return rc;
    }
    __atomic_add_fetch(&carafe_lock_boosts, 1, __ATOMIC_RELAXED);
    rc = __real_svcArbitrateLock(wait_tag, tag_location, self_tag);
    svcSetThreadPriority(CUR_THREAD_HANDLE, CARAFE_PROGRAM_PRIORITY);
    carafeWaitNote(CarafeWaitLock, armGetSystemTick() - start);
    return rc;
}
