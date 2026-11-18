/* Carafe's thread orchestrator (carafe-nx/carafe, runtime/orchestrator),
 * GPL-3.0-or-later like Carafe itself. It stands in front of the runtime's
 * balancer: while it only watches, the balancer places threads; when one core
 * is full it lays the busy threads out itself (orchestrator_policy.c).
 *
 * Changed here: whether it is on is asked at every period, not once, because
 * this runtime's balancer already runs while the launcher is on the screen and
 * the setting is the program's. */
#include <switch.h>

#include <stdio.h>
#include <string.h>

#include "carafe_settings.h"
#include "orchestrator_policy.h"

#define CORE_COUNT 4

struct wine_nx_thread_view {
    unsigned int tid;
    char kind;
    char name[32];
    unsigned int permille;
    u64 mask;
    int movable;
    int side;
};

unsigned int wine_nx_thread_views(struct wine_nx_thread_view* views, unsigned int max);
unsigned int wine_nx_thread_side_cores(void);
int wine_nx_thread_place(unsigned int tid, s32 core, u32 mask);
void wine_nx_thread_reserve_core(s32 core);
void wine_nx_runtime_trace(const char* msg) __attribute__((weak));
void __real_wine_nx_thread_balance(void);
Result __real_svcSetThreadCoreMask(Handle handle, s32 core, u32 mask);

static int enabled = -1;
static OrchestratorState state;
static struct wine_nx_thread_view views[ORCHESTRATOR_MAX_THREADS];
static OrchestratorThread threads[ORCHESTRATOR_MAX_THREADS];
static u64 lastTick;
static u64 lastIdle[CORE_COUNT];
static bool lastValid[CORE_COUNT];

static void trace(const char* line) {
    if (wine_nx_runtime_trace) {
        wine_nx_runtime_trace(line);
    }
}

static bool orchestrating(void) {
    int on = carafeOrchestrating() ? 1 : 0;
    if (on && enabled != 1) {
        orchestratorStart(&state);
        lastTick = 0;
        trace("[ORCHESTRATOR] on: lays threads out over the cores by their load and each core's own load");
    }
    if (!on && enabled == 1) {
        wine_nx_thread_reserve_core(-1);
        trace("[ORCHESTRATOR] off: the runtime's balancer places threads");
    }
    enabled = on;
    return on == 1;
}

static bool idleTicksOn(unsigned int core, u64* ticks) {
    if (R_FAILED(__real_svcSetThreadCoreMask(CUR_THREAD_HANDLE, (s32)core, 1u << core))) {
        return false;
    }
    for (int tries = 0; tries < 8 && svcGetCurrentProcessorNumber() != core; tries++) {
        svcSleepThread(0);
    }
    return svcGetCurrentProcessorNumber() == core &&
        R_SUCCEEDED(svcGetInfo(ticks, InfoType_IdleTickCount, INVALID_HANDLE, UINT64_MAX));
}

static void measureCores(OrchestratorCores* cores) {
    s32 homeCore;
    u64 homeMask;
    unsigned int domain = cores->appMask | cores->sideMask;
    if (R_FAILED(svcGetThreadCoreMask(&homeCore, &homeMask, CUR_THREAD_HANDLE))) {
        return;
    }
    u64 idle[CORE_COUNT];
    bool valid[CORE_COUNT] = {false};
    for (unsigned int core = 0; core < CORE_COUNT; core++) {
        if ((domain & (1u << core)) != 0) {
            valid[core] = idleTicksOn(core, &idle[core]);
        }
    }
    __real_svcSetThreadCoreMask(CUR_THREAD_HANDLE, homeCore, (u32)homeMask);
    u64 now = armGetSystemTick();
    bool measured = lastTick != 0 && now > lastTick;
    for (unsigned int core = 0; core < CORE_COUNT; core++) {
        if ((domain & (1u << core)) == 0) {
            continue;
        }
        if (!valid[core] || !lastValid[core] || idle[core] < lastIdle[core]) {
            measured = false;
        } else if (measured) {
            u64 spent = idle[core] - lastIdle[core];
            u64 span = now - lastTick;
            cores->busyPermille[core] = spent >= span ? 0 : (unsigned int)(1000 - spent * 1000 / span);
        }
        lastIdle[core] = idle[core];
        lastValid[core] = valid[core];
    }
    lastTick = now;
    cores->measured = measured;
}

static const char* nameOf(unsigned int tid, unsigned int count) {
    for (unsigned int i = 0; i < count; i++) {
        if (views[i].kind == 'w' && views[i].tid == tid) {
            return views[i].name;
        }
    }
    return "";
}

static int append(char* line, size_t size, int used, const char* text) {
    if (used < 0 || (size_t)used >= size) {
        return used;
    }
    return used + snprintf(line + used, size - (size_t)used, "%s", text);
}

static void traceMode(const OrchestratorPlan* plan, const OrchestratorCores* cores, unsigned int count) {
    char line[640];
    char part[96];
    int used = snprintf(line, sizeof(line), "[ORCHESTRATOR] %s: busiest core %u.%u%%, best layout %u.%u%%%s",
        orchestratorModeName(plan->mode), plan->busiestCorePermille / 10, plan->busiestCorePermille % 10,
        plan->plannedCorePermille / 10, plan->plannedCorePermille % 10, cores->measured ? "" : " (core loads assumed)");
    if (plan->mode != OrchestratorModePlace) {
        append(line, sizeof(line), used, "; the runtime's balancer places threads");
        trace(line);
        return;
    }
    for (int core = 0; core < CORE_COUNT; core++) {
        if (((cores->appMask | cores->sideMask) & (1u << core)) == 0) {
            continue;
        }
        snprintf(part, sizeof(part), "; core %d %u.%u%%:", core, plan->corePermille[core] / 10,
            plan->corePermille[core] % 10);
        used = append(line, sizeof(line), used, part);
        unsigned int shown = 0, more = 0;
        for (size_t i = 0; i < plan->seatCount; i++) {
            const OrchestratorSeat* seat = &plan->seats[i];
            if (seat->core != core) {
                continue;
            }
            if (shown == 3) {
                more++;
                continue;
            }
            const char* name = nameOf(seat->tid, count);
            snprintf(part, sizeof(part), " %uw%s%s %u.%u%%", seat->tid, name[0] ? ":" : "", name,
                seat->permille / 10, seat->permille % 10);
            used = append(line, sizeof(line), used, part);
            shown++;
        }
        if (more != 0) {
            snprintf(part, sizeof(part), " and %u more", more);
            used = append(line, sizeof(line), used, part);
        }
    }
    trace(line);
}

static void applyMoves(const OrchestratorPlan* plan, unsigned int count) {
    for (size_t i = 0; i < plan->moveCount; i++) {
        const OrchestratorMove* move = &plan->moves[i];
        const char* name = nameOf(move->tid, count);
        char line[160];
        bool placed = wine_nx_thread_place(move->tid, move->core, move->mask) != 0;
        snprintf(line, sizeof(line), "[ORCHESTRATOR] %uw%s%s -> cores %#x, core %d first%s%s", move->tid,
            name[0] ? ":" : "", name, move->mask, move->core, move->side ? ", priority 63" : "",
            placed ? "" : ": refused");
        trace(line);
    }
}

void __wrap_wine_nx_thread_balance(void) {
    u64 processMask;
    if (!orchestrating() || R_FAILED(svcGetInfo(&processMask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) {
        __real_wine_nx_thread_balance();
        return;
    }
    OrchestratorCores cores;
    memset(&cores, 0, sizeof(cores));
    cores.appMask = (unsigned int)(processMask & 7);
    cores.sideMask = (unsigned int)(processMask & wine_nx_thread_side_cores());
    measureCores(&cores);
    unsigned int count = wine_nx_thread_views(views, ORCHESTRATOR_MAX_THREADS);
    if (count == 0) {
        __real_wine_nx_thread_balance();
        return;
    }
    for (unsigned int i = 0; i < count; i++) {
        threads[i] = (OrchestratorThread){views[i].tid, views[i].kind, views[i].permille, (unsigned int)views[i].mask,
            views[i].movable != 0, views[i].side != 0};
    }
    OrchestratorPlan plan;
    orchestratorDecide(threads, count, &cores, &state, &plan);
    if (plan.modeChanged || plan.seatsChanged) {
        wine_nx_thread_reserve_core(plan.mode == OrchestratorModePlace ? plan.mainCore : -1);
        traceMode(&plan, &cores, count);
    }
    applyMoves(&plan, count);
    if (plan.mode != OrchestratorModePlace) {
        __real_wine_nx_thread_balance();
    }
}
