/* Carafe's thread orchestrator, the part that decides (carafe-nx/carafe,
 * runtime/orchestrator), GPL-3.0-or-later like Carafe itself. It needs no
 * console: tests/orchestrator.c runs it on the host. */
#include "orchestrator_policy.h"

#include <string.h>

static unsigned int clientLoad(const OrchestratorThread* threads, size_t count, size_t index) {
    unsigned int load = threads[index].permille;
    for (size_t i = 0; i < count; i++) {
        if (i != index && threads[i].kind == 's' && threads[i].tid == threads[index].tid) {
            load += threads[i].permille;
        }
    }
    return load;
}

static int singleCore(unsigned int mask) {
    return mask != 0 && (mask & (mask - 1)) == 0 ? __builtin_ctz(mask) : -1;
}

static unsigned int domainOf(const OrchestratorCores* cores) {
    return cores->appMask | cores->sideMask;
}

static bool isSide(const OrchestratorCores* cores, int core) {
    return core >= 0 && (cores->sideMask & (1u << core)) != 0;
}

static bool placeable(const OrchestratorThread* thread, unsigned int domain) {
    return thread->kind == 'w' && thread->movable && (thread->mask == 0 || (thread->mask & domain) != 0);
}

static int currentCore(const OrchestratorThread* thread, unsigned int domain) {
    int core = singleCore(thread->mask);
    return core >= 0 && (domain & (1u << core)) != 0 ? core : -1;
}

static void spread(unsigned int* load, unsigned int mask, unsigned int amount) {
    unsigned int cores = (unsigned int)__builtin_popcount(mask);
    for (int core = 0; core < ORCHESTRATOR_MAX_CORES && cores != 0; core++) {
        if ((mask & (1u << core)) != 0) {
            load[core] += amount / cores;
        }
    }
}

static void measure(const OrchestratorThread* threads, size_t count, const OrchestratorCores* cores,
    unsigned int* base, unsigned int* current) {
    unsigned int domain = domainOf(cores);
    unsigned int moving[ORCHESTRATOR_MAX_CORES] = {0};
    unsigned int staying[ORCHESTRATOR_MAX_CORES] = {0};
    for (size_t i = 0; i < count; i++) {
        if (threads[i].kind != 'w') {
            continue;
        }
        unsigned int load = clientLoad(threads, count, i);
        unsigned int mask = threads[i].mask != 0 ? threads[i].mask & domain : domain;
        unsigned int* into = placeable(&threads[i], domain) ? moving : staying;
        spread(into, mask, load);
    }
    for (int core = 0; core < ORCHESTRATOR_MAX_CORES; core++) {
        if ((domain & (1u << core)) == 0) {
            base[core] = 0;
            current[core] = 0;
        } else if (cores->measured) {
            unsigned int busy = cores->busyPermille[core];
            base[core] = busy > moving[core] ? busy - moving[core] : 0;
            current[core] = busy;
        } else {
            base[core] = staying[core] + (isSide(cores, core) ? ORCHESTRATOR_SIDE_SYSTEM_PERMILLE : 0);
            current[core] = base[core] + moving[core];
        }
    }
}

static unsigned int highest(const unsigned int* load, unsigned int mask) {
    unsigned int top = 0;
    for (int core = 0; core < ORCHESTRATOR_MAX_CORES; core++) {
        if ((mask & (1u << core)) != 0 && load[core] > top) {
            top = load[core];
        }
    }
    return top;
}

static bool visitor(const OrchestratorState* state, unsigned int tid) {
    for (size_t i = 0; i < state->crossingCount; i++) {
        if (state->crossings[i].tid == tid) {
            return !state->crossings[i].wasSide;
        }
    }
    return false;
}

static unsigned int peakOf(const OrchestratorState* state, unsigned int tid) {
    for (size_t i = 0; i < state->peakCount; i++) {
        if (state->peaks[i].tid == tid) {
            return state->peaks[i].permille;
        }
    }
    return 0;
}

static void updatePeaks(const OrchestratorThread* threads, size_t count, OrchestratorState* state) {
    OrchestratorPeak peaks[ORCHESTRATOR_MAX_THREADS];
    size_t kept = 0;
    for (size_t i = 0; i < count && kept < ORCHESTRATOR_MAX_THREADS; i++) {
        if (threads[i].kind != 'w') {
            continue;
        }
        unsigned int load = clientLoad(threads, count, i);
        unsigned int old = peakOf(state, threads[i].tid);
        unsigned int faded = old - old / ORCHESTRATOR_PEAK_FADE;
        peaks[kept++] = (OrchestratorPeak){threads[i].tid, load > faded ? load : faded};
    }
    memcpy(state->peaks, peaks, sizeof(peaks[0]) * kept);
    state->peakCount = kept;
}

static bool allowed(const OrchestratorCores* cores, const OrchestratorState* state, int core,
    const OrchestratorThread* thread, unsigned int load, bool heaviest) {
    if (!isSide(cores, core)) {
        return (cores->appMask & (1u << core)) != 0;
    }
    if (thread->side && !visitor(state, thread->tid)) {
        return !heaviest || cores->appMask == 0;
    }
    return !heaviest && load >= ORCHESTRATOR_SIDE_MIN_PERMILLE && load < ORCHESTRATOR_HEAVY_PERMILLE &&
        peakOf(state, thread->tid) < ORCHESTRATOR_HEAVY_PERMILLE;
}

static bool crowdedVisitor(const OrchestratorThread* threads, size_t count, const OrchestratorCores* cores,
    const OrchestratorState* state) {
    unsigned int domain = domainOf(cores);
    for (size_t i = 0; i < count; i++) {
        if (placeable(&threads[i], domain) && isSide(cores, currentCore(&threads[i], domain)) &&
            visitor(state, threads[i].tid) && clientLoad(threads, count, i) >= ORCHESTRATOR_HEAVY_PERMILLE) {
            return true;
        }
    }
    return false;
}

static size_t assign(const OrchestratorThread* threads, size_t count, const OrchestratorCores* cores,
    const OrchestratorState* state, const unsigned int* base, OrchestratorSeat* seats, unsigned int* total) {
    unsigned int domain = domainOf(cores);
    size_t order[ORCHESTRATOR_MAX_THREADS];
    unsigned int loads[ORCHESTRATOR_MAX_THREADS];
    size_t found = 0;
    for (size_t i = 0; i < count && found < ORCHESTRATOR_MAX_THREADS; i++) {
        if (!placeable(&threads[i], domain)) {
            continue;
        }
        unsigned int load = clientLoad(threads, count, i);
        size_t at = found++;
        while (at > 0 && (loads[at - 1] < load || (loads[at - 1] == load && threads[order[at - 1]].tid >
            threads[i].tid))) {
            order[at] = order[at - 1];
            loads[at] = loads[at - 1];
            at--;
        }
        order[at] = i;
        loads[at] = load;
    }
    for (int core = 0; core < ORCHESTRATOR_MAX_CORES; core++) {
        total[core] = base[core];
    }
    size_t seated = 0;
    for (size_t n = 0; n < found; n++) {
        const OrchestratorThread* thread = &threads[order[n]];
        unsigned int load = loads[n];
        int best = -1;
        for (int core = 0; core < ORCHESTRATOR_MAX_CORES; core++) {
            if ((domain & (1u << core)) == 0 || !allowed(cores, state, core, thread, load, n == 0)) {
                continue;
            }
            if (best < 0 || total[core] < total[best]) {
                best = core;
            }
        }
        if (best < 0) {
            continue;
        }
        int stay = currentCore(thread, domain);
        if (stay >= 0 && stay != best && allowed(cores, state, stay, thread, load, n == 0) &&
            total[stay] <= total[best] + ORCHESTRATOR_STAY_PERMILLE) {
            best = stay;
        }
        total[best] += load;
        seats[seated++] = (OrchestratorSeat){thread->tid, best, load};
    }
    return seated;
}

static const OrchestratorThread* findThread(const OrchestratorThread* threads, size_t count, unsigned int tid) {
    for (size_t i = 0; i < count; i++) {
        if (threads[i].kind == 'w' && threads[i].tid == tid) {
            return &threads[i];
        }
    }
    return NULL;
}

static bool differs(const OrchestratorThread* threads, size_t count, const OrchestratorSeat* seats, size_t seated,
    unsigned int domain) {
    for (size_t i = 0; i < seated; i++) {
        const OrchestratorThread* thread = findThread(threads, count, seats[i].tid);
        if (thread != NULL && currentCore(thread, domain) != seats[i].core) {
            return true;
        }
    }
    return false;
}

static void addMove(OrchestratorPlan* plan, unsigned int tid, int core, unsigned int mask, bool side) {
    if (plan->moveCount < ORCHESTRATOR_MAX_THREADS) {
        plan->moves[plan->moveCount++] = (OrchestratorMove){tid, core, mask, side};
    }
}

static void noteCrossing(OrchestratorState* state, unsigned int tid, bool fromSide, bool toSide) {
    for (size_t i = 0; i < state->crossingCount; i++) {
        if (state->crossings[i].tid == tid) {
            if (state->crossings[i].wasSide == toSide) {
                state->crossings[i] = state->crossings[--state->crossingCount];
            }
            return;
        }
    }
    if (fromSide != toSide && state->crossingCount < ORCHESTRATOR_MAX_THREADS) {
        state->crossings[state->crossingCount++] = (OrchestratorCrossing){tid, fromSide};
    }
}

static void adopt(const OrchestratorThread* threads, size_t count, const OrchestratorCores* cores,
    const OrchestratorSeat* seats, size_t seated, OrchestratorState* state, OrchestratorPlan* plan) {
    for (size_t i = 0; i < seated; i++) {
        const OrchestratorThread* thread = findThread(threads, count, seats[i].tid);
        if (thread == NULL || thread->mask == (1u << seats[i].core)) {
            continue;
        }
        bool toSide = isSide(cores, seats[i].core);
        addMove(plan, seats[i].tid, seats[i].core, 1u << seats[i].core, toSide);
        noteCrossing(state, seats[i].tid, thread->side, toSide);
    }
    memcpy(state->seats, seats, sizeof(seats[0]) * seated);
    state->seatCount = seated;
}

static int firstCore(unsigned int mask) {
    return mask != 0 ? __builtin_ctz(mask) : -1;
}

static void leave(const OrchestratorThread* threads, size_t count, const OrchestratorCores* cores,
    OrchestratorState* state, OrchestratorPlan* plan) {
    for (size_t i = 0; i < state->crossingCount; i++) {
        if (findThread(threads, count, state->crossings[i].tid) == NULL) {
            continue;
        }
        if (state->crossings[i].wasSide && cores->sideMask != 0) {
            int core = firstCore(cores->sideMask);
            addMove(plan, state->crossings[i].tid, core, 1u << core, true);
        } else if (!state->crossings[i].wasSide) {
            addMove(plan, state->crossings[i].tid, firstCore(cores->appMask), cores->appMask, false);
        }
    }
    OrchestratorPeak peaks[ORCHESTRATOR_MAX_THREADS];
    size_t peakCount = state->peakCount;
    memcpy(peaks, state->peaks, sizeof(peaks[0]) * peakCount);
    orchestratorStart(state);
    memcpy(state->peaks, peaks, sizeof(peaks[0]) * peakCount);
    state->peakCount = peakCount;
    plan->modeChanged = true;
}

void orchestratorStart(OrchestratorState* state) {
    memset(state, 0, sizeof(*state));
    state->mode = OrchestratorModeWatch;
}

static void tidyNewThreads(const OrchestratorThread* threads, size_t count, const OrchestratorCores* cores,
    const unsigned int* current, OrchestratorPlan* plan) {
    unsigned int domain = domainOf(cores);
    for (size_t i = 0; i < count; i++) {
        const OrchestratorThread* thread = &threads[i];
        if (!placeable(thread, domain) || currentCore(thread, domain) >= 0) {
            continue;
        }
        unsigned int choice = (thread->mask != 0 ? thread->mask : domain) & cores->appMask;
        int best = -1;
        for (int core = 0; core < ORCHESTRATOR_MAX_CORES; core++) {
            if ((choice & (1u << core)) != 0 && (best < 0 || current[core] < current[best])) {
                best = core;
            }
        }
        if (best >= 0) {
            addMove(plan, thread->tid, best, 1u << best, false);
        }
    }
}

static void describe(const OrchestratorState* state, const OrchestratorCores* cores, const unsigned int* base,
    OrchestratorPlan* plan) {
    unsigned int domain = domainOf(cores);
    for (int core = 0; core < ORCHESTRATOR_MAX_CORES; core++) {
        plan->corePermille[core] = (domain & (1u << core)) != 0 ? base[core] : 0;
    }
    for (size_t i = 0; i < state->seatCount; i++) {
        plan->corePermille[state->seats[i].core] += state->seats[i].permille;
    }
    memcpy(plan->seats, state->seats, sizeof(state->seats[0]) * state->seatCount);
    plan->seatCount = state->seatCount;
    if (state->seatCount != 0) {
        plan->mainTid = state->seats[0].tid;
        plan->mainCore = state->seats[0].core;
    }
}

void orchestratorDecide(const OrchestratorThread* threads, size_t count, const OrchestratorCores* cores,
    OrchestratorState* state, OrchestratorPlan* plan) {
    memset(plan, 0, sizeof(*plan));
    plan->mainCore = -1;

    unsigned int domain = domainOf(cores);
    unsigned int base[ORCHESTRATOR_MAX_CORES];
    unsigned int current[ORCHESTRATOR_MAX_CORES];
    unsigned int total[ORCHESTRATOR_MAX_CORES];
    OrchestratorSeat seats[ORCHESTRATOR_MAX_THREADS];
    updatePeaks(threads, count, state);
    measure(threads, count, cores, base, current);
    size_t seated = assign(threads, count, cores, state, base, seats, total);
    unsigned int now = highest(current, domain);
    unsigned int planned = highest(total, domain);
    bool moves = differs(threads, count, seats, seated, domain);
    bool better = now >= ORCHESTRATOR_DOMINANT_PERMILLE && planned + ORCHESTRATOR_GAIN_PERMILLE <= now && moves;
    bool urgent = moves && crowdedVisitor(threads, count, cores, state);
    plan->busiestCorePermille = now;
    plan->plannedCorePermille = planned;

    if (state->mode == OrchestratorModeWatch) {
        state->busyPeriods = better ? state->busyPeriods + 1 : 0;
        if (state->busyPeriods >= ORCHESTRATOR_ENTER_PERIODS && seated != 0) {
            adopt(threads, count, cores, seats, seated, state, plan);
            state->mode = OrchestratorModePlace;
            state->busyPeriods = 0;
            state->changePeriods = 0;
            state->lightPeriods = 0;
            plan->modeChanged = true;
            plan->seatsChanged = true;
        }
    } else {
        state->lightPeriods = now < ORCHESTRATOR_RELEASE_PERMILLE ? state->lightPeriods + 1 : 0;
        if (state->lightPeriods >= ORCHESTRATOR_LEAVE_PERIODS) {
            leave(threads, count, cores, state, plan);
        } else if (better || urgent) {
            if (++state->changePeriods >= ORCHESTRATOR_CHANGE_PERIODS || urgent) {
                adopt(threads, count, cores, seats, seated, state, plan);
                state->changePeriods = 0;
                plan->seatsChanged = true;
            }
        } else {
            state->changePeriods = 0;
            tidyNewThreads(threads, count, cores, current, plan);
        }
    }

    plan->mode = state->mode;
    if (state->mode == OrchestratorModePlace) {
        describe(state, cores, base, plan);
    }
}

const char* orchestratorModeName(OrchestratorMode mode) {
    return mode == OrchestratorModePlace ? "placing" : "watching";
}
