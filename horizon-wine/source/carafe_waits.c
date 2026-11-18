#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <switch.h>

#include "carafe_waits.h"

#define WAIT_SLOTS 256
#define WAIT_LINE 900
#define WAIT_SHOWN 6

typedef struct {
    Handle handle;
    u64 ticks[CarafeWaitKinds];
    u64 count[CarafeWaitKinds];
} WaitSlot;

int __real_horizon_futex_wait(const int* addr, int value, long long timeout_ns);
int __real_horizon_server_sync_call(unsigned int tid, const void* message, const void* data, unsigned int data_size,
    void* reply, void* reply_data);
unsigned int wine_nx_thread_tid(Handle handle, char* kind, char* name, size_t size);
void wine_nx_runtime_trace(const char* msg) __attribute__((weak));

static volatile bool g_enabled;
static WaitSlot g_slots[WAIT_SLOTS];

static WaitSlot* slotOf(Handle handle) {
    unsigned int start = (unsigned int)(handle * 2654435761u) % WAIT_SLOTS;
    for (unsigned int i = 0; i < WAIT_SLOTS; i++) {
        WaitSlot* slot = &g_slots[(start + i) % WAIT_SLOTS];
        Handle seen = __atomic_load_n(&slot->handle, __ATOMIC_ACQUIRE);
        if (seen == handle) {
            return slot;
        }
        if (seen == 0) {
            Handle expected = 0;
            if (__atomic_compare_exchange_n(&slot->handle, &expected, handle, false, __ATOMIC_ACQ_REL,
                    __ATOMIC_ACQUIRE) || expected == handle) {
                return slot;
            }
        }
    }
    return NULL;
}

void carafeWaitsEnable(void) {
    g_enabled = true;
}

void carafeWaitNote(CarafeWaitKind kind, u64 ticks) {
    if (!g_enabled) {
        return;
    }
    WaitSlot* slot = slotOf(threadGetCurHandle());
    if (slot != NULL) {
        __atomic_add_fetch(&slot->ticks[kind], ticks, __ATOMIC_RELAXED);
        __atomic_add_fetch(&slot->count[kind], 1, __ATOMIC_RELAXED);
    }
}

int __wrap_horizon_futex_wait(const int* addr, int value, long long timeout_ns) {
    if (!g_enabled) {
        return __real_horizon_futex_wait(addr, value, timeout_ns);
    }
    u64 start = armGetSystemTick();
    int result = __real_horizon_futex_wait(addr, value, timeout_ns);
    carafeWaitNote(CarafeWaitSync, armGetSystemTick() - start);
    return result;
}

int __wrap_horizon_server_sync_call(unsigned int tid, const void* message, const void* data, unsigned int data_size,
    void* reply, void* reply_data) {
    if (!g_enabled) {
        return __real_horizon_server_sync_call(tid, message, data, data_size, reply, reply_data);
    }
    u64 start = armGetSystemTick();
    int result = __real_horizon_server_sync_call(tid, message, data, data_size, reply, reply_data);
    carafeWaitNote(CarafeWaitServer, armGetSystemTick() - start);
    return result;
}

typedef struct {
    Handle handle;
    u64 total;
    u64 ticks[CarafeWaitKinds];
    u64 count[CarafeWaitKinds];
} WaitRow;

static unsigned int collect(WaitRow* rows) {
    static WaitSlot seen[WAIT_SLOTS];
    unsigned int count = 0;
    for (unsigned int i = 0; i < WAIT_SLOTS; i++) {
        Handle handle = __atomic_load_n(&g_slots[i].handle, __ATOMIC_ACQUIRE);
        if (handle == 0) {
            continue;
        }
        WaitRow* row = &rows[count++];
        memset(row, 0, sizeof(*row));
        row->handle = handle;
        for (int kind = 0; kind < CarafeWaitKinds; kind++) {
            u64 ticks = __atomic_load_n(&g_slots[i].ticks[kind], __ATOMIC_RELAXED);
            u64 calls = __atomic_load_n(&g_slots[i].count[kind], __ATOMIC_RELAXED);
            if (seen[i].handle == handle) {
                row->ticks[kind] = ticks - seen[i].ticks[kind];
                row->count[kind] = calls - seen[i].count[kind];
            }
            seen[i].ticks[kind] = ticks;
            seen[i].count[kind] = calls;
            row->total += row->ticks[kind];
        }
        seen[i].handle = handle;
    }
    return count;
}

void carafeWaitsReport(void) {
    static u64 last_tick;
    static WaitRow rows[WAIT_SLOTS];
    static const char* const names[CarafeWaitKinds] = {"lock", "sync", "server"};
    u64 now = armGetSystemTick();
    unsigned int count = collect(rows);
    u64 interval = last_tick != 0 && now > last_tick ? now - last_tick : 0;
    last_tick = now;
    if (interval == 0 || wine_nx_runtime_trace == NULL) {
        return;
    }
    for (unsigned int i = 0; i < count && i < WAIT_SHOWN; i++) {
        unsigned int best = i;
        for (unsigned int j = i + 1; j < count; j++) {
            if (rows[j].total > rows[best].total) {
                best = j;
            }
        }
        WaitRow swap = rows[i];
        rows[i] = rows[best];
        rows[best] = swap;
    }
    char line[WAIT_LINE];
    int used = snprintf(line, sizeof(line), "[WAITS] share of each thread's time blocked, by kind:");
    for (unsigned int r = 0; r < count && r < WAIT_SHOWN && used > 0 && (size_t)used < sizeof(line) - 160; r++) {
        if (rows[r].total == 0) {
            break;
        }
        char kind = '?';
        char name[32] = "";
        unsigned int tid = wine_nx_thread_tid(rows[r].handle, &kind, name, sizeof(name));
        if (tid != 0) {
            used += snprintf(line + used, sizeof(line) - (size_t)used, " %u%c%s%s", tid, kind, name[0] ? ":" : "",
                name);
        } else {
            used += snprintf(line + used, sizeof(line) - (size_t)used, " native %#x", (unsigned int)rows[r].handle);
        }
        for (int k = 0; k < CarafeWaitKinds; k++) {
            if (rows[r].count[k] == 0) {
                continue;
            }
            u64 permille = rows[r].ticks[k] * 1000 / interval;
            if (permille > 1000) {
                permille = 1000;
            }
            used += snprintf(line + used, sizeof(line) - (size_t)used, " %s %u.%u%% (%llu)", names[k],
                (unsigned int)(permille / 10), (unsigned int)(permille % 10), (unsigned long long)rows[r].count[k]);
        }
        used += snprintf(line + used, sizeof(line) - (size_t)used, ";");
    }
    wine_nx_runtime_trace(line);
}
