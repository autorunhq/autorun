#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned int fail_metadata;
static void *test_calloc(size_t count, size_t size)
{
    if (fail_metadata) { fail_metadata = 0; return NULL; }
    return calloc(count, size);
}
#define calloc test_calloc
#include "../source/fex_jit.c"
#undef calloc

enum { OBJECT_COUNT = 128, RESERVATION_COUNT = 256 };
static struct { size_t size; void *rw, *rx; } objects[OBJECT_COUNT];
static struct reservation { uintptr_t address; size_t size; } reservations[RESERVATION_COUNT];
static size_t reservation_size, reservation_limit = 64u * 1024 * 1024 + 8192;
static unsigned int live, created, fail_create, fail_op = ~0u, fail_close;
static unsigned int fail_reservation, fail_unmap;
static char last_trace[256];

int envIsSyscallHinted(unsigned int call) { return call == 0x4b || call == 0x4c; }
void wine_nx_runtime_trace(const char *message) { snprintf(last_trace, sizeof(last_trace), "%s", message); }
void *horizon_reserve_native_code(size_t size, void **token)
{
    uintptr_t address = 0x20000000;
    unsigned int i;

    *token = NULL;
    if (fail_reservation && !--fail_reservation) return NULL;
    if (size > reservation_limit) return NULL;
    for (;;)
    {
        if (address + size > 0x38000000) return NULL;
        for (i = 0; i < RESERVATION_COUNT; ++i)
            if (reservations[i].size && address < reservations[i].address + reservations[i].size &&
                address + size > reservations[i].address) break;
        if (i == RESERVATION_COUNT) break;
        address = reservations[i].address + reservations[i].size;
    }
    for (i = 0; i < RESERVATION_COUNT; ++i) if (!reservations[i].size)
    {
        reservations[i].address = address;
        reservations[i].size = size;
        reservation_size += size;
        *token = &reservations[i];
        return (void *)address;
    }
    assert(0);
    return NULL;
}
void horizon_release_native_code(void *token)
{
    struct reservation *reservation = token;
    assert(reservation->size);
    for (unsigned int i = 1; i < OBJECT_COUNT; ++i)
    {
        uintptr_t rw = (uintptr_t)objects[i].rw, rx = (uintptr_t)objects[i].rx;
        assert(!rw || rw < reservation->address || rw >= reservation->address + reservation->size);
        assert(!rx || rx < reservation->address || rx >= reservation->address + reservation->size);
    }
    reservation_size -= reservation->size;
    reservation->size = 0;
}
Result svcCreateCodeMemory(Handle *handle, void *backing, size_t size)
{
    assert(backing && !((uintptr_t)backing & 4095) && size && !(size & 4095));
    if (fail_create) { fail_create = 0; return 0xce01; }
    for (unsigned int i = 1; i < OBJECT_COUNT; ++i) if (!objects[i].size)
    {
        objects[i].size = size;
        *handle = i;
        ++live;
        ++created;
        return 0;
    }
    assert(0);
    return 1;
}
Result svcControlCodeMemory(Handle handle, unsigned int op, void *address, size_t size, unsigned int perm)
{
    assert(handle && handle < OBJECT_COUNT && objects[handle].size == size);
    unsigned int i;
    for (i = 0; i < RESERVATION_COUNT; ++i)
        if (reservations[i].size == size + 8192 && reservations[i].address + 4096 == (uintptr_t)address) break;
    assert(i != RESERVATION_COUNT);
    if (op == fail_op) { fail_op = ~0u; return 0xdc01; }
    if (op == CodeMapOperation_UnmapOwner && fail_unmap) { fail_unmap = 0; return 0xdc01; }
    switch (op)
    {
    case CodeMapOperation_MapOwner:
        assert(!objects[handle].rw && perm == Perm_Rw);
        objects[handle].rw = address;
        break;
    case CodeMapOperation_MapSlave:
        assert(!objects[handle].rx && objects[handle].rw && perm == Perm_Rx);
        assert(address != objects[handle].rw);
        objects[handle].rx = address;
        break;
    case CodeMapOperation_UnmapOwner:
        assert(objects[handle].rw == address && !perm);
        objects[handle].rw = NULL;
        break;
    case CodeMapOperation_UnmapSlave:
        assert(objects[handle].rx == address && !perm);
        objects[handle].rx = NULL;
        break;
    default: assert(0);
    }
    return 0;
}
Result svcCloseHandle(Handle handle)
{
    assert(handle && handle < OBJECT_COUNT && objects[handle].size && !objects[handle].rw && !objects[handle].rx);
    if (fail_close) { fail_close = 0; return 0xfa01; }
    objects[handle].size = 0;
    --live;
    return 0;
}
void armDCacheFlush(void *address, size_t size) { assert(address && size); }
void armICacheInvalidate(void *address, size_t size) { assert(address && size); }

int main(void)
{
    const size_t mb = 1024 * 1024;
    void *rx[96], *rw, *extra;
    assert(!wine_nx_fex_jit_create(16384, &rx[0], &rw));
    assert(!wine_nx_fex_jit_create(16 * mb, &rx[1], &rw));
    assert(!wine_nx_fex_jit_create(32 * mb, &rx[2], &rw));
    assert(!wine_nx_fex_jit_create(64 * mb, &rx[3], &rw));
    assert(!wine_nx_fex_jit_create(64 * mb, &rx[4], &rw));
    assert(live == 5 && reservation_size == 2 * (176 * mb + 16384 + 5 * 8192));
    assert(wine_nx_fex_jit_create(32 * mb, &extra, &rw) == ENOMEM && !extra && !rw && created == 5);
    assert(!wine_nx_fex_jit_close(rx[2]));
    assert(!wine_nx_fex_jit_create(32 * mb, &extra, &rw) && extra == rx[2]);
    assert(!wine_nx_fex_jit_release() && !live && !reservation_size);
    for (unsigned int i = 0; i < 96; ++i) assert(!wine_nx_fex_jit_create(4096, &rx[i], &rw));
    assert(live == 96 && strstr(last_trace, "buffers=96 "));
    for (unsigned int i = 0; i < 96; i += 2) assert(!wine_nx_fex_jit_close(rx[i]));
    for (unsigned int i = 0; i < 96; i += 2) assert(!wine_nx_fex_jit_create(4096, &rx[i], &rw));
    assert(live == 96);
    assert(!wine_nx_fex_jit_release() && !live && !reservation_size);
    fail_metadata = 1;
    assert(wine_nx_fex_jit_create(mb, &extra, &rw) == ENOMEM && !extra && !rw && !mappings);
    fail_reservation = 2;
    assert(wine_nx_fex_jit_create(mb, &extra, &rw) == ENOMEM && !extra && !rw && !reservation_size);
    for (unsigned int op = CodeMapOperation_MapOwner; op <= CodeMapOperation_MapSlave; ++op)
    {
        fail_op = op;
        assert(wine_nx_fex_jit_create(mb, &extra, &rw) == ENOMEM && !extra && !rw);
        assert(!live && !reservation_size && strstr(last_trace, "rc=0xdc01"));
    }
    fail_create = 1;
    assert(wine_nx_fex_jit_create(mb, &extra, &rw) == ENOMEM && !live && !reservation_size);
    fail_op = CodeMapOperation_MapSlave;
    fail_unmap = 1;
    assert(wine_nx_fex_jit_create(mb, &extra, &rw) == ENOMEM && !extra && !rw && live == 1);
    assert(reservation_size == 2 * (mb + 8192));
    assert(!wine_nx_fex_jit_create(mb, &extra, &rw) && live == 2);
    assert(!wine_nx_fex_jit_release() && !live && !reservation_size);
    for (unsigned int op = CodeMapOperation_UnmapOwner; op <= CodeMapOperation_UnmapSlave; ++op)
    {
        assert(!wine_nx_fex_jit_create(mb, &extra, &rw));
        fail_op = op;
        assert(wine_nx_fex_jit_close(extra) == EIO && live == 1 && wine_nx_fex_jit_size(extra) == mb);
        assert(!wine_nx_fex_jit_close(extra) && !live);
    }
    assert(!wine_nx_fex_jit_create(mb, &extra, &rw));
    fail_close = 1;
    assert(wine_nx_fex_jit_release() == mb && live == 1 && reservation_size);
    assert(!wine_nx_fex_jit_release() && !live && !reservation_size);
    for (unsigned int i = 0; i < 3; ++i) assert(!wine_nx_fex_jit_create(mb, &rx[i], &rw));
    fail_close = 1;
    assert(wine_nx_fex_jit_release() == mb && live == 1 && mappings && !mappings->next);
    assert(wine_nx_fex_jit_size(rx[2]) == mb && !wine_nx_fex_jit_size(rx[0]));
    assert(!wine_nx_fex_jit_create(mb, &extra, &rw) && live == 2);
    assert(!wine_nx_fex_jit_release() && !live && !reservation_size && !mappings);
    reservation_limit = 32 * mb + 8192;
    assert(!wine_nx_fex_jit_create(16384, &rx[0], &rw));
    assert(!wine_nx_fex_jit_create(16 * mb, &rx[1], &rw));
    assert(!wine_nx_fex_jit_create(32 * mb, &rx[2], &rw));
    assert(wine_nx_fex_jit_create(64 * mb, &extra, &rw) == ENOMEM);
    assert(!wine_nx_fex_jit_create(32 * mb, &rx[3], &rw));
    assert(!wine_nx_fex_jit_create(16 * mb, &rx[4], &rw));
    assert(live == 5 && reservation_size > 2 * 64 * mb);
    assert(!wine_nx_fex_jit_release() && !reservation_size);
    {
        const size_t sizes[] = {16384, 16 * mb, 32 * mb, 32 * mb, 32 * mb,
                               16 * mb, 16 * mb, 16 * mb, 16 * mb};
        for (unsigned int i = 0; i < 9; ++i) assert(!wine_nx_fex_jit_create(sizes[i], &rx[i], &rw));
        assert(live == 9 && wine_nx_fex_jit_size(rx[0]) == 16384);
        assert(wine_nx_fex_jit_create(16 * mb, &extra, &rw) == ENOMEM && live == 9);
        assert(!wine_nx_fex_jit_close(rx[2]));
        assert(!wine_nx_fex_jit_create(16 * mb, &extra, &rw) && live == 9);
        assert(!wine_nx_fex_jit_release() && !live && !reservation_size && !mappings);
    }
    reservation_limit = 0;
    assert(wine_nx_fex_jit_create(mb, &extra, &rw) == ENOMEM && !extra && !rw && !live);
    puts("FEX Horizon JIT: 96 live buffers, retained-buffer growth, address-space exhaustion, reuse, guards and failure cleanup passed");
}
