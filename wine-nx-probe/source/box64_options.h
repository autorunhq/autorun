/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later. */
#ifndef WINE_NX_BOX64_OPTIONS_H
#define WINE_NX_BOX64_OPTIONS_H

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define NX_BOX64_MAX_VALUES 5

enum nx_box64_option_id
{
    NX_BOX64_ALIGNED_ATOMICS,
    NX_BOX64_BIGBLOCK,
    NX_BOX64_CALLRET,
    NX_BOX64_SAFEFLAGS,
    NX_BOX64_STRONGMEM,
    NX_BOX64_FORWARD,
    NX_BOX64_DF,
    NX_BOX64_DIV0,
    NX_BOX64_FASTNAN,
    NX_BOX64_FASTROUND,
    NX_BOX64_NATIVEFLAGS,
    NX_BOX64_NOARCH,
    NX_BOX64_PAUSE,
    NX_BOX64_SEP,
    NX_BOX64_WEAKBARRIER,
    NX_BOX64_X87DOUBLE,
    NX_BOX64_PURGE,
    NX_BOX64_PURGE_AGE,
    NX_BOX64_OPTION_COUNT
};

struct nx_box64_option
{
    enum nx_box64_option_id id;
    const char *name;
    const char *help;
    unsigned char advanced;
    unsigned short default_value;
    unsigned char value_count;
    short values[NX_BOX64_MAX_VALUES];
    const char *value_names[NX_BOX64_MAX_VALUES];
};

static const struct nx_box64_option nx_box64_options[NX_BOX64_OPTION_COUNT] =
{
    { NX_BOX64_ALIGNED_ATOMICS, "BOX64_DYNAREC_ALIGNED_ATOMICS",
      "Aligned-only atomics reduce generated code, but a misaligned LOCK operation will crash.",
      0, 0, 2, { 0, 1 }, { "0 - Safe", "1 - Faster" } },
    { NX_BOX64_BIGBLOCK, "BOX64_DYNAREC_BIGBLOCK",
      "Larger translated blocks can improve speed. Level 0 suits self-modifying or heavily threaded code; level 3 suits many Wine games.",
      0, 2, 4, { 0, 1, 2, 3 }, { "0 - Small", "1 - Large", "2 - Balanced", "3 - Wine" } },
    { NX_BOX64_CALLRET, "BOX64_DYNAREC_CALLRET",
      "Optimizes CALL and RET. Level 2 uses Wine-NX's guarded return path so changed blocks remain safe.",
      0, 2, 3, { 0, 1, 2 }, { "0 - Off", "1 - Fast", "2 - Guarded" } },
    { NX_BOX64_SAFEFLAGS, "BOX64_DYNAREC_SAFEFLAGS",
      "Controls x86 flag handling around calls and returns. Lower values are faster; higher values cover more edge cases.",
      0, 1, 3, { 0, 1, 2 }, { "0 - Fast", "1 - Balanced", "2 - Safe" } },
    { NX_BOX64_STRONGMEM, "BOX64_DYNAREC_STRONGMEM",
      "Adds barriers to approximate x86 memory ordering. Raise this when a multithreaded game hangs or behaves incorrectly.",
      0, 0, 5, { 0, 1, 2, 3, 4 }, { "0 - Off", "1 - Basic", "2 - SIMD", "3 - Strong", "4 - TSO" } },
    { NX_BOX64_FORWARD, "BOX64_DYNAREC_FORWARD",
      "Maximum gap, in bytes, followed while building a translated block. Larger values can improve performance and increase code size.",
      0, 128, 5, { 0, 128, 256, 512, 1024 }, { "0", "128", "256", "512", "1024" } },
    { NX_BOX64_DF, "BOX64_DYNAREC_DF",
      "Defers x86 flag calculation until the flags are needed.",
      1, 1, 2, { 0, 1 }, { "0 - Off", "1 - On" } },
    { NX_BOX64_DIV0, "BOX64_DYNAREC_DIV0",
      "Generates the x86 divide-by-zero exception. Wine-NX enables this by default for compatibility.",
      1, 1, 2, { 0, 1 }, { "0 - Off", "1 - On" } },
    { NX_BOX64_FASTNAN, "BOX64_DYNAREC_FASTNAN",
      "Uses faster native NaN behavior instead of precisely reproducing x86 negative NaNs.",
      1, 1, 2, { 0, 1 }, { "0 - Precise", "1 - Fast" } },
    { NX_BOX64_FASTROUND, "BOX64_DYNAREC_FASTROUND",
      "Controls floating-point rounding accuracy. Level 1 is fastest; level 0 is most precise.",
      1, 1, 3, { 0, 1, 2 }, { "0 - Precise", "1 - Fast", "2 - Hybrid" } },
    { NX_BOX64_NATIVEFLAGS, "BOX64_DYNAREC_NATIVEFLAGS",
      "Uses ARM64 condition flags directly when possible.",
      1, 1, 2, { 0, 1 }, { "0 - Off", "1 - On" } },
    { NX_BOX64_NOARCH, "BOX64_DYNAREC_NOARCH",
      "Reduces per-block metadata. Higher values save memory but can break signal handling or protected software.",
      1, 0, 3, { 0, 1, 2 }, { "0 - Full", "1 - Reduced", "2 - Minimal" } },
    { NX_BOX64_PAUSE, "BOX64_DYNAREC_PAUSE",
      "Selects how x86 PAUSE behaves in spin loops. The best mode depends on the game's threading.",
      1, 0, 4, { 0, 1, 2, 3 }, { "0 - Ignore", "1 - Yield", "2 - WFI", "3 - WFE" } },
    { NX_BOX64_SEP, "BOX64_DYNAREC_SEP",
      "Lets optimized returns re-enter a translated block. Wine-NX defaults to level 2 because PE mappings are not reported as ELF mappings.",
      1, 2, 3, { 0, 1, 2 }, { "0 - Off", "1 - Files", "2 - All" } },
    { NX_BOX64_WEAKBARRIER, "BOX64_DYNAREC_WEAKBARRIER",
      "Reduces the cost of memory barriers. Higher values are faster and less conservative.",
      1, 1, 3, { 0, 1, 2 }, { "0 - Safe", "1 - Weak", "2 - Weakest" } },
    { NX_BOX64_X87DOUBLE, "BOX64_DYNAREC_X87DOUBLE",
      "Controls when x87 values use double precision instead of the faster float path.",
      1, 0, 3, { 0, 1, 2 }, { "0 - Automatic", "1 - Double", "2 - Honor control" } },
    { NX_BOX64_PURGE, "BOX64_DYNAREC_PURGE",
      "Once code memory is full, lets translated code the game has not run for a while give its room back, so new code is translated instead of interpreted. Costs a little on every block; for games that run out of code memory, as on a 32-bit address space.",
      1, 0, 2, { 0, 1 }, { "0 - Off", "1 - On" } },
    { NX_BOX64_PURGE_AGE, "BOX64_DYNAREC_PURGE_AGE",
      "How long translated code has to go unused before a purge may take it back, in 10 ms ticks.",
      1, 1000, 4, { 500, 1000, 3000, 6000 }, { "5 s", "10 s", "30 s", "60 s" } },
};

static inline const struct nx_box64_option *nx_box64_option_find( const char *name )
{
    size_t i;

    for (i = 0; i < NX_BOX64_OPTION_COUNT; i++)
        if (!strcmp( name, nx_box64_options[i].name )) return nx_box64_options + i;
    return NULL;
}

static inline int nx_box64_option_choice( const struct nx_box64_option *option, long value )
{
    int i;

    if (!option) return -1;
    for (i = 0; i < option->value_count; i++)
        if (option->values[i] == value) return i;
    return -1;
}

static inline int nx_box64_option_parse_value( const char *text, long *value )
{
    char *end;

    while (isspace( (unsigned char)*text )) text++;
    *value = strtol( text, &end, 0 );
    if (end == text) return 0;
    while (isspace( (unsigned char)*end )) end++;
    return !*end || *end == '#';
}

/* The option on one line; 0 for a blank line, a comment or anything else. */
static inline int nx_box64_option_line( const char *line, char *name, size_t size, long *value )
{
    const char *end, *equals;
    size_t length;

    while (isspace( (unsigned char)*line )) line++;
    if (!*line || *line == '#' || !(equals = strchr( line, '=' ))) return 0;
    for (end = equals; end > line && isspace( (unsigned char)end[-1] ); end--) continue;
    length = end - line;
    if (!length || length >= size) return 0;
    memcpy( name, line, length );
    name[length] = 0;
    return nx_box64_option_parse_value( equals + 1, value );
}

#endif
