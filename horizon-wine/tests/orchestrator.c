#include "orchestrator_policy.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            printf("%s:%d: %s\n", __FILE__, __LINE__, #condition);              \
            failures++;                                                         \
        }                                                                       \
    } while (0)

static OrchestratorThread program(unsigned int tid, unsigned int permille, unsigned int mask) {
    return (OrchestratorThread){tid, 'w', permille, mask, true, false};
}

static OrchestratorThread server(unsigned int tid, unsigned int permille, unsigned int mask) {
    return (OrchestratorThread){tid, 's', permille, mask, false, false};
}

static OrchestratorThread graphics(unsigned int tid, unsigned int permille) {
    return (OrchestratorThread){tid, 'w', permille, 8, true, true};
}

static OrchestratorThread pinned(unsigned int tid, unsigned int permille, unsigned int mask) {
    return (OrchestratorThread){tid, 'w', permille, mask, false, false};
}

static OrchestratorCores busy(const OrchestratorThread* threads, size_t count, unsigned int system) {
    OrchestratorCores cores;
    memset(&cores, 0, sizeof(cores));
    cores.appMask = 7;
    cores.sideMask = 8;
    cores.measured = true;
    cores.busyPermille[3] = system;
    for (size_t i = 0; i < count; i++) {
        unsigned int mask = threads[i].mask;
        unsigned int n = (unsigned int)__builtin_popcount(mask);
        for (int core = 0; core < 4 && n != 0; core++) {
            if ((mask & (1u << core)) != 0) {
                cores.busyPermille[core] += threads[i].permille / n;
            }
        }
    }
    return cores;
}

static OrchestratorCores appOnly(void) {
    OrchestratorCores cores;
    memset(&cores, 0, sizeof(cores));
    cores.appMask = 7;
    return cores;
}

static void decide(OrchestratorThread* threads, size_t count, unsigned int system, OrchestratorState* state,
    OrchestratorPlan* plan) {
    OrchestratorCores cores = busy(threads, count, system);
    orchestratorDecide(threads, count, &cores, state, plan);
}

static const OrchestratorMove* moveOf(const OrchestratorPlan* plan, unsigned int tid) {
    for (size_t i = 0; i < plan->moveCount; i++) {
        if (plan->moves[i].tid == tid) {
            return &plan->moves[i];
        }
    }
    return NULL;
}

static void apply(OrchestratorThread* threads, size_t count, const OrchestratorPlan* plan) {
    for (size_t i = 0; i < plan->moveCount; i++) {
        for (size_t j = 0; j < count; j++) {
            if (threads[j].kind == 'w' && threads[j].tid == plan->moves[i].tid) {
                threads[j].mask = plan->moves[i].mask;
                threads[j].side = plan->moves[i].side;
            }
        }
    }
}

static void openWorld(OrchestratorThread* threads) {
    OrchestratorThread world[] = {
        program(4, 760, 2), graphics(44, 620), program(48, 550, 4), program(52, 260, 1), program(40, 80, 1),
        program(36, 40, 1), server(4, 8, 2),
    };
    memcpy(threads, world, sizeof(world));
}

static void theGraphicsWorkerLeavesACrowdedSystemCore(void) {
    OrchestratorThread threads[7];
    openWorld(threads);
    OrchestratorState state;
    OrchestratorPlan plan;
    orchestratorStart(&state);
    decide(threads, 7, 370, &state, &plan);
    CHECK(plan.mode == OrchestratorModeWatch && plan.moveCount == 0);
    CHECK(plan.busiestCorePermille == 990);
    decide(threads, 7, 370, &state, &plan);
    CHECK(plan.mode == OrchestratorModePlace && plan.modeChanged && plan.seatsChanged);
    const OrchestratorMove* cs = moveOf(&plan, 44);
    CHECK(cs != NULL && cs->core == 0 && cs->mask == 1 && !cs->side);
    const OrchestratorMove* frontEnd = moveOf(&plan, 52);
    CHECK(frontEnd != NULL && frontEnd->core == 3 && frontEnd->side);
    CHECK(moveOf(&plan, 4) == NULL && moveOf(&plan, 48) == NULL);
    CHECK(plan.mainTid == 4 && plan.mainCore == 1);
    CHECK(plan.plannedCorePermille < 800);
}

static void aPlacedLayoutStaysPut(void) {
    OrchestratorThread threads[7];
    openWorld(threads);
    OrchestratorState state;
    OrchestratorPlan plan;
    orchestratorStart(&state);
    decide(threads, 7, 370, &state, &plan);
    decide(threads, 7, 370, &state, &plan);
    apply(threads, 7, &plan);
    for (int period = 0; period < 5; period++) {
        decide(threads, 7, 370, &state, &plan);
        CHECK(plan.mode == OrchestratorModePlace && !plan.modeChanged && !plan.seatsChanged && plan.moveCount == 0);
    }
}

static void nothingToGainStaysWatching(void) {
    OrchestratorThread threads[] = {
        program(4, 680, 2), program(48, 970, 4), program(52, 650, 1), graphics(44, 560), program(40, 80, 1),
    };
    OrchestratorState state;
    OrchestratorPlan plan;
    orchestratorStart(&state);
    for (int period = 0; period < 4; period++) {
        decide(threads, 5, 350, &state, &plan);
        CHECK(plan.mode == OrchestratorModeWatch && plan.moveCount == 0);
    }
}

static void aGameThreadNeverTakesTheSystemCoreWhenBusy(void) {
    OrchestratorThread threads[] = {program(4, 950, 1), program(8, 900, 1), program(40, 60, 2)};
    OrchestratorState state;
    OrchestratorPlan plan;
    orchestratorStart(&state);
    decide(threads, 3, 300, &state, &plan);
    decide(threads, 3, 300, &state, &plan);
    CHECK(plan.mode == OrchestratorModePlace);
    for (size_t i = 0; i < plan.moveCount; i++) {
        CHECK(plan.moves[i].core != 3 && !plan.moves[i].side);
    }
    const OrchestratorMove* second = moveOf(&plan, 8);
    CHECK(second != NULL && second->core != 0 && second->core != 3);
}

static void smallThreadsStayOffTheSystemCore(void) {
    OrchestratorThread threads[] = {
        program(4, 900, 1), program(8, 120, 1), program(12, 100, 1), program(16, 90, 1), program(20, 80, 1),
    };
    OrchestratorState state;
    OrchestratorPlan plan;
    orchestratorStart(&state);
    decide(threads, 5, 300, &state, &plan);
    decide(threads, 5, 300, &state, &plan);
    CHECK(plan.mode == OrchestratorModePlace);
    for (size_t i = 0; i < plan.moveCount; i++) {
        CHECK(plan.moves[i].core != 3);
    }
    CHECK(moveOf(&plan, 4) == NULL);
}

static void leavingPutsCrossedThreadsBack(void) {
    OrchestratorThread threads[7];
    openWorld(threads);
    OrchestratorState state;
    OrchestratorPlan plan;
    orchestratorStart(&state);
    decide(threads, 7, 370, &state, &plan);
    decide(threads, 7, 370, &state, &plan);
    apply(threads, 7, &plan);
    for (size_t i = 0; i < 7; i++) {
        threads[i].permille /= 4;
    }
    decide(threads, 7, 200, &state, &plan);
    decide(threads, 7, 200, &state, &plan);
    CHECK(plan.mode == OrchestratorModePlace);
    decide(threads, 7, 200, &state, &plan);
    CHECK(plan.mode == OrchestratorModeWatch && plan.modeChanged);
    const OrchestratorMove* cs = moveOf(&plan, 44);
    CHECK(cs != NULL && cs->core == 3 && cs->side);
    const OrchestratorMove* frontEnd = moveOf(&plan, 52);
    CHECK(frontEnd != NULL && frontEnd->mask == 7 && !frontEnd->side);
}

static void aServerThreadCountsForItsClient(void) {
    OrchestratorThread threads[] = {
        program(4, 820, 1), server(4, 60, 1), program(8, 380, 1), server(8, 30, 1), program(12, 50, 2),
    };
    OrchestratorState state;
    OrchestratorPlan plan;
    orchestratorStart(&state);
    decide(threads, 5, 300, &state, &plan);
    decide(threads, 5, 300, &state, &plan);
    CHECK(plan.mode == OrchestratorModePlace && plan.seatCount >= 2);
    CHECK(plan.seats[0].tid == 4 && plan.seats[0].permille == 880 && plan.seats[1].permille == 410);
    const OrchestratorMove* second = moveOf(&plan, 8);
    CHECK(second != NULL && second->core != 0 && second->core != 3);
}

static void aThreadTheProgramPlacedIsLeftAlone(void) {
    OrchestratorThread threads[] = {program(4, 960, 1), pinned(12, 400, 1), program(40, 70, 1)};
    OrchestratorState state;
    OrchestratorPlan plan;
    orchestratorStart(&state);
    decide(threads, 3, 300, &state, &plan);
    decide(threads, 3, 300, &state, &plan);
    CHECK(plan.mode == OrchestratorModePlace && moveOf(&plan, 12) == NULL);
    const OrchestratorMove* main = moveOf(&plan, 4);
    CHECK(main != NULL && main->core != 0 && main->core != 3);
}

static void withoutMeasurementsTheSystemShareIsAssumed(void) {
    OrchestratorThread threads[7];
    openWorld(threads);
    OrchestratorCores cores = appOnly();
    cores.sideMask = 8;
    OrchestratorState state;
    OrchestratorPlan plan;
    orchestratorStart(&state);
    orchestratorDecide(threads, 7, &cores, &state, &plan);
    CHECK(plan.busiestCorePermille == ORCHESTRATOR_SIDE_SYSTEM_PERMILLE + 620);
    orchestratorDecide(threads, 7, &cores, &state, &plan);
    CHECK(plan.mode == OrchestratorModePlace);
    const OrchestratorMove* cs = moveOf(&plan, 44);
    CHECK(cs != NULL && !cs->side);
}

static void aNewThreadWhilePlacingGetsTheQuietestCore(void) {
    OrchestratorThread threads[8];
    openWorld(threads);
    OrchestratorState state;
    OrchestratorPlan plan;
    orchestratorStart(&state);
    decide(threads, 7, 370, &state, &plan);
    decide(threads, 7, 370, &state, &plan);
    apply(threads, 7, &plan);
    threads[7] = program(120, 30, 7);
    decide(threads, 8, 370, &state, &plan);
    CHECK(plan.mode == OrchestratorModePlace && !plan.seatsChanged);
    const OrchestratorMove* fresh = moveOf(&plan, 120);
    CHECK(fresh != NULL && fresh->core == 2 && fresh->mask == 4);
}

static void aBetterLayoutWaitsTwoPeriods(void) {
    OrchestratorThread threads[7];
    openWorld(threads);
    OrchestratorState state;
    OrchestratorPlan plan;
    orchestratorStart(&state);
    decide(threads, 7, 370, &state, &plan);
    decide(threads, 7, 370, &state, &plan);
    apply(threads, 7, &plan);
    threads[2].permille = 900;
    decide(threads, 7, 370, &state, &plan);
    CHECK(plan.mode == OrchestratorModePlace && !plan.seatsChanged && plan.busiestCorePermille >= 850);
    decide(threads, 7, 370, &state, &plan);
    CHECK(plan.mode == OrchestratorModePlace && plan.seatsChanged && plan.moveCount != 0);
}

static void aMainThreadQuietWhileLoadingStaysOffTheSystemCore(void) {
    OrchestratorThread menu[] = {
        program(4, 990, 2), graphics(44, 80), program(48, 50, 4), program(52, 50, 1), program(40, 30, 4),
    };
    OrchestratorThread loading[] = {
        program(4, 283, 2), graphics(44, 682), program(48, 444, 4), program(52, 416, 1), program(40, 59, 4),
    };
    OrchestratorState state;
    OrchestratorPlan plan;
    orchestratorStart(&state);
    decide(menu, 5, 300, &state, &plan);
    decide(menu, 5, 300, &state, &plan);
    CHECK(plan.mode == OrchestratorModeWatch);
    decide(loading, 5, 300, &state, &plan);
    decide(loading, 5, 300, &state, &plan);
    CHECK(plan.mode == OrchestratorModePlace);
    const OrchestratorMove* main = moveOf(&plan, 4);
    CHECK(main == NULL || (main->core != 3 && !main->side));
    for (size_t i = 0; i < plan.seatCount; i++) {
        CHECK(plan.seats[i].tid != 4 || plan.seats[i].core != 3);
    }
}

static void aVisitorThatGetsBusyLeavesTheSystemCoreAtOnce(void) {
    OrchestratorThread threads[7];
    openWorld(threads);
    OrchestratorState state;
    OrchestratorPlan plan;
    orchestratorStart(&state);
    decide(threads, 7, 370, &state, &plan);
    decide(threads, 7, 370, &state, &plan);
    apply(threads, 7, &plan);
    CHECK(threads[3].side && threads[3].mask == 8);
    threads[3].permille = 520;
    decide(threads, 7, 370, &state, &plan);
    CHECK(plan.mode == OrchestratorModePlace && plan.seatsChanged);
    const OrchestratorMove* frontEnd = moveOf(&plan, 52);
    CHECK(frontEnd != NULL && frontEnd->core != 3 && !frontEnd->side);
}

static void oneCoreStaysWatching(void) {
    OrchestratorThread threads[] = {program(4, 960, 1), program(40, 70, 1)};
    OrchestratorCores cores = appOnly();
    cores.appMask = 1;
    cores.measured = true;
    cores.busyPermille[0] = 1000;
    OrchestratorState state;
    OrchestratorPlan plan;
    orchestratorStart(&state);
    for (int period = 0; period < 3; period++) {
        orchestratorDecide(threads, 2, &cores, &state, &plan);
        CHECK(plan.mode == OrchestratorModeWatch && plan.moveCount == 0);
    }
}

int main(void) {
    theGraphicsWorkerLeavesACrowdedSystemCore();
    aPlacedLayoutStaysPut();
    nothingToGainStaysWatching();
    aGameThreadNeverTakesTheSystemCoreWhenBusy();
    smallThreadsStayOffTheSystemCore();
    leavingPutsCrossedThreadsBack();
    aServerThreadCountsForItsClient();
    aThreadTheProgramPlacedIsLeftAlone();
    withoutMeasurementsTheSystemShareIsAssumed();
    aNewThreadWhilePlacingGetsTheQuietestCore();
    aBetterLayoutWaitsTwoPeriods();
    aMainThreadQuietWhileLoadingStaysOffTheSystemCore();
    aVisitorThatGetsBusyLeavesTheSystemCoreAtOnce();
    oneCoreStaysWatching();
    if (failures != 0) {
        printf("%d checks failed\n", failures);
        return 1;
    }
    printf("orchestrator policy: all checks passed\n");
    return 0;
}
