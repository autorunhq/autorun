#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int fail_alloc;
static void *index_calloc(size_t count, size_t size)
{
    return fail_alloc ? NULL : calloc(count, size);
}
#define calloc index_calloc
#include "../../dlls/ntdll/unix/horizon_swap_index.h"
#undef calloc

int main(void)
{
    struct horizon_swap_index index = {0};
    const uintptr_t limit = UINT64_C(1) << 39, boundary = UINT64_C(1) << 27;
    unsigned int i;
    assert(!swap_index_contains(&index, 0, limit));
    assert(!swap_index_update(&index, boundary - 4096, 3 * 4096, 1));
    assert(swap_index_contains(&index, boundary - 1, 1));
    assert(swap_index_contains(&index, boundary, 1));
    assert(!swap_index_contains(&index, boundary + 8192, 1));
    assert(!swap_index_update(&index, boundary, 4096, 0));
    assert(!swap_index_contains(&index, boundary, 4096));
    assert(swap_index_contains(&index, boundary + 4096, 4096));
    assert(!swap_index_update(&index, boundary + 4097, 4095, 0));
    assert(swap_index_contains(&index, boundary + 4096, 1));
    assert(!swap_index_update(&index, limit - 4096, 4096, 1));
    assert(swap_index_contains(&index, limit - 1, 2));
    assert(!swap_index_contains(&index, limit, 4096));
    assert(swap_index_contains(&index, UINTPTR_MAX - 1, 4));
    assert(swap_index_update(&index, limit - 4096, 8192, 1) == -1);
    fail_alloc = 1;
    assert(swap_index_update(&index, boundary * 3, 4096, 1) == -1);
    assert(!swap_index_contains(&index, boundary * 3, 4096));
    fail_alloc = 0;
    for (i = 0; i < 512; i++)
    {
        uintptr_t addr = boundary * 3 + i * 4096;
        assert(!swap_index_update(&index, addr, 4096, 1));
        assert(swap_index_contains(&index, addr + 4095, 1));
    }
    for (i = 0; i < 512; i += 2)
        assert(!swap_index_update(&index, boundary * 3 + i * 4096, 4096, 0));
    for (i = 0; i < 512; i++)
        assert(swap_index_contains(&index, boundary * 3 + i * 4096, 4096) == (int)(i & 1));
    for (i = 0; i < 4096; i++) free(index.regions[i]);
    puts("swap index: page isolation, boundaries, partial exclusions and allocation failure passed");
}
