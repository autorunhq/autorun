#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "carafe_bench.h"

#define HANDOFF_ROUNDS 2000
#define BUSY_ROUNDS 4
#define ATOMIC_ROUNDS 200000
#define UNCACHED_BYTES (256 * 1024)
#define LINE_BYTES 64
#define BRANCH_STEPS 200000
#define WORKER_PRIORITY 58
#define GAME_PRIORITY 59
#define HOME_CORE 2
#define BUSY_CORE 0
#define TEST_LIMIT_MS 250
#define BUSY_LIMIT_MS 60
#define WAIT_SLICE_NS 1000000
#define TICKS_PER_MS 19200

Result __real_svcSetThreadCoreMask(Handle handle, s32 core, u32 mask);

typedef struct {
    unsigned int core;
    s32 priority;
    bool sleep;
    volatile u32* word;
    u32 rounds;
    u64 deadline;
    volatile bool ready;
} Partner;

typedef struct {
    unsigned int core;
    volatile bool* stop;
    u64 deadline;
    volatile bool ready;
} Spinner;

typedef struct {
    unsigned int core;
    volatile u64* counter;
    u32 rounds;
    volatile bool* go;
    u64 deadline;
    volatile bool ready;
} Adder;

static volatile u32 g_word __attribute__((aligned(LINE_BYTES)));
static volatile u64 g_counter __attribute__((aligned(LINE_BYTES)));

static double cycles(u64 ticks, unsigned int mhz, u64 count) {
    return count == 0 ? 0 : (double)armTicksToNs(ticks) * mhz / 1000.0 / (double)count;
}

static u64 deadlineIn(unsigned int ms) {
    return armGetSystemTick() + (u64)ms * TICKS_PER_MS;
}

static bool past(u64 deadline) {
    return armGetSystemTick() > deadline;
}

static bool pinTo(unsigned int core) {
    if (R_FAILED(__real_svcSetThreadCoreMask(CUR_THREAD_HANDLE, (s32)core, 1u << core))) {
        return false;
    }
    for (int tries = 0; tries < 8 && svcGetCurrentProcessorNumber() != core; tries++) {
        svcSleepThread(0);
    }
    return svcGetCurrentProcessorNumber() == core;
}

static bool waitFor(volatile u32* word, u32 value, bool sleep, u64 deadline) {
    unsigned int spins = 0;
    while (__atomic_load_n(word, __ATOMIC_ACQUIRE) != value) {
        if (sleep) {
            svcWaitForAddress((void*)word, ArbitrationType_WaitIfEqual, (s64)(value - 1), WAIT_SLICE_NS);
        }
        if ((sleep || (++spins & 1023) == 0) && past(deadline)) {
            return false;
        }
    }
    return true;
}

static void post(volatile u32* word, u32 value, bool sleep) {
    __atomic_store_n(word, value, __ATOMIC_RELEASE);
    if (sleep) {
        svcSignalToAddress((void*)word, SignalType_Signal, 0, 1);
    }
}

static void* partnerMain(void* arg) {
    Partner* partner = arg;
    pinTo(partner->core);
    svcSetThreadPriority(CUR_THREAD_HANDLE, partner->priority);
    partner->ready = true;
    for (u32 i = 0; i < partner->rounds; i++) {
        if (!waitFor(partner->word, 2 * i + 1, partner->sleep, partner->deadline)) {
            break;
        }
        post(partner->word, 2 * i + 2, partner->sleep);
    }
    return NULL;
}

static void* spinnerMain(void* arg) {
    Spinner* spinner = arg;
    pinTo(spinner->core);
    svcSetThreadPriority(CUR_THREAD_HANDLE, GAME_PRIORITY);
    spinner->ready = true;
    unsigned int spins = 0;
    while (!*spinner->stop && ((++spins & 1023) != 0 || !past(spinner->deadline))) {
    }
    return NULL;
}

static bool started(volatile bool* ready, u64 deadline) {
    while (!*ready) {
        if (past(deadline)) {
            return false;
        }
        svcSleepThread(1000000);
    }
    return true;
}

static double handoff(unsigned int core, bool sleep, s32 priority, u32 rounds, unsigned int mhz, bool busy) {
    u64 deadline = deadlineIn(busy ? BUSY_LIMIT_MS : TEST_LIMIT_MS);
    volatile bool stop = false;
    Spinner spinner = {core, &stop, deadline, false};
    pthread_t spinning;
    if (busy && pthread_create(&spinning, NULL, spinnerMain, &spinner) != 0) {
        return -1;
    }
    g_word = 0;
    Partner partner = {core, priority, sleep, &g_word, rounds, deadline, false};
    pthread_t thread;
    double result = -1;
    if (pthread_create(&thread, NULL, partnerMain, &partner) == 0) {
        if (started(&partner.ready, deadline) && (!busy || started(&spinner.ready, deadline))) {
            u64 start = armGetSystemTick();
            u32 done = 0;
            while (done < rounds) {
                post(&g_word, 2 * done + 1, sleep);
                if (!waitFor(&g_word, 2 * done + 2, sleep, deadline)) {
                    break;
                }
                done++;
            }
            if (done == rounds) {
                result = cycles(armGetSystemTick() - start, mhz, 2ull * rounds);
            }
        }
        post(&g_word, UINT32_MAX, sleep);
        pthread_join(thread, NULL);
    }
    if (busy) {
        stop = true;
        pthread_join(spinning, NULL);
    }
    return result;
}

static void* adderMain(void* arg) {
    Adder* adder = arg;
    pinTo(adder->core);
    svcSetThreadPriority(CUR_THREAD_HANDLE, WORKER_PRIORITY);
    adder->ready = true;
    unsigned int spins = 0;
    while (!*adder->go) {
        if ((++spins & 1023) == 0 && past(adder->deadline)) {
            return NULL;
        }
    }
    for (u32 i = 0; i < adder->rounds; i++) {
        __atomic_fetch_add(adder->counter, 1, __ATOMIC_ACQ_REL);
    }
    return NULL;
}

static double contended(int other, unsigned int mhz) {
    u64 deadline = deadlineIn(TEST_LIMIT_MS);
    volatile bool go = false;
    g_counter = 0;
    Adder adder = {(unsigned int)(other < 0 ? HOME_CORE : other), &g_counter, ATOMIC_ROUNDS, &go, deadline, false};
    pthread_t thread;
    bool together = other >= 0;
    if (together && pthread_create(&thread, NULL, adderMain, &adder) != 0) {
        return -1;
    }
    if (together && !started(&adder.ready, deadline)) {
        go = true;
        pthread_join(thread, NULL);
        return -1;
    }
    u64 start = armGetSystemTick();
    go = true;
    for (u32 i = 0; i < ATOMIC_ROUNDS; i++) {
        __atomic_fetch_add(&g_counter, 1, __ATOMIC_ACQ_REL);
    }
    u64 ticks = armGetSystemTick() - start;
    if (together) {
        pthread_join(thread, NULL);
    }
    return cycles(ticks, mhz, ATOMIC_ROUNDS);
}

static void appendCycles(char* text, size_t size, const char* label, double value, int decimals) {
    size_t used = strlen(text);
    if (value < 0) {
        snprintf(text + used, size - used, "%s no answer", label);
    } else {
        snprintf(text + used, size - used, "%s %.*f", label, decimals, value);
    }
}

void carafeBenchCores(unsigned int mhz, CarafeBenchTrace trace) {
    s32 home_core;
    u64 home_mask;
    s32 home_priority;
    if (R_FAILED(svcGetThreadCoreMask(&home_core, &home_mask, CUR_THREAD_HANDLE)) ||
        R_FAILED(svcGetThreadPriority(&home_priority, CUR_THREAD_HANDLE)) || !pinTo(HOME_CORE)) {
        return;
    }
    svcSetThreadPriority(CUR_THREAD_HANDLE, WORKER_PRIORITY);
    double spin_0 = handoff(0, false, WORKER_PRIORITY, HANDOFF_ROUNDS, mhz, false);
    double spin_3 = handoff(3, false, WORKER_PRIORITY, HANDOFF_ROUNDS, mhz, false);
    double sleep_same = handoff(HOME_CORE, true, WORKER_PRIORITY, HANDOFF_ROUNDS, mhz, false);
    double sleep_0 = handoff(0, true, WORKER_PRIORITY, HANDOFF_ROUNDS, mhz, false);
    double sleep_3 = handoff(3, true, WORKER_PRIORITY, HANDOFF_ROUNDS, mhz, false);
    double busy_59 = handoff(BUSY_CORE, true, GAME_PRIORITY, BUSY_ROUNDS, mhz, true);
    double busy_58 = handoff(BUSY_CORE, true, WORKER_PRIORITY, BUSY_ROUNDS, mhz, true);
    double alone = contended(-1, mhz);
    double with_0 = contended(0, mhz);
    double with_3 = contended(3, mhz);
    __real_svcSetThreadCoreMask(CUR_THREAD_HANDLE, home_core, (u32)home_mask);
    svcSetThreadPriority(CUR_THREAD_HANDLE, home_priority);
    char text[640];
    snprintf(text, sizeof(text), "between cores at %u MHz, cycles one way: spinning on a line,", mhz);
    appendCycles(text, sizeof(text), " core 2 to 0", spin_0, 0);
    appendCycles(text, sizeof(text), ", 2 to 3", spin_3, 0);
    appendCycles(text, sizeof(text), "; waking a sleeping thread, 2 to 2", sleep_same, 0);
    appendCycles(text, sizeof(text), ", 2 to 0", sleep_0, 0);
    appendCycles(text, sizeof(text), ", 2 to 3", sleep_3, 0);
    appendCycles(text, sizeof(text), "; waking one on a core busy at 59, at 59", busy_59, 0);
    appendCycles(text, sizeof(text), ", at 58", busy_58, 0);
    appendCycles(text, sizeof(text), "; an atomic increment alone", alone, 1);
    appendCycles(text, sizeof(text), ", with core 0", with_0, 1);
    appendCycles(text, sizeof(text), ", with core 3", with_3, 1);
    trace("[A57] ", text);
}

static u32* lineRing(u8* buffer, size_t bytes) {
    size_t count = bytes / LINE_BYTES;
    u32* order = malloc(count * sizeof(u32));
    if (order == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        order[i] = (u32)i;
    }
    u64 seed = 4242;
    for (size_t i = count - 1; i > 0; i--) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        size_t j = (size_t)((seed >> 33) % i);
        u32 t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (size_t i = 0; i < count; i++) {
        *(u32*)(buffer + (size_t)order[i] * LINE_BYTES) = order[(i + 1) % count] * LINE_BYTES;
    }
    return order;
}

typedef struct {
    double line_write;
    double word_write;
    double dependent_read;
    double stream_read;
} MemoryCosts;

static MemoryCosts memoryCosts(u8* buffer, size_t bytes, unsigned int mhz) {
    MemoryCosts costs = {0, 0, 0, 0};
    size_t lines = bytes / LINE_BYTES;
    u64 start = armGetSystemTick();
    for (size_t i = 0; i < lines; i++) {
        u64* line = (u64*)(buffer + i * LINE_BYTES);
        for (int w = 0; w < LINE_BYTES / 8; w++) {
            line[w] = i + (u64)w;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    costs.line_write = cycles(armGetSystemTick() - start, mhz, lines);
    u64 seed = 99;
    start = armGetSystemTick();
    for (size_t i = 0; i < lines; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        *(volatile u32*)(buffer + ((seed >> 33) % (bytes / 4)) * 4) = (u32)i;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    costs.word_write = cycles(armGetSystemTick() - start, mhz, lines);
    u32* order = lineRing(buffer, bytes);
    if (order != NULL) {
        __asm__ volatile("dsb sy" ::: "memory");
        u32 at = order[0] * LINE_BYTES;
        size_t steps = lines < 4096 ? lines : 4096;
        start = armGetSystemTick();
        for (size_t i = 0; i < steps; i++) {
            at = *(volatile u32*)(buffer + at);
        }
        costs.dependent_read = cycles(armGetSystemTick() - start, mhz, steps);
        __asm__ volatile("" : : "r"(at));
        free(order);
    }
    u64 sum = 0;
    start = armGetSystemTick();
    for (size_t i = 0; i < bytes / 8; i++) {
        sum += ((volatile u64*)buffer)[i];
    }
    costs.stream_read = cycles(armGetSystemTick() - start, mhz, lines);
    __asm__ volatile("" : : "r"(sum));
    return costs;
}

void carafeBenchMemory(unsigned int mhz, CarafeBenchTrace trace) {
    u8* buffer = aligned_alloc(0x1000, UNCACHED_BYTES);
    if (buffer == NULL) {
        return;
    }
    memset(buffer, 0, UNCACHED_BYTES);
    MemoryCosts cached = memoryCosts(buffer, UNCACHED_BYTES, mhz);
    memset(buffer, 0xa5, UNCACHED_BYTES);
    u64 start = armGetSystemTick();
    armDCacheClean(buffer, UNCACHED_BYTES);
    double clean = cycles(armGetSystemTick() - start, mhz, UNCACHED_BYTES / LINE_BYTES);
    armDCacheFlush(buffer, UNCACHED_BYTES);
    Result rc = svcSetMemoryAttribute(buffer, UNCACHED_BYTES, MemAttr_IsUncached, MemAttr_IsUncached);
    char text[512];
    if (R_FAILED(rc)) {
        snprintf(text, sizeof(text), "uncached memory unavailable: %#x; cleaning a dirty line %.1f cycles",
            (unsigned int)rc, clean);
        trace("[A57] ", text);
        free(buffer);
        return;
    }
    MemoryCosts uncached = memoryCosts(buffer, UNCACHED_BYTES, mhz);
    svcSetMemoryAttribute(buffer, UNCACHED_BYTES, MemAttr_IsUncached, 0);
    free(buffer);
    snprintf(text, sizeof(text),
        "memory as DXVK maps it at %u MHz, cycles cached / uncached: writing a whole line %.1f / %.1f, one word "
        "at random %.1f / %.1f, a dependent read %.1f / %.1f, reading a line in order %.1f / %.1f; cleaning a "
        "dirty line %.1f",
        mhz, cached.line_write, uncached.line_write, cached.word_write, uncached.word_write, cached.dependent_read,
        uncached.dependent_read, cached.stream_read, uncached.stream_read, clean);
    trace("[A57] ", text);
}

static double periodic(unsigned int period, unsigned int mhz) {
    static void* const targets[4] = {&&first, &&second, &&third, &&fourth};
    static const unsigned char pattern[8] = {0, 2, 1, 3, 3, 1, 0, 2};
    u64 sum = 0;
    u64 seed = 7;
    unsigned int step = 0;
    u64 start = armGetSystemTick();
    for (int i = 0; i < BRANCH_STEPS; i++) {
        unsigned int index;
        if (period == 0) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            index = (unsigned int)(seed >> 33) & 3;
        } else {
            index = period == 8 ? pattern[step] : step & 3;
            step = step + 1 == period ? 0 : step + 1;
        }
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
    return cycles(ticks, mhz, BRANCH_STEPS);
}

static double compareChain(unsigned int mhz) {
    u64 sum = 0;
    u64 seed = 7;
    u64 start = armGetSystemTick();
    for (int i = 0; i < BRANCH_STEPS; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        unsigned int index = (unsigned int)(seed >> 33) & 3;
        __asm__ volatile("" : "+r"(index));
        if (index == 0) {
            sum += 1;
        } else if (index == 1) {
            sum += 3;
        } else if (index == 2) {
            sum += 5;
        } else {
            sum += 7;
        }
    }
    u64 ticks = armGetSystemTick() - start;
    __asm__ volatile("" : : "r"(sum));
    return cycles(ticks, mhz, BRANCH_STEPS);
}

void carafeBenchBranches(unsigned int mhz, CarafeBenchTrace trace) {
    char text[256];
    snprintf(text, sizeof(text),
        "jump tables at %u MHz, cycles a step: an indirect branch cycling through 2 targets %.1f, 4 %.1f, "
        "8 entries over 4 targets %.1f, at random among 4 %.1f; a compare chain at random among 4 %.1f",
        mhz, periodic(2, mhz), periodic(4, mhz), periodic(8, mhz), periodic(0, mhz), compareChain(mhz));
    trace("[A57] ", text);
}

#define FETCH_BYTES (128 * 1024)
#define FETCH_RUN_INSTRUCTIONS (1u << 21)
#define ENTRY_BLOCKS 64
#define ENTRY_RUNS 4000

void carafeFetchCode(void);
void carafeEntries8(u64* state, u64* stores, u64 value);
void carafeEntries8Header(u64* state, u64* stores, u64 value);
void carafeEntries16(u64* state, u64* stores, u64 value);
void carafeEntries16Header(u64* state, u64* stores, u64 value);

__asm__(
    ".pushsection .text.carafe_bench_code,\"ax\",%progbits\n"
    ".balign 64\n"
    ".global carafeFetchCode\n"
    ".type carafeFetchCode, %function\n"
    "carafeFetchCode:\n"
    ".rept 4096\n"
    "add x9, x9, #1\n"
    "add x10, x10, #1\n"
    "add x11, x11, #1\n"
    "add x12, x12, #1\n"
    "add x13, x13, #1\n"
    "add x14, x14, #1\n"
    "add x15, x15, #1\n"
    "b 1f\n"
    "1:\n"
    ".endr\n"
    "ret\n"
    ".macro carafe_entry header, stores\n"
    ".if \\header\n"
    "adr x16, .\n"
    "str x16, [x0]\n"
    "str xzr, [x0, #8]\n"
    ".endif\n"
    ".set carafe_offset, 0\n"
    ".rept \\stores\n"
    "str x2, [x1, #carafe_offset]\n"
    ".set carafe_offset, carafe_offset + 8\n"
    ".endr\n"
    "adr x17, 2f\n"
    "br x17\n"
    "2:\n"
    ".endm\n"
    ".macro carafe_entries name, header, stores\n"
    ".balign 64\n"
    ".global \\name\n"
    ".type \\name, %function\n"
    "\\name:\n"
    ".rept 64\n"
    "carafe_entry \\header, \\stores\n"
    ".endr\n"
    "ret\n"
    ".endm\n"
    "carafe_entries carafeEntries8, 0, 8\n"
    "carafe_entries carafeEntries8Header, 1, 8\n"
    "carafe_entries carafeEntries16, 0, 16\n"
    "carafe_entries carafeEntries16Header, 1, 16\n"
    ".popsection\n");

static double fetchCycles(size_t bytes, unsigned int mhz) {
    void (*run)(void) = (void (*)(void))((uintptr_t)carafeFetchCode + FETCH_BYTES - bytes);
    u64 instructions = bytes / 4 + 1;
    u64 runs = FETCH_RUN_INSTRUCTIONS / instructions;
    u64 best = UINT64_MAX;
    for (int attempt = 0; attempt < 3; attempt++) {
        u64 start = armGetSystemTick();
        for (u64 i = 0; i < runs; i++) {
            run();
        }
        u64 ticks = armGetSystemTick() - start;
        best = ticks < best ? ticks : best;
    }
    return cycles(best, mhz, runs * instructions);
}

static double entryCycles(void (*run)(u64*, u64*, u64), unsigned int mhz) {
    static u64 state[8] __attribute__((aligned(LINE_BYTES)));
    static u64 stores[16] __attribute__((aligned(LINE_BYTES)));
    u64 best = UINT64_MAX;
    for (int attempt = 0; attempt < 3; attempt++) {
        u64 start = armGetSystemTick();
        for (int i = 0; i < ENTRY_RUNS; i++) {
            run(state, stores, (u64)i);
        }
        u64 ticks = armGetSystemTick() - start;
        best = ticks < best ? ticks : best;
    }
    return cycles(best, mhz, (u64)ENTRY_RUNS * ENTRY_BLOCKS);
}

void carafeBenchCode(unsigned int mhz, CarafeBenchTrace trace) {
    s32 home_core;
    u64 home_mask;
    if (R_FAILED(svcGetThreadCoreMask(&home_core, &home_mask, CUR_THREAD_HANDLE)) || !pinTo(HOME_CORE)) {
        return;
    }
    double fetch_16 = fetchCycles(16 * 1024, mhz);
    double fetch_32 = fetchCycles(32 * 1024, mhz);
    double fetch_48 = fetchCycles(48 * 1024, mhz);
    double fetch_64 = fetchCycles(64 * 1024, mhz);
    double fetch_96 = fetchCycles(96 * 1024, mhz);
    double fetch_128 = fetchCycles(128 * 1024, mhz);
    double entries_8 = entryCycles(carafeEntries8, mhz);
    double entries_8_header = entryCycles(carafeEntries8Header, mhz);
    double entries_16 = entryCycles(carafeEntries16, mhz);
    double entries_16_header = entryCycles(carafeEntries16Header, mhz);
    __real_svcSetThreadCoreMask(CUR_THREAD_HANDLE, home_core, (u32)home_mask);
    char text[512];
    snprintf(text, sizeof(text),
        "code at %u MHz: cycles an instruction, a taken branch every 32 bytes, over 16 KB %.2f, 32 KB %.2f, "
        "48 KB %.2f, 64 KB %.2f, 96 KB %.2f, 128 KB %.2f; cycles a block entered by br, 8 stores %.1f, with the "
        "two header stores %.1f, 16 stores %.1f, with the header stores %.1f",
        mhz, fetch_16, fetch_32, fetch_48, fetch_64, fetch_96, fetch_128, entries_8, entries_8_header, entries_16,
        entries_16_header);
    trace("[A57] ", text);
}
