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
    /* From here on, what only the FEX build with Carafe's patches reads, or
     * what only it makes work here (carafe/dlls/fex). */
    NX_FEX_DISK_CACHE,
    NX_FEX_CARAFE_FIRST = NX_FEX_DISK_CACHE,
    NX_FEX_DISK_CACHE_MAPPING,
    NX_FEX_X87_CONVERSION_ROUNDING,
    NX_FEX_X87_FAST_STACK,
    NX_FEX_X87_SINGLE_PRECISION,
    NX_FEX_GDB_SYMBOLS,
    NX_FEX_FLAGS_WITHOUT_FLAGM,
    NX_FEX_INDIRECT_CALL_CACHE,
    NX_FEX_BRIDGE_RETURN,
    NX_FEX_JUMP_TABLES,
    NX_FEX_COMPACT_RETURNS,
    NX_FEX_SLIM_RETURN_POINTS,
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
    /* The defaults are Carafe's: FEX itself has each of its own options off. */
    { NX_FEX_DISK_CACHE, "FEX_DISKCACHE",
      "Keeps translated code on disk and reuses it on the next start instead of translating it again.",
      1, 0, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_DISK_CACHE_MAPPING, "FEX_DISKCACHEFILEMAPPING",
      "Maps disk cache files into memory instead of reading them. Horizon's unix side has no file mapping.",
      1, 0, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_X87_CONVERSION_ROUNDING, "FEX_X87CONVERSIONROUNDING",
      "With reduced x87 precision, rounding mode changes apply only to float to integer conversions, without touching the host rounding mode.",
      1, 1, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_X87_FAST_STACK, "FEX_X87FASTSTACK",
      "Keeps x87 registers of earlier blocks out of the slow path and writes back only what a block changed.",
      1, 1, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_X87_SINGLE_PRECISION, "FEX_X87SINGLEPRECISION",
      "With reduced x87 precision, computes x87 math in 32-bit floats, as Direct3D 9 games set the precision to.",
      1, 0, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_GDB_SYMBOLS, "FEX_GDBSYMBOLS",
      "Records every x86 instruction in each translated block's address map, so the profiler names each one. The code is the same.",
      1, 0, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_FLAGS_WITHOUT_FLAGM, "FEX_FLAGSWITHOUTFLAGM",
      "On CPUs without FlagM, spends fewer instructions on x86 flags.",
      1, 1, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_INDIRECT_CALL_CACHE, "FEX_INDIRECTCALLCACHE",
      "Each indirect call keeps the first target it reaches, so calls to it skip the lookup table.",
      1, 0, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_BRIDGE_RETURN, "FEX_BRIDGERETURN",
      "The system-call and unix-call bridge leaves as a return, so the call-return stack stays in step.",
      1, 1, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_JUMP_TABLES, "FEX_JUMPTABLES",
      "A switch jumps straight to its cases inside the translated block instead of through the lookup table.",
      1, 1, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_COMPACT_RETURNS, "FEX_COMPACTRETURNS",
      "Returns keep their rarely used lookup outside the hot code, so more of it fits the instruction cache.",
      1, 1, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
    { NX_FEX_SLIM_RETURN_POINTS, "FEX_SLIMRETURNPOINTS",
      "The code after a call skips storing its block's header; faults and the profiler find the block themselves.",
      1, 1, 2, { "0", "1" }, { "0 - Off", "1 - On" } },
};

static inline int nx_fex_option_choice( const struct nx_fex_option *option, const char *value )
{
    int i;

    if (!option || !value) return -1;
    for (i = 0; i < option->value_count; i++)
        if (!strcasecmp( option->values[i], value )) return i;
    return -1;
}

/* A program on the stock FEX is neither shown nor given these: it knows three
 * of them by name and has none of the code behind them here. */
static inline int nx_fex_option_carafe( const struct nx_fex_option *option )
{
    return option->id >= NX_FEX_CARAFE_FIRST;
}

static inline const char *nx_fex_option_default( const struct nx_fex_option *option )
{
    return option->values[option->default_choice];
}

#endif
