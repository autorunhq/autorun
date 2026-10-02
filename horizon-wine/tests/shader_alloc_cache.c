static void cache_basic(void)
{
    assert(!small_used);
    wine_nx_shader_alloc_begin();
    assert(wine_nx_shader_alloc_active);
    void *a = __wrap__malloc_r(_REENT, 128);
    assert(a);
    memset(a, 0xa5, 128);
    __wrap__free_r(_REENT, a);
    size_t held = small_used;
    assert(held && cache.bytes);
    void *b = __wrap__calloc_r(_REENT, 4, 32);
    assert(a == b && small_used == held);
    for (unsigned int i = 0; i < 128; i++) assert(!((unsigned char *)b)[i]);
    __wrap__free_r(_REENT, b);
    wine_nx_shader_alloc_begin();
    wine_nx_shader_alloc_end();
    assert(wine_nx_shader_alloc_active && cache.bytes);
    b = __wrap__malloc_r(_REENT, 128);
    assert(a == b);
    memset(b, 0x19, 128);
    wine_nx_shader_alloc_end();
    assert(!wine_nx_shader_alloc_active && !cache.bytes && small_used);
    for (unsigned int i = 0; i < 128; i++) assert(((unsigned char *)b)[i] == 0x19);
    __wrap__free_r(_REENT, b);
    assert(!small_used);

    wine_nx_shader_alloc_begin();
    for (size_t align = 16; align <= 256; align *= 2)
    {
        a = __wrap__memalign_r(_REENT, align, 2048);
        assert(a && !((uintptr_t)a & (align - 1)));
        __wrap__free_r(_REENT, a);
        b = __wrap__memalign_r(_REENT, align, 2048);
        assert(b == a && !((uintptr_t)b & (align - 1)));
        __wrap__free_r(_REENT, b);
    }
    a = __wrap__memalign_r(_REENT, 65536, 1048576);
    assert(a && owns_pointer(a));
    held = cache.bytes;
    __wrap__free_r(_REENT, a);
    assert(cache.bytes == held && !heap.used);
    wine_nx_shader_alloc_end();
    assert(!small_used);
}

static void cache_resize_and_pressure(void)
{
    wine_nx_shader_alloc_begin();
    unsigned char *a = __wrap__malloc_r(_REENT, 128);
    unsigned char *b = __wrap__malloc_r(_REENT, 4096);
    assert(a && b);
    memset(a, 0x67, 128);
    __wrap__free_r(_REENT, b);
    unsigned char *c = __wrap__realloc_r(_REENT, a, 4096);
    assert(c == b);
    for (unsigned int i = 0; i < 128; i++) assert(c[i] == 0x67);
    assert(__wrap__realloc_r(_REENT, c, 4000) == c);
    fail_allocations = 2;
    assert(!__wrap__realloc_r(_REENT, c, 1048576));
    assert(!fail_allocations && !cache.bytes);
    for (unsigned int i = 0; i < 128; i++) assert(c[i] == 0x67);
    __wrap__free_r(_REENT, c);
    fail_allocations = 1;
    a = __wrap__calloc_r(_REENT, 1, 20000);
    assert(a && !fail_allocations && !cache.bytes);
    for (unsigned int i = 0; i < 20000; i++) assert(!a[i]);
    __wrap__free_r(_REENT, a);
    fail_allocations = 1;
    a = __wrap__memalign_r(_REENT, 128, 65536);
    assert(a && !fail_allocations && !cache.bytes && !((uintptr_t)a & 127));
    __wrap__free_r(_REENT, a);
    fail_allocations = 1;
    a = __wrap__malloc_r(_REENT, 1048576);
    assert(a && !fail_allocations && !cache.bytes);
    __wrap__free_r(_REENT, a);
    a = __wrap__realloc_r(_REENT, NULL, 128);
    assert(a);
    a = __wrap__realloc_r(_REENT, a, 0);
    __wrap__free_r(_REENT, a);
    errno = EIO;
    wine_nx_shader_alloc_end();
    assert(errno == EIO && !small_used);
}

static void cache_check_bounds(void)
{
    size_t bytes = 0, large_bytes = 0;
    unsigned int counts[BIN_COUNT] = {0};
    uint64_t nonempty = 0;
    for (unsigned int bin = 0; bin < CLASS_COUNT; bin++)
    {
        if (cache.bins[bin]) nonempty |= UINT64_C(1) << bin;
        for (struct cached_block *block = cache.bins[bin]; block; block = block->next)
        {
            unsigned int group = size_bin(block->usable);
            assert(size_class(block->usable) == bin);
            bytes += block->usable + sizeof(size_t);
            if (group >= SMALL_BIN_COUNT) large_bytes += block->usable + sizeof(size_t);
            assert(++counts[group] <= bin_limit(group));
        }
    }
    for (unsigned int bin = 0; bin < BIN_COUNT; bin++) assert(counts[bin] == cache.counts[bin]);
    assert(nonempty == cache.nonempty);
    assert(bytes == cache.bytes && bytes <= CACHE_LIMIT);
    assert(large_bytes == cache.large_bytes && large_bytes <= LARGE_LIMIT);
}

static void cache_lookup(void)
{
    assert(!wine_nx_shader_alloc_take(0, 16));
    assert(!wine_nx_shader_alloc_take(65537, 16));
    assert(!wine_nx_shader_alloc_take(128, 512));
    assert(!wine_nx_shader_alloc_take(128, 3));
    assert(!wine_nx_shader_alloc_take(128, 0));
    assert(!wine_nx_shader_alloc_put(NULL, 0));
    assert(!wine_nx_shader_alloc_put(NULL, 65537));
    for (size_t size = 1; size <= BLOCK_LIMIT; size++)
    {
        assert(size_class(size) < CLASS_COUNT);
        assert(size_class(size) >= size_class(size - 1));
        assert(reuse_limit(size) >= size && reuse_limit(size) <= BLOCK_LIMIT);
    }
    wine_nx_shader_alloc_begin();
    void *small[8], *large = __wrap__malloc_r(_REENT, 240);
    assert(large);
    for (unsigned int i = 0; i < 8; i++)
    {
        small[i] = __wrap__malloc_r(_REENT, 128);
        assert(small[i]);
    }
    __wrap__free_r(_REENT, large);
    for (unsigned int i = 0; i < 8; i++) __wrap__free_r(_REENT, small[i]);
    assert(wine_nx_shader_alloc_take(240, 16) == large);
    __wrap__free_r(_REENT, large);
    assert(wine_nx_shader_alloc_take(224, 16) == large);
    __wrap__free_r(_REENT, large);
    assert(!wine_nx_shader_alloc_take(1, 16));
    cache_check_bounds();
    wine_nx_shader_alloc_end();
    cache_check_bounds();
    assert(!small_used);

    wine_nx_shader_alloc_begin();
    for (unsigned int i = 0; i < 8; i++)
    {
        size_t size = i < 4 ? 512 + i * 128 : 1024 + (i - 4) * 256;
        small[i] = __wrap__memalign_r(_REENT, 256, size);
        assert(small[i]);
    }
    for (unsigned int i = 0; i < 8; i++) __wrap__free_r(_REENT, small[i]);
    for (unsigned int i = 0; i < 8; i++)
    {
        size_t size = i < 4 ? 512 + i * 128 : 1024 + (i - 4) * 256;
        assert(wine_nx_shader_alloc_take(size, 256) == small[i]);
        cache_check_bounds();
    }
    assert(!cache.nonempty);
    for (unsigned int i = 0; i < 8; i++) __wrap__free_r(_REENT, small[i]);
    wine_nx_shader_alloc_end();
    assert(!small_used);
}

static void cache_limits(void)
{
    void *items[256];
    wine_nx_shader_alloc_begin();
    for (unsigned int i = 0; i < 256; i++)
    {
        items[i] = __wrap__malloc_r(_REENT, 32768);
        assert(items[i]);
    }
    for (unsigned int i = 0; i < 256; i++)
    {
        __wrap__free_r(_REENT, items[i]);
        cache_check_bounds();
    }
    assert(cache.bytes > LARGE_LIMIT / 2 && cache.bytes <= LARGE_LIMIT);
    for (unsigned int i = 0; i < 256; i++)
    {
        items[i] = __wrap__malloc_r(_REENT, 128);
        assert(items[i]);
    }
    for (unsigned int i = 0; i < 256; i++) __wrap__free_r(_REENT, items[i]);
    assert(cache.counts[size_bin(128)] == 256);
    for (unsigned int i = 0; i < 256; i++)
    {
        items[i] = __wrap__malloc_r(_REENT, 128);
        assert(items[i]);
    }
    assert(!cache.counts[size_bin(128)]);
    for (unsigned int i = 0; i < 256; i++) __wrap__free_r(_REENT, items[i]);
    cache_check_bounds();
    wine_nx_shader_alloc_end();
    assert(!cache.bytes && !cache.large_bytes && !small_used);
    cache_check_bounds();
}

static void cache_total_limit(void)
{
    void *items[2 * SMALL_BIN_LIMIT];
    wine_nx_shader_alloc_begin();
    for (unsigned int i = 0; i < 2 * SMALL_BIN_LIMIT; i++)
    {
        items[i] = __wrap__malloc_r(_REENT, i < SMALL_BIN_LIMIT ? 768 : 256);
        assert(items[i]);
    }
    for (unsigned int i = 0; i < 2 * SMALL_BIN_LIMIT; i++)
    {
        __wrap__free_r(_REENT, items[i]);
        cache_check_bounds();
    }
    assert(cache.bytes > CACHE_LIMIT - 1024 && !cache.large_bytes);
    wine_nx_shader_alloc_end();
    assert(!cache.bytes && !cache.large_bytes && !small_used);

    wine_nx_shader_alloc_begin();
    void *p = __wrap__malloc_r(_REENT, 1024);
    assert(p);
    __wrap__free_r(_REENT, p);
    assert(cache.large_bytes == cache.bytes && cache.bytes);
    assert(__wrap__malloc_r(_REENT, 1000) == p);
    assert(!cache.bytes && !cache.large_bytes);
    __wrap__free_r(_REENT, p);
    cache_check_bounds();
    wine_nx_shader_alloc_end();
    assert(!cache.bytes && !cache.large_bytes && !small_used);
}

static void *cache_worker(void *argument)
{
    unsigned int seed = (uintptr_t)argument + 17;
    unsigned char *items[48] = {0};
    size_t sizes[48] = {0};
    for (unsigned int iteration = 0; iteration < 60000; iteration++)
    {
        if (!(iteration % 1000)) wine_nx_shader_alloc_begin();
        seed = seed * 1664525 + 1013904223;
        unsigned int slot = (seed >> 16) % 48;
        if (items[slot])
        {
            for (size_t i = 0; i < sizes[slot]; i++) assert(items[slot][i] == (unsigned char)slot);
            if (!(seed % 3))
            {
                size_t size = 1 + (seed >> 4) % 100000;
                unsigned char *p = __wrap__realloc_r(_REENT, items[slot], size);
                assert(p);
                for (size_t i = 0; i < size && i < sizes[slot]; i++) assert(p[i] == (unsigned char)slot);
                items[slot] = p;
                sizes[slot] = size;
                memset(p, slot, size);
            }
            else { __wrap__free_r(_REENT, items[slot]); items[slot] = NULL; }
        }
        else
        {
            size_t size = 1 + (seed >> 7) % 80000;
            size_t align = (size_t)16 << ((seed >> 11) % 6);
            items[slot] = __wrap__memalign_r(_REENT, align, size);
            assert(items[slot] && !((uintptr_t)items[slot] & (align - 1)));
            sizes[slot] = size;
            memset(items[slot], slot, size);
        }
        if (iteration % 1000 == 999)
        {
            cache_check_bounds();
            wine_nx_shader_alloc_end();
        }
    }
    for (unsigned int i = 0; i < 48; i++) __wrap__free_r(_REENT, items[i]);
    return NULL;
}

static void *cache_foreign_free(void *pointer)
{
    wine_nx_shader_alloc_begin();
    __wrap__free_r(_REENT, pointer);
    assert(cache.bytes);
    wine_nx_shader_alloc_end();
    return NULL;
}

static pthread_barrier_t cache_barrier;
static unsigned int enabled_workers;
static void *cache_limit_worker(void *unused)
{
    (void)unused;
    wine_nx_shader_alloc_begin();
    if (wine_nx_shader_alloc_active) __atomic_add_fetch(&enabled_workers, 1, __ATOMIC_RELAXED);
    pthread_barrier_wait(&cache_barrier);
    pthread_barrier_wait(&cache_barrier);
    wine_nx_shader_alloc_end();
    return NULL;
}

static void shader_alloc_cache_tests(void)
{
    pthread_t threads[16];
    cache_basic();
    cache_resize_and_pressure();
    cache_limits();
    cache_total_limit();
    cache_lookup();
    wine_nx_shader_alloc_begin();
    void *pointer = __wrap__malloc_r(_REENT, 1024);
    assert(pointer);
    wine_nx_shader_alloc_end();
    assert(!pthread_create(&threads[0], NULL, cache_foreign_free, pointer));
    assert(!pthread_join(threads[0], NULL));
    assert(!small_used);
    for (uintptr_t i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, cache_worker, (void *)i));
    for (unsigned int i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    assert(!small_used && !workers);
    assert(!pthread_barrier_init(&cache_barrier, NULL, 17));
    for (unsigned int i = 0; i < 16; i++) assert(!pthread_create(&threads[i], NULL, cache_limit_worker, NULL));
    pthread_barrier_wait(&cache_barrier);
    assert(enabled_workers == WORKER_LIMIT && workers == WORKER_LIMIT);
    pthread_barrier_wait(&cache_barrier);
    for (unsigned int i = 0; i < 16; i++) assert(!pthread_join(threads[i], NULL));
    assert(!pthread_barrier_destroy(&cache_barrier));
    assert(!workers && !small_used);
    puts("shader allocation cache: reuse, bounds, alignment, lifetimes and concurrent scopes passed");
}
