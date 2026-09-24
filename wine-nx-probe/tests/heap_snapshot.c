#include <assert.h>
#include <stdio.h>
#include "../source/heap_snapshot.h"

struct horizon_heap_chunk *__malloc_av_[258];
static _Alignas(16) unsigned char heap[4096];

int main(void)
{
    struct horizon_heap_chunk *first = (void *)heap, *second = (void *)(heap + 1024);
    size_t largest;
    unsigned int bin;

    for (bin = 0; bin < 128; bin++)
        __malloc_av_[2 * bin + 2] = __malloc_av_[2 * bin + 3] =
            (void *)((char *)&__malloc_av_[2 * bin + 2] - 2 * sizeof(size_t));
    largest = 64;
    assert(horizon_heap_largest((uintptr_t)heap, (uintptr_t)heap + sizeof(heap), 1, &largest));
    assert(largest == 64);

    first->size = 513;
    first->next = __malloc_av_[4];
    __malloc_av_[4] = first;
    second->size = 2049;
    second->next = __malloc_av_[256];
    __malloc_av_[256] = second;
    assert(horizon_heap_largest((uintptr_t)heap, (uintptr_t)heap + sizeof(heap), 3, &largest));
    assert(largest == 2048);
    assert(!horizon_heap_largest((uintptr_t)heap, (uintptr_t)heap + sizeof(heap), 1, &largest));

    second->size = 8192;
    assert(!horizon_heap_largest((uintptr_t)heap, (uintptr_t)heap + sizeof(heap), 3, &largest));
    second->size = 2049;
    second->next = second;
    assert(!horizon_heap_largest((uintptr_t)heap, (uintptr_t)heap + sizeof(heap), 3, &largest));
    second->next = (void *)1;
    assert(!horizon_heap_largest((uintptr_t)heap, (uintptr_t)heap + sizeof(heap), 3, &largest));
    puts("heap snapshot: free bins, split remainder, size flags, bounds and cycles passed");
}
