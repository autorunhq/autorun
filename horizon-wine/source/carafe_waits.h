#ifndef CARAFE_WAITS_H
#define CARAFE_WAITS_H

#include <switch.h>

typedef enum {
    CarafeWaitLock,
    CarafeWaitSync,
    CarafeWaitServer,
    CarafeWaitKinds,
} CarafeWaitKind;

void carafeWaitsEnable(void);
void carafeWaitNote(CarafeWaitKind kind, u64 ticks);
void carafeWaitsReport(void);

#endif
