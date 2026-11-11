#include <stdio.h>
#include <string.h>

#include <switch.h>

#include "carafe_fex_stats.h"

#define FEX_STATS_THREADS 128
#define FEX_STATS_SHOWN 4
#define FEX_STATS_LINE 900

typedef struct {
    u8 version;
    u8 app_type;
    u16 thread_stats_size;
    char fex_version[48];
    u32 head;
    u32 size;
    u32 pad;
} FexStatsHeader;

typedef struct {
    u32 next;
    u32 tid;
    u64 jit_time;
    u64 signal_time;
    u64 sigbus_count;
    u64 smc_count;
    u64 float_fallback_count;
    u64 cache_miss_count;
    u64 cache_read_lock_time;
    u64 cache_write_lock_time;
    u64 jit_count;
    u64 disk_cache_hit_count;
    u64 disk_cache_miss_count;
    u64 disk_cache_lookup_time;
    u64 return_miss_count;
} FexThreadStats;

_Static_assert(sizeof(FexStatsHeader) == 64, "FEX's ThreadStatsHeader");
_Static_assert(sizeof(FexThreadStats) == 112, "FEX's ThreadStats");

extern void* wine_nx_fex_stats_base __attribute__((weak));
extern u32 wine_nx_fex_stats_size __attribute__((weak));
void wine_nx_runtime_trace(const char* msg) __attribute__((weak));

typedef struct {
    u32 tid;
    FexThreadStats seen;
    FexThreadStats delta;
} FexThreadRow;

static FexThreadRow g_rows[FEX_STATS_THREADS];
static unsigned int g_row_count;

static FexThreadRow* rowOf(u32 tid) {
    for (unsigned int i = 0; i < g_row_count; i++) {
        if (g_rows[i].tid == tid) {
            return &g_rows[i];
        }
    }
    if (g_row_count == FEX_STATS_THREADS) {
        return NULL;
    }
    FexThreadRow* row = &g_rows[g_row_count++];
    memset(row, 0, sizeof(*row));
    row->tid = tid;
    return row;
}

static u64 since(u64 now, u64 before) {
    return now >= before ? now - before : 0;
}

static void difference(FexThreadRow* row, const FexThreadStats* now) {
    row->delta.jit_time = since(now->jit_time, row->seen.jit_time);
    row->delta.jit_count = since(now->jit_count, row->seen.jit_count);
    row->delta.cache_miss_count = since(now->cache_miss_count, row->seen.cache_miss_count);
    row->delta.cache_read_lock_time = since(now->cache_read_lock_time, row->seen.cache_read_lock_time);
    row->delta.cache_write_lock_time = since(now->cache_write_lock_time, row->seen.cache_write_lock_time);
    row->delta.smc_count = since(now->smc_count, row->seen.smc_count);
    row->delta.disk_cache_hit_count = since(now->disk_cache_hit_count, row->seen.disk_cache_hit_count);
    row->delta.disk_cache_miss_count = since(now->disk_cache_miss_count, row->seen.disk_cache_miss_count);
    row->delta.disk_cache_lookup_time = since(now->disk_cache_lookup_time, row->seen.disk_cache_lookup_time);
    row->delta.return_miss_count = since(now->return_miss_count, row->seen.return_miss_count);
    row->seen = *now;
}

static unsigned int microseconds(u64 ticks) {
    return (unsigned int)(armTicksToNs(ticks) / 1000);
}

void carafeFexStatsReport(void) {
    if (&wine_nx_fex_stats_base == NULL || wine_nx_runtime_trace == NULL) {
        return;
    }
    const u8* base = __atomic_load_n(&wine_nx_fex_stats_base, __ATOMIC_ACQUIRE);
    u32 size = __atomic_load_n(&wine_nx_fex_stats_size, __ATOMIC_ACQUIRE);
    if (base == NULL || size < sizeof(FexStatsHeader)) {
        return;
    }
    const FexStatsHeader* header = (const FexStatsHeader*)base;
    if (header->thread_stats_size != sizeof(FexThreadStats)) {
        return;
    }
    for (unsigned int i = 0; i < g_row_count; i++) {
        memset(&g_rows[i].delta, 0, sizeof(g_rows[i].delta));
    }
    u32 offset = __atomic_load_n(&header->head, __ATOMIC_ACQUIRE);
    for (unsigned int steps = 0; offset != 0 && steps < FEX_STATS_THREADS; steps++) {
        if (offset < sizeof(FexStatsHeader) || offset > size - sizeof(FexThreadStats)) {
            break;
        }
        FexThreadStats now;
        memcpy(&now, base + offset, sizeof(now));
        if (now.tid != 0) {
            FexThreadRow* row = rowOf(now.tid);
            if (row != NULL) {
                difference(row, &now);
            }
        }
        offset = now.next;
    }
    unsigned int order[FEX_STATS_SHOWN];
    unsigned int shown = 0;
    for (unsigned int i = 0; i < g_row_count; i++) {
        const FexThreadStats* d = &g_rows[i].delta;
        if (d->jit_count == 0 && d->cache_miss_count == 0 && d->smc_count == 0 && d->return_miss_count == 0) {
            continue;
        }
        u64 weight = d->jit_time + d->cache_miss_count;
        unsigned int at = shown < FEX_STATS_SHOWN ? shown++ : FEX_STATS_SHOWN;
        while (at > 0) {
            const FexThreadStats* above = &g_rows[order[at - 1]].delta;
            if (above->jit_time + above->cache_miss_count >= weight) {
                break;
            }
            if (at < FEX_STATS_SHOWN) {
                order[at] = order[at - 1];
            }
            at--;
        }
        if (at < FEX_STATS_SHOWN) {
            order[at] = i;
        }
    }
    char line[FEX_STATS_LINE];
    int used = snprintf(line, sizeof(line), "[FEX] since the last report, by thread:");
    for (unsigned int r = 0; r < shown && used > 0 && (size_t)used < sizeof(line) - 200; r++) {
        const FexThreadRow* row = &g_rows[order[r]];
        const FexThreadStats* d = &row->delta;
        used += snprintf(line + used, sizeof(line) - (size_t)used,
            " %uw compiled %llu blocks in %u us, L1 lookup misses %llu, returns off the call-return stack %llu, "
            "map lock waits %u us read %u us write, "
            "code changes %llu, disk cache %llu hits %llu misses in %u us;",
            row->tid, (unsigned long long)d->jit_count, microseconds(d->jit_time),
            (unsigned long long)d->cache_miss_count, (unsigned long long)d->return_miss_count,
            microseconds(d->cache_read_lock_time),
            microseconds(d->cache_write_lock_time), (unsigned long long)d->smc_count,
            (unsigned long long)d->disk_cache_hit_count, (unsigned long long)d->disk_cache_miss_count,
            microseconds(d->disk_cache_lookup_time));
    }
    if (shown == 0) {
        snprintf(line + used, sizeof(line) - (size_t)used, " nothing compiled or looked up");
    }
    wine_nx_runtime_trace(line);
}
