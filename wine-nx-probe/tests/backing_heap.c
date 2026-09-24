#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../source/backing_heap.h"

#define COUNT 2048
static struct backing_block blocks[COUNT];
static struct backing_heap heap;
static uint32_t random_state = 0x61428103;

static uint32_t next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    return random_state ^= random_state << 5;
}

static void validate(void)
{
    unsigned char free_blocks[COUNT] = {0};
    uint32_t at, length, holes = 0;
    size_t used = 0, largest = 0;
    int previous_free = 0;
    for (at = heap.bottom; at < heap.count; at += length)
    {
        uint32_t tag = blocks[at].length;
        length = tag & ~BACKING_USED;
        assert(length && length <= heap.count - at);
        assert(blocks[at + length - 1].length == tag);
        if (tag & BACKING_USED) { used += length * BACKING_UNIT; previous_free = 0; }
        else
        {
            assert(!previous_free && at != heap.bottom);
            previous_free = free_blocks[at] = 1;
            holes++;
            if (length * BACKING_UNIT > largest) largest = length * BACKING_UNIT;
        }
    }
    assert(used == heap.used && holes == heap.holes && largest == backing_largest(&heap));
    for (unsigned int bin = 0; bin < BACKING_BINS; bin++)
    {
        uint32_t previous = BACKING_NONE;
        for (at = heap.bins[bin]; at != BACKING_NONE; at = blocks[at].next)
        {
            assert(at < heap.count && free_blocks[at] && blocks[at].previous == previous);
            assert(backing_bin(blocks[at].length) == bin);
            free_blocks[at] = 0;
            previous = at;
            holes--;
        }
    }
    assert(!holes);
}

int main(void)
{
    const uintptr_t base = UINT64_C(0x1800040000);
    void *live[256] = {0}, *first, *second;
    size_t sizes[256] = {0};
    uintptr_t floor = base;
    assert(!backing_largest(&heap));
    backing_init(&heap, blocks, UINT64_C(0x200000000), 32);
    first = backing_alloc(&heap, heap.base, 2 * 1048576, 2 * 1048576);
    assert(first == (void *)heap.base && !heap.bottom && heap.used == 2 * 1048576);
    assert(!backing_alloc(&heap, heap.base, BACKING_UNIT, 1));
    backing_free(&heap, first);
    assert(heap.bottom == heap.count && !heap.used);

    backing_init(&heap, blocks, base, COUNT);
    first = backing_alloc(&heap, base, BACKING_UNIT, 4 * BACKING_UNIT);
    second = backing_alloc(&heap, base, BACKING_UNIT, BACKING_UNIT);
    backing_free(&heap, first);
    assert(backing_alloc(&heap, base, BACKING_UNIT, 4 * BACKING_UNIT) == first);
    backing_free(&heap, first);
    backing_free(&heap, second);
    assert(heap.bottom == COUNT && !heap.holes && !heap.used);
    assert(!backing_alloc(&heap, base, BACKING_UNIT, SIZE_MAX));
    assert(!backing_alloc(&heap, base, 3, BACKING_UNIT));
    assert(!backing_alloc(&heap, base, 0, BACKING_UNIT));
    assert(!backing_alloc(&heap, base, BACKING_UNIT, 0));
    assert(!backing_alloc(&heap, base, BACKING_UNIT, (COUNT + 1) * BACKING_UNIT));

    first = backing_alloc(&heap, base, BACKING_UNIT, 8 * BACKING_UNIT);
    assert(backing_resize(&heap, first, 2 * BACKING_UNIT));
    assert(backing_size(&heap, first) == 2 * BACKING_UNIT && heap.used == 2 * BACKING_UNIT);
    assert(backing_resize(&heap, first, 8 * BACKING_UNIT));
    assert(!backing_resize(&heap, first, 9 * BACKING_UNIT));
    assert(!backing_resize(&heap, first, SIZE_MAX));
    backing_free(&heap, first);

    for (unsigned int iteration = 0; iteration < 300000; iteration++)
    {
        unsigned int slot = next_random() % 256;
        if (live[slot] && !(next_random() % 3))
        {
            size_t previous = backing_size(&heap, live[slot]);
            size_t size = 1 + next_random() % (48 * BACKING_UNIT);
            if (backing_resize(&heap, live[slot], size))
                assert(backing_size(&heap, live[slot]) == (size + BACKING_UNIT - 1) / BACKING_UNIT * BACKING_UNIT);
            else assert(backing_size(&heap, live[slot]) == previous);
        }
        else if (live[slot]) { backing_free(&heap, live[slot]); live[slot] = NULL; }
        else
        {
            size_t alignment = (size_t)4096 << (next_random() % 11);
            sizes[slot] = 1 + next_random() % (48 * BACKING_UNIT);
            live[slot] = backing_alloc(&heap, floor, alignment, sizes[slot]);
            if (live[slot])
            {
                uintptr_t start = (uintptr_t)live[slot];
                size_t length = backing_size(&heap, live[slot]);
                assert(!(start % alignment) && start >= floor);
                assert(length >= sizes[slot] && length - sizes[slot] < BACKING_UNIT);
                for (unsigned int i = 0; i < 256; i++)
                    if (i != slot && live[i])
                        assert(start + length <= (uintptr_t)live[i] ||
                               (uintptr_t)live[i] + backing_size(&heap, live[i]) <= start);
            }
        }
        if (!(iteration % 100))
        {
            validate();
            floor = base + next_random() % (heap.bottom * BACKING_UNIT + 1);
        }
    }
    for (unsigned int i = 0; i < 256; i++) if (live[i]) backing_free(&heap, live[i]);
    validate();
    assert(heap.bottom == COUNT && !heap.used && !heap.holes);
    printf("backing heap: 300000 operations, all space returned\n");
    return 0;
}
