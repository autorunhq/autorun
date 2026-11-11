#include "orchestrator_policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t parseThreads(const char* line, OrchestratorThread* threads) {
    const char* p = strstr(line, "cores:");
    size_t count = 0;
    if (p == NULL) {
        return 0;
    }
    p += strlen("cores:");
    while (*p != '\0' && count < ORCHESTRATOR_MAX_THREADS) {
        while (*p == ' ') {
            p++;
        }
        char* end;
        unsigned long tid = strtoul(p, &end, 10);
        if (end == p) {
            break;
        }
        char kind = *end;
        const char* at = strchr(end, '@');
        if (at == NULL) {
            break;
        }
        long core = strtol(at + 1, &end, 10);
        double percent = strtod(end, &end);
        unsigned int mask = core >= 0 ? 1u << core : 7u;
        threads[count++] = (OrchestratorThread){(unsigned int)tid, kind, (unsigned int)(percent * 10 + 0.5), mask,
            kind == 'w' && core >= 0, core == 3};
        p = end;
        while (*p == '%') {
            p++;
        }
    }
    return count;
}

static bool parseCores(const char* line, OrchestratorCores* cores) {
    const char* p = strstr(line, "system:");
    if (p == NULL) {
        return false;
    }
    memset(cores, 0, sizeof(*cores));
    cores->appMask = 7;
    cores->sideMask = 8;
    while ((p = strstr(p, "core ")) != NULL) {
        char* end;
        long core = strtol(p + 5, &end, 10);
        double percent = strtod(end, &end);
        if (core >= 0 && core < ORCHESTRATOR_MAX_CORES) {
            cores->busyPermille[core] = (unsigned int)(percent * 10 + 0.5);
        }
        p = end;
    }
    cores->measured = true;
    return true;
}

static void addLoads(const OrchestratorThread* threads, size_t count, unsigned int* load, bool add) {
    for (size_t i = 0; i < count; i++) {
        if (threads[i].kind != 'w' || threads[i].mask == 0 || (threads[i].mask & (threads[i].mask - 1)) != 0) {
            continue;
        }
        unsigned int amount = threads[i].permille;
        for (size_t j = 0; j < count; j++) {
            if (threads[j].kind == 's' && threads[j].tid == threads[i].tid) {
                amount += threads[j].permille;
            }
        }
        unsigned int* core = &load[__builtin_ctz(threads[i].mask)];
        *core = add ? *core + amount : (*core > amount ? *core - amount : 0);
    }
}

static void applyMoves(OrchestratorThread* threads, size_t count, const OrchestratorMove* placed, size_t placedCount) {
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < placedCount; j++) {
            if (threads[i].kind == 'w' && threads[i].tid == placed[j].tid) {
                threads[i].mask = placed[j].mask;
                threads[i].side = placed[j].side;
            }
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s autorun_runtime.log\n", argv[0]);
        return 2;
    }
    FILE* file = fopen(argv[1], "r");
    if (file == NULL) {
        perror(argv[1]);
        return 2;
    }
    OrchestratorState state;
    orchestratorStart(&state);
    OrchestratorMove placed[ORCHESTRATOR_MAX_THREADS];
    size_t placedCount = 0;
    OrchestratorThread threads[ORCHESTRATOR_MAX_THREADS];
    OrchestratorThread logged[ORCHESTRATOR_MAX_THREADS];
    size_t count = 0;
    char line[8192];
    unsigned int number = 0, reports = 0, placing = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        number++;
        if (strncmp(line, "[THREADS]", 9) == 0) {
            count = parseThreads(line, threads);
            memcpy(logged, threads, sizeof(threads[0]) * count);
            applyMoves(threads, count, placed, placedCount);
            continue;
        }
        OrchestratorCores cores;
        if (count == 0 || strncmp(line, "[CORES] busy", 12) != 0 || !parseCores(line, &cores)) {
            continue;
        }
        addLoads(logged, count, cores.busyPermille, false);
        addLoads(threads, count, cores.busyPermille, true);
        OrchestratorPlan plan;
        orchestratorDecide(threads, count, &cores, &state, &plan);
        reports++;
        if (plan.mode == OrchestratorModePlace) {
            placing++;
        }
        if (plan.modeChanged || plan.seatsChanged) {
            printf("%u: %s, busiest core %u.%u%%, planned %u.%u%%;", number, orchestratorModeName(plan.mode),
                plan.busiestCorePermille / 10, plan.busiestCorePermille % 10, plan.plannedCorePermille / 10,
                plan.plannedCorePermille % 10);
            for (int core = 0; core < 4 && plan.mode == OrchestratorModePlace; core++) {
                printf(" core %d %u.%u%%", core, plan.corePermille[core] / 10, plan.corePermille[core] % 10);
            }
            printf("\n");
        }
        if (plan.modeChanged && plan.mode == OrchestratorModeWatch) {
            placedCount = 0;
        }
        for (size_t i = 0; i < plan.moveCount; i++) {
            printf("%u:   move %u to core %d, mask %#x%s\n", number, plan.moves[i].tid, plan.moves[i].core,
                plan.moves[i].mask, plan.moves[i].side ? ", priority 63" : "");
            size_t j = 0;
            while (j < placedCount && placed[j].tid != plan.moves[i].tid) {
                j++;
            }
            if (j == placedCount && placedCount < ORCHESTRATOR_MAX_THREADS) {
                placedCount++;
            }
            placed[j] = plan.moves[i];
        }
        applyMoves(threads, count, placed, placedCount);
    }
    fclose(file);
    printf("%u reports, %u placing\n", reports, placing);
    return 0;
}
