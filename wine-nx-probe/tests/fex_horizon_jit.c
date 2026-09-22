#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned int fail_metadata;
static void *test_calloc(size_t count, size_t size)
{
    if (fail_metadata && !--fail_metadata) return NULL;
    return calloc(count, size);
}
#define calloc test_calloc
#include "../source/fex_jit.c"
#undef calloc

enum { OBJECT_COUNT = 128, RESERVATION_COUNT = 256 };
static struct { size_t size; void *rw, *rx; } objects[OBJECT_COUNT];
static struct reservation { uintptr_t address; size_t size; } reservations[RESERVATION_COUNT];
static size_t reservation_size, reservation_limit = FEX_ARENA_MAX_SIZE + FEX_ARENA_LARGE_ALIGNMENT + 65536;
static uintptr_t address_limit = 0x8000000000ULL;
static size_t map_limit = FEX_ARENA_MAX_SIZE;
static unsigned int map_limit_op = CodeMapOperation_MapOwner, resource_failures;
static unsigned int live, created, fail_create, fail_op = ~0u, fail_close, peak;
static unsigned int object_limit = 32;
static unsigned int fail_reservation, fail_unmap;

int envIsSyscallHinted(unsigned int call) { return call == 0x4b || call == 0x4c; }
void horizon_get_address_space_limits(void **start, void **limit)
{
    *start = (void *)0x200000;
    *limit = (void *)address_limit;
}
void *horizon_reserve_native_code(size_t size, void **token)
{
    uintptr_t address = 0x20000000;
    unsigned int i;

    *token = NULL;
    if (fail_reservation && !--fail_reservation) return NULL;
    if (size > reservation_limit) return NULL;
    for (;;)
    {
        if (address + size > address_limit) return NULL;
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
    if (fail_create || live == object_limit) { fail_create = 0; *handle = 0xdead; return 0xce01; }
    for (unsigned int i = 1; i < OBJECT_COUNT; ++i) if (!objects[i].size)
    {
        objects[i].size = size;
        *handle = i;
        ++live;
        if (live > peak) peak = live;
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
        if (reservations[i].size && (uintptr_t)address >= reservations[i].address + 4096 &&
            (uintptr_t)address + size + 4096 <= reservations[i].address + reservations[i].size) break;
    assert(i != RESERVATION_COUNT);
    if (op == fail_op) { fail_op = ~0u; return 0xdc01; }
    if (op == map_limit_op && size > map_limit) { ++resource_failures; return 0xce01; }
    if (op == CodeMapOperation_UnmapOwner && fail_unmap) { fail_unmap = 0; return 0xdc01; }
    switch (op)
    {
    case CodeMapOperation_MapOwner:
    {
        size_t alignment = address_limit <= 0x100000000ULL ? 65536 : FEX_ARENA_LARGE_ALIGNMENT;
        if (size & (alignment - 1)) alignment = 4096;
        assert(!objects[handle].rw && perm == Perm_Rw);
        assert(!((uintptr_t)address & (alignment - 1)));
        objects[handle].rw = address;
        break;
    }
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
    void *rx[96], *rw, *extra, *packed;
    size_t capacity;
    assert(!wine_nx_fex_jit_create(16384, &rx[0], &rw));
    assert(!wine_nx_fex_jit_create(16 * mb, &rx[1], &rw));
    assert(!wine_nx_fex_jit_create(32 * mb, &rx[2], &rw));
    for (unsigned int i = 3; i < 16; ++i) assert(!wine_nx_fex_jit_create(64 * mb, &rx[i], &rw));
    assert(live == 10 && peak == 10);
    for (struct fex_jit_arena *arena = arenas; arena; arena = arena->next)
        assert(arena->size <= FEX_ARENA_MAX_SIZE);
    object_limit = live;
    assert(!wine_nx_fex_jit_create(64 * mb, &packed, &rw) && live == object_limit);
    assert(wine_nx_fex_jit_create(64 * mb, &extra, &rw) == ENOMEM && !extra && !rw);
    assert(!wine_nx_fex_jit_close(packed));
    object_limit = 32;
    for (unsigned int i = 0; i < 16; ++i)
    {
        size_t size = wine_nx_fex_jit_size(rx[i]);
        assert(size && !wine_nx_fex_jit_flush(rx[i], size));
        assert(wine_nx_fex_jit_flush(rx[i], size + 1) == EINVAL);
        for (unsigned int j = 0; j < i; ++j)
            assert((uintptr_t)rx[i] + size <= (uintptr_t)rx[j] ||
                   (uintptr_t)rx[j] + wine_nx_fex_jit_size(rx[j]) <= (uintptr_t)rx[i]);
    }
    assert(!wine_nx_fex_jit_release() && !live && !reservation_size && !arenas);
    for (unsigned int i = 0; i < 96; ++i) assert(!wine_nx_fex_jit_create(4096, &rx[i], &rw));
    assert(live == 1);
    for (unsigned int i = 0; i < 96; i += 2) assert(!wine_nx_fex_jit_close(rx[i]));
    for (unsigned int i = 0; i < 96; i += 2)
    {
        assert(!wine_nx_fex_jit_create(4096, &extra, &rw) && extra == rx[i]);
    }
    assert(live == 1);
    assert(!wine_nx_fex_jit_close(rx[10]) && !wine_nx_fex_jit_close(rx[11]));
    assert(!wine_nx_fex_jit_create(8192, &extra, &rw) && extra == rx[10]);
    assert(wine_nx_fex_jit_size(extra) == 8192 && !wine_nx_fex_jit_size(rx[11]));
    assert(!wine_nx_fex_jit_release() && !live && !reservation_size);
    next_arena_size = FEX_ARENA_MAX_SIZE;
    assert(!wine_nx_fex_jit_create(mb, &extra, &rw) && arenas->size == FEX_ARENA_MAX_SIZE);
    assert(!wine_nx_fex_jit_close(extra) && !arenas && next_arena_size == FEX_ARENA_FIRST_SIZE);
    for (unsigned int allocation = 1; allocation <= 2; ++allocation)
    {
        fail_metadata = allocation;
        assert(wine_nx_fex_jit_create(mb, &extra, &rw) == ENOMEM && !extra && !rw && !mappings && !arenas);
    }
    reservation_limit = 0;
    assert(wine_nx_fex_jit_create(mb, &extra, &rw) == ENOMEM && !extra && !rw && !reservation_size);
    reservation_limit = FEX_ARENA_MAX_SIZE + FEX_ARENA_LARGE_ALIGNMENT + 65536;
    fail_reservation = 2;
    assert(!wine_nx_fex_jit_create(mb, &extra, &rw));
    assert(!wine_nx_fex_jit_release() && !live && !reservation_size);
    for (unsigned int op = CodeMapOperation_MapOwner; op <= CodeMapOperation_MapSlave; ++op)
    {
        fail_op = op;
        assert(wine_nx_fex_jit_create(mb, &extra, &rw) == ENOMEM && !extra && !rw);
        assert(!live && !reservation_size);
    }
    fail_create = 1;
    assert(wine_nx_fex_jit_create(mb, &extra, &rw) == ENOMEM && !live && !reservation_size && !arenas);
    fail_op = CodeMapOperation_MapSlave;
    fail_unmap = 1;
    assert(wine_nx_fex_jit_create(mb, &extra, &rw) == ENOMEM && !extra && !rw && live == 1);
    assert(reservation_size == 2 * (FEX_ARENA_FIRST_SIZE + FEX_ARENA_LARGE_ALIGNMENT + 65536));
    assert(!mappings && arenas && !arenas->ready && !arenas->users);
    assert(!wine_nx_fex_jit_create(mb, &extra, &rw) && live == 2);
    assert(!wine_nx_fex_jit_release() && !live && !reservation_size && !arenas);
    for (unsigned int op = CodeMapOperation_UnmapOwner; op <= CodeMapOperation_UnmapSlave; ++op)
    {
        assert(!wine_nx_fex_jit_create(mb, &extra, &rw));
        fail_op = op;
        assert(wine_nx_fex_jit_close(extra) == EIO && live == 1 && wine_nx_fex_jit_size(extra) == mb);
        assert(!arenas->ready && wine_nx_fex_jit_flush(extra, 4) == EINVAL);
        assert(!wine_nx_fex_jit_close(extra) && !live);
        assert(!wine_nx_fex_jit_release());
    }
    assert(!wine_nx_fex_jit_create(mb, &extra, &rw));
    capacity = arenas->size;
    fail_close = 1;
    assert(wine_nx_fex_jit_release() == capacity && live == 1 && reservation_size);
    assert(!wine_nx_fex_jit_release() && !live && !reservation_size);
    for (unsigned int i = 0; i < 3; ++i) assert(!wine_nx_fex_jit_create(mb, &rx[i], &rw));
    capacity = arenas->size;
    fail_close = 1;
    assert(wine_nx_fex_jit_release() == capacity && live == 1 && mappings && !mappings->next);
    assert(wine_nx_fex_jit_size(rx[0]) == mb && !wine_nx_fex_jit_size(rx[2]));
    assert(!wine_nx_fex_jit_create(mb, &extra, &rw) && live == 2);
    assert(!wine_nx_fex_jit_release() && !live && !reservation_size && !mappings);
    reservation_limit = 32 * mb + FEX_ARENA_LARGE_ALIGNMENT + 65536;
    assert(!wine_nx_fex_jit_create(16384, &rx[0], &rw));
    assert(!wine_nx_fex_jit_create(16 * mb, &rx[1], &rw));
    assert(!wine_nx_fex_jit_create(32 * mb, &rx[2], &rw));
    assert(wine_nx_fex_jit_create(64 * mb, &extra, &rw) == ENOMEM);
    assert(!wine_nx_fex_jit_create(32 * mb, &rx[3], &rw));
    assert(!wine_nx_fex_jit_create(16 * mb, &rx[4], &rw));
    assert(live <= 5 && reservation_size > 2 * 64 * mb);
    assert(!wine_nx_fex_jit_release() && !reservation_size);
    {
        const size_t sizes[] = {16384, 16 * mb, 32 * mb, 32 * mb, 32 * mb,
                               16 * mb, 16 * mb, 16 * mb, 16 * mb};
        unsigned int arena_count;
        for (unsigned int i = 0; i < 9; ++i) assert(!wine_nx_fex_jit_create(sizes[i], &rx[i], &rw));
        assert(live < 10 && wine_nx_fex_jit_size(rx[0]) == 16384);
        arena_count = live;
        assert(!wine_nx_fex_jit_close(rx[8]));
        assert(!wine_nx_fex_jit_create(16 * mb, &extra, &rw) && live == arena_count);
        assert(!wine_nx_fex_jit_release() && !live && !reservation_size && !mappings);
    }
    reservation_limit = 0;
    assert(wine_nx_fex_jit_create(mb, &extra, &rw) == ENOMEM && !extra && !rw && !live);
    reservation_limit = FEX_ARENA_MAX_SIZE + FEX_ARENA_LARGE_ALIGNMENT + 65536;
    for (unsigned int op = CodeMapOperation_MapOwner; op <= CodeMapOperation_MapSlave; ++op)
    {
        map_limit_op = op;
        map_limit = 16 * mb;
        next_arena_size = FEX_ARENA_MAX_SIZE;
        resource_failures = 0;
        assert(wine_nx_fex_jit_create(64 * mb, &extra, &rw) == ENOMEM);
        assert(wine_nx_fex_jit_create(32 * mb, &extra, &rw) == ENOMEM);
        assert(!wine_nx_fex_jit_create(16 * mb, &extra, &rw));
        assert(arenas->size == 16 * mb && live == 1 && resource_failures == 3);
        assert(!wine_nx_fex_jit_release() && !live && !reservation_size);
    }
    map_limit = FEX_ARENA_MAX_SIZE;
    address_limit = 0x100000000ULL;
    next_arena_size = FEX_ARENA_MAX_SIZE;
    assert(!wine_nx_fex_jit_create(16 * mb, &extra, &rw));
    assert(arenas->size == FEX_JIT_MAX_SIZE &&
           arenas->reservation_size == FEX_JIT_MAX_SIZE + 2 * 65536);
    assert(!wine_nx_fex_jit_release() && !live && !reservation_size);
    puts("FEX Horizon JIT: pooled buffers, object limits, hole reuse, aligned aliases, mapping-resource retries and failure cleanup passed");
}
