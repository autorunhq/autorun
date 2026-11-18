/* Carafe's thread orchestrator, the part that decides (carafe-nx/carafe,
 * runtime/orchestrator), GPL-3.0-or-later like Carafe itself. It needs no
 * console: tests/orchestrator.c runs it on the host. */
#ifndef CARAFE_ORCHESTRATOR_POLICY_H
#define CARAFE_ORCHESTRATOR_POLICY_H

#include <stdbool.h>
#include <stddef.h>

#define ORCHESTRATOR_MAX_THREADS 128
#define ORCHESTRATOR_MAX_CORES 32
#define ORCHESTRATOR_DOMINANT_PERMILLE 850
#define ORCHESTRATOR_HEAVY_PERMILLE 300
#define ORCHESTRATOR_SIDE_MIN_PERMILLE 150
#define ORCHESTRATOR_SIDE_SYSTEM_PERMILLE 350
#define ORCHESTRATOR_RELEASE_PERMILLE 600
#define ORCHESTRATOR_GAIN_PERMILLE 50
#define ORCHESTRATOR_STAY_PERMILLE 30
#define ORCHESTRATOR_PEAK_FADE 16
#define ORCHESTRATOR_ENTER_PERIODS 2
#define ORCHESTRATOR_CHANGE_PERIODS 2
#define ORCHESTRATOR_LEAVE_PERIODS 3

typedef enum {
    OrchestratorModeWatch,
    OrchestratorModePlace,
} OrchestratorMode;

typedef struct {
    unsigned int tid;
    char kind;
    unsigned int permille;
    unsigned int mask;
    bool movable;
    bool side;
} OrchestratorThread;

typedef struct {
    unsigned int appMask;
    unsigned int sideMask;
    bool measured;
    unsigned int busyPermille[ORCHESTRATOR_MAX_CORES];
} OrchestratorCores;

typedef struct {
    unsigned int tid;
    int core;
    unsigned int mask;
    bool side;
} OrchestratorMove;

typedef struct {
    unsigned int tid;
    int core;
    unsigned int permille;
} OrchestratorSeat;

typedef struct {
    unsigned int tid;
    bool wasSide;
} OrchestratorCrossing;

typedef struct {
    unsigned int tid;
    unsigned int permille;
} OrchestratorPeak;

typedef struct {
    OrchestratorMode mode;
    OrchestratorPeak peaks[ORCHESTRATOR_MAX_THREADS];
    size_t peakCount;
    OrchestratorSeat seats[ORCHESTRATOR_MAX_THREADS];
    size_t seatCount;
    OrchestratorCrossing crossings[ORCHESTRATOR_MAX_THREADS];
    size_t crossingCount;
    unsigned int busyPeriods;
    unsigned int changePeriods;
    unsigned int lightPeriods;
} OrchestratorState;

typedef struct {
    OrchestratorMode mode;
    bool modeChanged;
    bool seatsChanged;
    unsigned int mainTid;
    int mainCore;
    unsigned int busiestCorePermille;
    unsigned int plannedCorePermille;
    unsigned int corePermille[ORCHESTRATOR_MAX_CORES];
    OrchestratorSeat seats[ORCHESTRATOR_MAX_THREADS];
    size_t seatCount;
    size_t moveCount;
    OrchestratorMove moves[ORCHESTRATOR_MAX_THREADS];
} OrchestratorPlan;

void orchestratorStart(OrchestratorState* state);
void orchestratorDecide(const OrchestratorThread* threads, size_t count, const OrchestratorCores* cores,
    OrchestratorState* state, OrchestratorPlan* plan);
const char* orchestratorModeName(OrchestratorMode mode);

#endif
