#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "carafe_bench.h"
#include "carafe_fex_stats.h"
#include "carafe_settings.h"
#include "carafe_waits.h"

#define NATIVE_KINDS 64
#define BENCH_LOOPS 20000
#define BENCH_REPEATS 32
#define BENCH_REPORT 3
#define TRACE_PIECE 220
#define CORE_COUNT 4
#define PLAY_BENCH_EVERY 10
#define PLAY_BENCH_CORE 0
#define NOMINAL_MHZ 1000

void __real_wine_nx_threads_report_stalled(void);
Result __real_svcSetThreadCoreMask(Handle handle, s32 core, u32 mask);
void __real_wine_nx_thread_report(void);
int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg);
void carafe_diag_dump(void) __attribute__((weak));
void carafe_alert_dump(void) __attribute__((weak));
void wine_nx_runtime_trace(const char* msg) __attribute__((weak));
extern unsigned int wine_nx_vk_presents __attribute__((weak));

static uintptr_t g_native_starts[NATIVE_KINDS];
static unsigned int g_native_counts[NATIVE_KINDS];

void __wrap_wine_nx_threads_report_stalled(void) {
    __real_wine_nx_threads_report_stalled();
    if (carafe_diag_dump) {
        carafe_diag_dump();
    }
    if (carafe_alert_dump) {
        carafe_alert_dump();
    }
}

static unsigned int clockMhz(PcvModuleId module) {
    ClkrstSession session;
    u32 hz = 0;
    if (R_FAILED(clkrstOpenSession(&session, module, 3))) {
        return 0;
    }
    if (R_FAILED(clkrstGetClockRate(&session, &hz))) {
        hz = 0;
    }
    clkrstCloseSession(&session);
    return hz / 1000000;
}

#define BENCH_SETUP                                                                                                  \
    "fmov d0, #1.5\n"                                                                                                \
    "fmov d1, #1.0\n"                                                                                                \
    "fmov d2, #1.0\n"                                                                                                \
    "fmov d3, #1.0\n"                                                                                                \
    "mov x9, %1\n"                                                                                                   \
    "mov x10, #1\n"                                                                                                  \
    "mov x15, #2\n"                                                                                                  \
    "mrs x13, fpcr\n"                                                                                                \
    "eor x14, x13, #0x400000\n"

#define BENCH(fn, body)                                                                                              \
    static u64 fn(u64 loops, u64* buffer) {                                                                          \
        u64 start = armGetSystemTick();                                                                              \
        __asm__ volatile(BENCH_SETUP "1:\n.rept 32\n" body "\n.endr\nsub %0, %0, #1\ncbnz %0, 1b\nmsr fpcr, x13\n" \
                         : "+r"(loops)                                                                               \
                         : "r"(buffer)                                                                               \
                         : "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "v0", "v1", "v2", "v3",      \
                           "cc", "memory");                                                                          \
        return armGetSystemTick() - start;                                                                           \
    }

BENCH(benchAdd, "add x9, x9, #1")
BENCH(benchFadd, "fadd d0, d0, d1")
BENCH(benchFmul, "fmul d0, d0, d1")
BENCH(benchFcvt, "fcvt s0, d0\nfcvt d0, s0")
BENCH(benchFpIntFp, "fcvtzs x9, d0\nscvtf d0, x9")
BENCH(benchFrint, "frinti d0, d0")
BENCH(benchGprFp, "fmov d0, x9\nfmov x9, d0")
BENCH(benchNzcvChain, "mrs x9, nzcv\nmsr nzcv, x9")
BENCH(benchCarryInvert, "mrs x9, nzcv\neor x9, x9, #0x20000000\nmsr nzcv, x9")
BENCH(benchMsrNzcv, "msr nzcv, x10")
BENCH(benchCmpCsel, "cmp x9, x15\ncsel x9, x10, x9, eq")
BENCH(benchStoreLoad64, "str x9, [%1]\nldr x9, [%1]")
BENCH(benchStore64Load32, "str x9, [%1]\nldr w9, [%1]")
BENCH(benchStoreLoad16, "strh w9, [%1]\nldrh w9, [%1]")
BENCH(benchStoreLoad8, "strb w9, [%1]\nldrb w9, [%1]")
BENCH(benchPointerChase, "ldr x9, [x9]")
BENCH(benchFpcrWrite, "msr fpcr, x14\nmsr fpcr, x13")
BENCH(benchRoundPlain, "frinti d1, d0\nfcvtzs x11, d1\nscvtf d0, x11")
BENCH(benchRoundFpcr, "msr fpcr, x14\nfrinti d1, d0\nfcvtzs x11, d1\nmsr fpcr, x13\nscvtf d0, x11")
BENCH(
    benchRoundSelect,
    "frinti d1, d0\nfcvtzs x11, d1\nfrintm d2, d0\nfcvtzs x12, d2\nfrintp d3, d0\nfcvtzs x16, d3\n"
    "fcvtzs x17, d0\nmrs x9, nzcv\ncmp x10, #1\ncsel x11, x12, x11, eq\ncmp x10, #2\ncsel x11, x16, x11, eq\n"
    "cmp x10, #3\ncsel x11, x17, x11, eq\nmsr nzcv, x9\nscvtf d0, x11"
)

typedef struct {
    const char* name;
    u64 (*run)(u64 loops, u64* buffer);
} Bench;

static const Bench BENCHES[] = {
    {"add", benchAdd},
    {"fadd", benchFadd},
    {"fmul", benchFmul},
    {"fcvt-d-s-d", benchFcvt},
    {"fcvtzs+scvtf", benchFpIntFp},
    {"frinti", benchFrint},
    {"fmov-x-d-x", benchGprFp},
    {"mrs+msr-nzcv", benchNzcvChain},
    {"carry-invert", benchCarryInvert},
    {"msr-nzcv", benchMsrNzcv},
    {"cmp+csel", benchCmpCsel},
    {"str+ldr-64", benchStoreLoad64},
    {"str64+ldr32", benchStore64Load32},
    {"strh+ldrh", benchStoreLoad16},
    {"strb+ldrb", benchStoreLoad8},
    {"ldr-chase", benchPointerChase},
    {"msr-fpcr", benchFpcrWrite},
    {"round-plain", benchRoundPlain},
    {"round-fpcr", benchRoundFpcr},
    {"round-select", benchRoundSelect},
};

static void tracePieces(const char* prefix, const char* text) {
    size_t length = strlen(text);
    size_t at = 0;
    do {
        size_t piece = length - at > TRACE_PIECE ? TRACE_PIECE : length - at;
        if (at + piece < length) {
            for (size_t back = piece; back > TRACE_PIECE / 2; back--) {
                if (text[at + back] == ' ') {
                    piece = back;
                    break;
                }
            }
        }
        char line[TRACE_PIECE + 32];
        snprintf(line, sizeof(line), "%s%.*s", prefix, (int)piece, text + at);
        wine_nx_runtime_trace(line);
        at += piece;
    } while (at < length);
}

#define CHASE_STEPS 200000
#define LINE_BYTES 64
#define PAGE_BYTES 4096

static u64 nextRandom(u64* state) {
    *state = *state * 6364136223846793005ULL + 1442695040888963407ULL;
    return *state >> 33;
}

static size_t slotOffset(size_t index, size_t stride) {
    return index * stride + (stride == PAGE_BYTES ? (index % (PAGE_BYTES / LINE_BYTES)) * LINE_BYTES : 0);
}

static double chaseCycles(size_t bytes, size_t stride, unsigned int mhz) {
    size_t count = bytes / stride;
    char* buffer = aligned_alloc(PAGE_BYTES, bytes);
    size_t* order = malloc(count * sizeof(size_t));
    if (buffer == NULL || order == NULL) {
        free(buffer);
        free(order);
        return 0;
    }
    u64 seed = 12345;
    for (size_t i = 0; i < count; i++) {
        order[i] = i;
    }
    for (size_t i = count - 1; i > 0; i--) {
        size_t j = (size_t)(nextRandom(&seed) % i);
        size_t t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (size_t i = 0; i < count; i++) {
        *(void**)(buffer + slotOffset(order[i], stride)) = buffer + slotOffset(order[(i + 1) % count], stride);
    }
    void* p = buffer + slotOffset(order[0], stride);
    for (int i = 0; i < 1000; i++) {
        p = *(void**)p;
    }
    u64 start = armGetSystemTick();
    for (int i = 0; i < CHASE_STEPS; i++) {
        p = *(void**)p;
    }
    u64 ticks = armGetSystemTick() - start;
    __asm__ volatile("" : : "r"(p));
    free(order);
    free(buffer);
    return (double)armTicksToNs(ticks) * mhz / 1000.0 / CHASE_STEPS;
}

static double branchCycles(bool random, unsigned int mhz) {
    static void* const targets[4] = { &&first, &&second, &&third, &&fourth };
    u64 seed = 777;
    u64 sum = 0;
    u64 start = armGetSystemTick();
    for (int i = 0; i < CHASE_STEPS; i++) {
        u64 index = random ? nextRandom(&seed) & 3 : (nextRandom(&seed) & 0);
        goto* targets[index];
    first:
        sum += 1;
        continue;
    second:
        sum += 3;
        continue;
    third:
        sum += 5;
        continue;
    fourth:
        sum += 7;
        continue;
    }
    u64 ticks = armGetSystemTick() - start;
    __asm__ volatile("" : : "r"(sum));
    return (double)armTicksToNs(ticks) * mhz / 1000.0 / CHASE_STEPS;
}

static void runMemoryBenches(unsigned int mhz) {
    char text[512];
    snprintf(text, sizeof(text),
        "memory and branches at %u MHz, cycles per step: load L1 (16 KB) %.1f, L2 (512 KB) %.1f, DRAM (4 MB) %.1f, one line "
        "per 4 KB page over 16 MB %.1f; indirect branch to one target %.1f, to one of four at random %.1f",
        mhz, chaseCycles(16 * 1024, LINE_BYTES, mhz), chaseCycles(512 * 1024, LINE_BYTES, mhz),
        chaseCycles(4 * 1024 * 1024, LINE_BYTES, mhz), chaseCycles(16 * 1024 * 1024, PAGE_BYTES, mhz),
        branchCycles(false, mhz), branchCycles(true, mhz));
    tracePieces("[A57] ", text);
}

static void runPlayMemoryBench(unsigned int mhz) {
    s32 home_core;
    u64 home_mask;
    if (R_FAILED(svcGetThreadCoreMask(&home_core, &home_mask, CUR_THREAD_HANDLE)) ||
        R_FAILED(__real_svcSetThreadCoreMask(CUR_THREAD_HANDLE, PLAY_BENCH_CORE, 1u << PLAY_BENCH_CORE))) {
        return;
    }
    double dram = chaseCycles(4 * 1024 * 1024, LINE_BYTES, mhz);
    double large = chaseCycles(32 * 1024 * 1024, LINE_BYTES, mhz);
    __real_svcSetThreadCoreMask(CUR_THREAD_HANDLE, home_core, (u32)home_mask);
    char text[160];
    snprintf(text, sizeof(text), "memory while the game runs, at %u MHz on core %d, cycles a dependent load: "
        "4 MB %.1f, 32 MB %.1f", mhz, PLAY_BENCH_CORE, dram, large);
    tracePieces("[A57] ", text);
}

static void runBenches(unsigned int mhz) {
    static u64 buffer[8] __attribute__((aligned(64)));
    char text[1024];
    size_t used = (size_t)snprintf(text, sizeof(text), "cycles at %u MHz, one link of a dependent chain:", mhz);
    for (size_t i = 0; i < sizeof(BENCHES) / sizeof(BENCHES[0]) && used < sizeof(text); i++) {
        u64 best = UINT64_MAX;
        for (int attempt = 0; attempt < 3; attempt++) {
            buffer[0] = (u64)(uintptr_t)buffer;
            u64 ticks = BENCHES[i].run(BENCH_LOOPS, buffer);
            best = ticks < best ? ticks : best;
        }
        double cycles = (double)armTicksToNs(best) * mhz / 1000.0 / ((double)BENCH_LOOPS * BENCH_REPEATS);
        used += (size_t)snprintf(text + used, sizeof(text) - used, " %s %.1f", BENCHES[i].name, cycles);
    }
    tracePieces("[A57] ", text);
}

#define GPU_LOAD_IOCTL 0x80044715
#define GPU_SAMPLE_NS 100000000ULL

static u64 g_gpu_load_sum;
static u64 g_gpu_load_count;

static void* sampleGpuLoad(void* arg) {
    u32 fd = (u32)(uintptr_t)arg;
    for (;;) {
        u32 load = 0;
        if (R_FAILED(nvIoctl(fd, GPU_LOAD_IOCTL, &load))) {
            break;
        }
        __atomic_add_fetch(&g_gpu_load_sum, load, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_gpu_load_count, 1, __ATOMIC_RELAXED);
        svcSleepThread(GPU_SAMPLE_NS);
    }
    return NULL;
}

static bool startGpuSampler(void) {
    u32 fd = 0;
    u32 load = 0;
    if (!carafeMeasuring() || R_FAILED(nvInitialize())) {
        return false;
    }
    if (R_FAILED(nvOpen(&fd, "/dev/nvhost-ctrl-gpu"))) {
        nvExit();
        return false;
    }
    pthread_t thread;
    if (R_FAILED(nvIoctl(fd, GPU_LOAD_IOCTL, &load))
        || __real_pthread_create(&thread, NULL, sampleGpuLoad, (void*)(uintptr_t)fd) != 0) {
        nvClose(fd);
        nvExit();
        return false;
    }
    pthread_detach(thread);
    return true;
}

static int gpuLoadPercent(void) {
    static int state;
    if (state == 0) {
        state = startGpuSampler() ? 1 : -1;
        return -1;
    }
    if (state < 0) {
        return -1;
    }
    u64 sum = __atomic_exchange_n(&g_gpu_load_sum, 0, __ATOMIC_RELAXED);
    u64 count = __atomic_exchange_n(&g_gpu_load_count, 0, __ATOMIC_RELAXED);
    return count != 0 ? (int)(sum / count / 10) : -1;
}

static void reportFrames(void) {
    static u64 last_tick;
    static unsigned int last_frames;
    if (&wine_nx_vk_presents == NULL) {
        return;
    }
    u64 now = armGetSystemTick();
    unsigned int frames = __atomic_load_n(&wine_nx_vk_presents, __ATOMIC_RELAXED);
    if (last_tick != 0 && frames != last_frames) {
        u64 ns = armTicksToNs(now - last_tick);
        double fps = (double)(frames - last_frames) * 1e9 / (double)ns;
        char line[96];
        snprintf(line, sizeof(line), "[FPS] %.1f over %.1f s, %u frames", fps, (double)ns / 1e9, frames - last_frames);
        wine_nx_runtime_trace(line);
    }
    last_tick = now;
    last_frames = frames;
}

static unsigned int g_bench_mhz;

static void* benchMain(void* arg) {
    (void)arg;
    runBenches(g_bench_mhz);
    runMemoryBenches(g_bench_mhz);
    carafeBenchCores(g_bench_mhz, tracePieces);
    carafeBenchMemory(g_bench_mhz, tracePieces);
    carafeBenchBranches(g_bench_mhz, tracePieces);
    carafeBenchCode(g_bench_mhz, tracePieces);
    return NULL;
}

static void startBenches(unsigned int mhz) {
    pthread_t thread;
    g_bench_mhz = mhz;
    if (pthread_create(&thread, NULL, benchMain, NULL) == 0) {
        pthread_detach(thread);
    }
}

static bool measuring(void) {
    static int known = -1;
    if (known < 0) {
        known = carafeMeasuring() ? 1 : 0;
    }
    return known == 1;
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

static void reportCoreIdle(void) {
    static u64 last_tick;
    static u64 last_idle[CORE_COUNT];
    static bool last_valid[CORE_COUNT];
    u64 process_mask;
    s32 home_core;
    u64 home_mask;
    if (R_FAILED(svcGetInfo(&process_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) ||
        R_FAILED(svcGetThreadCoreMask(&home_core, &home_mask, CUR_THREAD_HANDLE))) {
        return;
    }
    u64 idle[CORE_COUNT];
    bool valid[CORE_COUNT] = {false};
    for (unsigned int core = 0; core < CORE_COUNT; core++) {
        if ((process_mask & (1ull << core)) != 0) {
            valid[core] = idleTicksOn(core, &idle[core]);
        }
    }
    __real_svcSetThreadCoreMask(CUR_THREAD_HANDLE, home_core, (u32)home_mask);
    u64 now = armGetSystemTick();
    if (last_tick != 0 && now > last_tick) {
        char line[160];
        int used = snprintf(line, sizeof(line), "[CORES] busy, every thread and the system:");
        for (unsigned int core = 0; core < CORE_COUNT && used > 0 && (size_t)used < sizeof(line); core++) {
            if (!valid[core] || !last_valid[core] || idle[core] < last_idle[core]) {
                continue;
            }
            u64 idle_part = (idle[core] - last_idle[core]) * 1000 / (now - last_tick);
            unsigned int busy = idle_part >= 1000 ? 0 : (unsigned int)(1000 - idle_part);
            used += snprintf(line + used, sizeof(line) - (size_t)used, " core %u %u.%u%%", core, busy / 10,
                busy % 10);
        }
        wine_nx_runtime_trace(line);
    }
    last_tick = now;
    for (unsigned int core = 0; core < CORE_COUNT; core++) {
        last_idle[core] = idle[core];
        last_valid[core] = valid[core];
    }
}

void __wrap_wine_nx_thread_report(void) {
    static int ready;
    static Result failed;
    static unsigned int reports;
    __real_wine_nx_thread_report();
    if (wine_nx_runtime_trace == NULL) {
        return;
    }
    reportFrames();
    if (measuring()) {
        reportCoreIdle();
        carafeWaitsEnable();
        carafeWaitsReport();
        carafeFexStatsReport();
    }
    if (++reports == 1) {
        char summary[1024];
        if (carafeOptionsSummary(summary, sizeof(summary))) {
            tracePieces("[OPTIONS] ", summary);
        }
    }
    if (failed) {
        return;
    }
    if (!ready) {
        Result rc = clkrstInitialize();
        if (R_FAILED(rc)) {
            failed = rc;
            char line[64];
            snprintf(line, sizeof(line), "[CLOCKS] unavailable: clkrst %#x", (unsigned int)rc);
            wine_nx_runtime_trace(line);
            return;
        }
        ready = 1;
    }
    unsigned int cpu = clockMhz(PcvModuleId_CpuBus);
    int gpu_load = gpuLoadPercent();
    char load[32] = "";
    if (gpu_load >= 0) {
        snprintf(load, sizeof(load), ", gpu busy %d%%", gpu_load);
    }
    char line[160];
    snprintf(
        line,
        sizeof(line),
        "[CLOCKS] cpu %u MHz, gpu %u MHz, memory %u MHz, %s%s",
        cpu,
        clockMhz(PcvModuleId_GPU),
        clockMhz(PcvModuleId_EMC),
        appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld",
        load
    );
    wine_nx_runtime_trace(line);
    unsigned int mhz = cpu != 0 ? cpu : NOMINAL_MHZ;
    if (reports == BENCH_REPORT && measuring()) {
        startBenches(mhz);
    }
    if (reports > BENCH_REPORT && reports % PLAY_BENCH_EVERY == 0 && measuring()) {
        runPlayMemoryBench(mhz);
    }
}

static uintptr_t runtimeBase(void) {
    static uintptr_t base;
    MemoryInfo info;
    u32 page = 0;
    if (!base && R_SUCCEEDED(svcQueryMemory(&info, &page, (u64)(uintptr_t)&runtimeBase))) {
        base = info.addr;
    }
    return base;
}

static void noteNativeThread(uintptr_t start, uintptr_t caller) {
    unsigned int slot = NATIVE_KINDS;
    for (unsigned int i = 0; i < NATIVE_KINDS; i++) {
        uintptr_t seen = __atomic_load_n(&g_native_starts[i], __ATOMIC_ACQUIRE);
        if (seen == start) {
            slot = i;
            break;
        }
        if (seen == 0) {
            uintptr_t expected = 0;
            if (__atomic_compare_exchange_n(&g_native_starts[i], &expected, start, false, __ATOMIC_ACQ_REL,
                    __ATOMIC_ACQUIRE) || expected == start) {
                slot = i;
                break;
            }
        }
    }
    if (slot == NATIVE_KINDS || wine_nx_runtime_trace == NULL) {
        return;
    }
    unsigned int count = __atomic_add_fetch(&g_native_counts[slot], 1, __ATOMIC_RELAXED);
    if (count & (count - 1)) {
        return;
    }
    uintptr_t base = runtimeBase();
    char line[160];
    snprintf(
        line,
        sizeof(line),
        "[NATIVE] threads started at runtime+0x%lx, from runtime+0x%lx: %u",
        (unsigned long)(start - base),
        (unsigned long)(caller - base),
        count
    );
    wine_nx_runtime_trace(line);
}

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg) {
    noteNativeThread((uintptr_t)start, (uintptr_t)__builtin_return_address(0));
    return __real_pthread_create(thread, attr, start, arg);
}
