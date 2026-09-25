#ifndef WINE_NX_FEX_OPTIONS_H
#define WINE_NX_FEX_OPTIONS_H

#include <string.h>
#include <strings.h>

#define NX_FEX_MAX_VALUES 7

enum nx_fex_option_id
{
    NX_FEX_TSO,
    NX_FEX_MULTIBLOCK,
    NX_FEX_MAXINST,
    NX_FEX_SMC,
    NX_FEX_DISABLE_L2,
    NX_FEX_DYNAMIC_L1,
    NX_FEX_X87_REDUCED,
    NX_FEX_VECTOR_TSO,
    NX_FEX_MEMCPY_TSO,
    NX_FEX_HALF_BARRIER,
    NX_FEX_STRICT_SPLIT_LOCKS,
    NX_FEX_UNALIGNED_BACKPATCH,
    NX_FEX_VOLATILE_METADATA,
    NX_FEX_MONO_HACKS,
    NX_FEX_HIDE_HYPERVISOR,
    NX_FEX_SMALL_TSC,
    NX_FEX_HIDE_HYBRID,
    NX_FEX_L1_INCREASE,
    NX_FEX_L1_DECREASE,
    NX_FEX_OPTION_COUNT
};

struct nx_fex_option
{
    enum nx_fex_option_id id;
    const char *name;
    const char *help;
    unsigned char advanced;
    unsigned char default_choice;
    unsigned char value_count;
    const char *values[NX_FEX_MAX_VALUES];
    const char *value_names[NX_FEX_MAX_VALUES];
};

static const struct nx_fex_option nx_fex_options[NX_FEX_OPTION_COUNT] =
{
    { NX_FEX_TSO, "FEX_TSOENABLED",
      "Emulates x86 total store ordering. Off is faster and is Autorun's default; enable it for games with memory-ordering bugs.",
      0, 0, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_MULTIBLOCK, "FEX_MULTIBLOCK",
      "Compiles multiple connected guest blocks together. This usually improves execution speed but increases compilation work and code size.",
      0, 1, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_MAXINST, "FEX_MAXINST",
      "Maximum guest instructions in one translated block. Smaller blocks use less code memory; larger blocks allow more optimization.",
      0, 2, 7, { "250", "500", "1000", "2500", "5000", "10000", "20000" },
      { "250", "500", "1000", "2500", "5000", "10000", "20000" } },
    { NX_FEX_SMC, "FEX_SMCCHECKS",
      "Checks translated code for self-modification. MTrack is the normal page-tracking mode; Full validates every execution and is slow.",
      0, 0, 3, { "none", "mtrack", "full" }, { "None", "MTrack", "Full" } },
    { NX_FEX_DISABLE_L2, "FEX_DISABLEL2CACHE",
      "Disables the JIT L2 lookup cache to save memory. Turning it off may reduce lookup stutter at a higher memory cost.",
      0, 1, 2, { "0", "1" }, { "0 - L2 enabled", "1 - L2 disabled" } },
    { NX_FEX_DYNAMIC_L1, "FEX_DYNAMICL1CACHE",
      "Dynamically sizes each thread's JIT L1 cache to reduce memory use.",
      0, 1, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_X87_REDUCED, "FEX_X87REDUCEDPRECISION",
      "Uses 64-bit x87 precision for speed. This may cause numerical or rendering differences in software that expects extended precision.",
      0, 1, 2, { "0", "1" }, { "0 - Full precision", "1 - Reduced" } },
    { NX_FEX_VECTOR_TSO, "FEX_VECTORTSOENABLED",
      "Applies TSO emulation to vector loads and stores when FEX_TSOENABLED is on.",
      1, 0, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_MEMCPY_TSO, "FEX_MEMCPYSETTSOENABLED",
      "Applies TSO emulation to REP MOVS and REP STOS when FEX_TSOENABLED is on.",
      1, 0, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_HALF_BARRIER, "FEX_HALFBARRIERTSOENABLED",
      "Backpatches unaligned accesses to half-barrier atomics when TSO is enabled.",
      1, 1, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_STRICT_SPLIT_LOCKS, "FEX_STRICTINPROCESSSPLITLOCKS",
      "Serializes split locks within the process so a cross-boundary atomic cannot tear.",
      1, 0, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_UNALIGNED_BACKPATCH, "FEX_KERNELUNALIGNEDATOMICBACKPATCHING",
      "Backpatches unaligned atomics after the first fault to avoid repeated exception handling.",
      1, 1, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_VOLATILE_METADATA, "FEX_VOLATILEMETADATA",
      "Uses PE volatile metadata to omit memory-ordering operations where the executable marks them unnecessary.",
      1, 1, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_MONO_HACKS, "FEX_MONOHACKS",
      "Uses FEX's Mono-specific self-modifying-code handling and smaller blocks when Mono is detected.",
      1, 1, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_HIDE_HYPERVISOR, "FEX_HIDEHYPERVISORBIT",
      "Hides the hypervisor CPUID bit for applications that reject virtualized CPUs.",
      1, 0, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_SMALL_TSC, "FEX_SMALLTSCSCALE",
      "Scales a low-frequency host counter to the minimum frequency expected by x86 software.",
      1, 1, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_HIDE_HYBRID, "FEX_HIDEHYBRID",
      "Hides hybrid CPU topology from the guest.",
      1, 1, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_L1_INCREASE, "FEX_DYNAMICL1CACHEINCREASECOUNTHEURISTIC",
      "Lookup-rate threshold for growing the dynamic L1 cache. Lower values grow it more aggressively.",
      1, 2, 5, { "50", "100", "250", "500", "1000" }, { "50", "100", "250", "500", "1000" } },
    { NX_FEX_L1_DECREASE, "FEX_DYNAMICL1CACHEDECREASECOUNTHEURISTIC",
      "Lookup-rate threshold for shrinking the dynamic L1 cache. Higher values reclaim memory more aggressively.",
      1, 2, 5, { "10", "25", "50", "100", "200" }, { "10", "25", "50", "100", "200" } },
};

static inline int nx_fex_option_choice( const struct nx_fex_option *option, const char *value )
{
    int i;

    if (!option || !value) return -1;
    for (i = 0; i < option->value_count; i++)
        if (!strcasecmp( option->values[i], value )) return i;
    return -1;
}

static inline const char *nx_fex_option_default( const struct nx_fex_option *option )
{
    return option->values[option->default_choice];
}

#endif
