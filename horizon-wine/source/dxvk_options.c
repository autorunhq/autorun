#include "dxvk_options.h"

static const struct dxvk_option_choice boolean[] =
{
    { NULL, "Default" },
    { "True", "True" },
    { "False", "False" },
};

static const struct dxvk_option_choice tristate[] =
{
    { NULL, "Default" },
    { "Auto", "Auto" },
    { "True", "True" },
    { "False", "False" },
};

static const struct dxvk_option_choice threads[] =
{
    { NULL, "Default" },
    { "0", "0 (Automatic)" },
    { "1", "1" },
    { "2", "2" },
    { "3", "3" },
    { "4", "4" },
    { "6", "6" },
    { "8", "8" },
    { "12", "12" },
    { "16", "16" },
};

static const struct dxvk_option_choice memory[] =
{
    { NULL, "Default" },
    { "0", "0 (Automatic)" },
    { "128", "128" },
    { "256", "256" },
    { "512", "512" },
    { "768", "768" },
    { "1024", "1024" },
    { "1536", "1536" },
    { "2048", "2048" },
    { "3072", "3072" },
    { "4096", "4096" },
};

static const struct dxvk_option_choice texture_memory[] =
{
    { NULL, "Default" },
    { "16", "16" },
    { "32", "32" },
    { "64", "64" },
    { "100", "100" },
    { "128", "128" },
    { "256", "256" },
};

static const struct dxvk_option_choice available_memory[] =
{
    { NULL, "Default" },
    { "128", "128" },
    { "256", "256" },
    { "512", "512" },
    { "768", "768" },
    { "1024", "1024" },
    { "1536", "1536" },
    { "2048", "2048" },
    { "3072", "3072" },
    { "4096", "4096" },
};

static const struct dxvk_option_choice tessellation[] =
{
    { NULL, "Default" },
    { "0", "0 (Game setting)" },
    { "8", "8" },
    { "16", "16" },
    { "32", "32" },
    { "64", "64" },
};

static const struct dxvk_option_choice anisotropy[] =
{
    { NULL, "Default" },
    { "-1", "Game setting" },
    { "0", "Off" },
    { "2", "2" },
    { "4", "4" },
    { "8", "8" },
    { "16", "16" },
};

static const struct dxvk_option_choice floating[] =
{
    { NULL, "Default" },
    { "Auto", "Auto" },
    { "True", "True" },
    { "False", "False" },
    { "Strict", "Strict" },
};

static const struct dxvk_option_choice latency[] =
{
    { NULL, "Default" },
    { "0", "0 (Game setting)" },
    { "1", "1" },
    { "2", "2" },
    { "3", "3" },
    { "4", "4" },
    { "8", "8" },
    { "16", "16" },
};

static const struct dxvk_option_choice buffers[] =
{
    { NULL, "Default" },
    { "0", "0 (Game setting)" },
    { "1", "1" },
    { "2", "2" },
    { "3", "3" },
    { "4", "4" },
};

static const struct dxvk_option_choice lod[] =
{
    { NULL, "Default" },
    { "-2", "-2" },
    { "-1", "-1" },
    { "-0.5", "-0.5" },
    { "0", "0" },
    { "0.5", "0.5" },
    { "1", "1" },
};

static const struct dxvk_option_choice msaa[] =
{
    { NULL, "Default" },
    { "-1", "Game setting" },
    { "0", "Off" },
    { "2", "2" },
    { "4", "4" },
    { "8", "8" },
};

static const struct dxvk_option_choice resources[] =
{
    { NULL, "Default" },
    { "\"\"", "None" },
    { "\"v\"", "Vertex buffers" },
    { "\"i\"", "Index buffers" },
    { "\"c\"", "Constant buffers" },
    { "\"r\"", "Shader resources" },
    { "\"vi\"", "Vertex + index" },
    { "\"a\"", "All" },
};

static const struct dxvk_option_choice compilation[] =
{
    { NULL, "Default" },
    { "\"dyasync\"", "dyasync" },
    { "\"async\"", "async" },
    { "\"none\"", "none" },
};

static const struct dxvk_option_choice pacing[] =
{
    { NULL, "Default" },
    { "\"max-frame-latency\"", "max-frame-latency" },
    { "\"low-latency\"", "low-latency" },
    { "\"min-latency\"", "min-latency" },
};

static const struct dxvk_option_choice feature_level[] =
{
    { NULL, "Default" },
    { "9_1", "9_1" },
    { "9_2", "9_2" },
    { "9_3", "9_3" },
    { "10_0", "10_0" },
    { "10_1", "10_1" },
    { "11_0", "11_0" },
    { "11_1", "11_1" },
};

static const struct dxvk_option_choice aspect[] =
{
    { NULL, "Default" },
    { "\"\"", "All" },
    { "\"16:9\"", "16:9" },
    { "\"4:3\"", "4:3" },
    { "\"16:10\"", "16:10" },
};

static const struct dxvk_option_choice refresh[] =
{
    { NULL, "Default" },
    { "0", "0 (Game setting)" },
    { "30", "30" },
    { "60", "60" },
    { "120", "120" },
};

#define OPTION(name, help, values, first, end, sarek_first, sarek_end, advanced, gpl, default_value) \
    { name, help, default_value, values, sizeof(values) / sizeof(values[0]), { first, end }, { sarek_first, sarek_end }, advanced, gpl }

/* Release boundaries follow the option parsers, including removals and Sarek backports. */
const struct dxvk_option dxvk_options[] =
{
    OPTION( "dxvk.numCompilerThreads",
            "Compiler workers. 0 chooses automatically; fewer workers can leave more CPU time for the game.",
            threads, 10000, 0, 11003, 0, 0, 0, "0 / Auto" ),
    OPTION( "dxvk.shaderCompilationMethod",
            "Sarek compilation mode. Deferred compilation reduces stalls but can temporarily omit effects; none compiles synchronously.",
            compilation, 0, 0, 11300, 0, 0, 0, "dyasync" ),
    OPTION( "dxvk.numShaderCompilerThreads",
            "Sarek workers for deferred shader compilation, separate from state-cache compilation.",
            threads, 0, 0, 11300, 0, 0, 0, "0 / Auto" ),
    OPTION( "dxvk.enableDyasync",
            "Sarek's deferred shader compilation. Can reduce stalls while variants compile.",
            boolean, 0, 0, 11200, 11300, 0, 0, "True" ),
    OPTION( "dxvk.numDyasyncThreads",
            "Worker count for Sarek's deferred compiler. 0 selects automatically.",
            threads, 0, 0, 11200, 11300, 0, 0, "0 / Auto" ),
    OPTION( "dxvk.enableGraphicsPipelineLibrary",
            "Auto compiles optimized pipelines in the background. True skips that optimization; False disables pipeline libraries.",
            tristate, 20000, 0, 0, 0, 0, 0, "Auto" ),
    OPTION( "dxvk.trackPipelineLifetime",
            "Reclaims pipeline libraries more aggressively to save memory. Auto follows DXVK's architecture-dependent policy.",
            tristate, 20000, 0, 0, 0, 0, 0, "Auto" ),
    OPTION( "dxvk.maxMemoryBudget",
            "DXVK allocation budget in MiB. Does not add RAM or cover native Wine/translator allocations. 0 uses the driver budget.",
            memory, 20700, 0, 11300, 0, 0, 0, "0 / Auto" ),
    OPTION( "dxvk.enableStateCache",
            "Keep DXVK's legacy pipeline state cache. Disabling it can increase compilation stalls on later runs.",
            boolean, 10000, 20700, 11003, 0, 0, 0, "True" ),
    OPTION( "d3d11.maxTessFactor",
            "Caps D3D11 tessellation. Lower factors can improve GPU performance but reduce geometry detail.",
            tessellation, 10000, 0, 11003, 0, 0, 0, "0 / Game" ),
    OPTION( "d3d11.disableMsaa",
            "Forces D3D11 multisampled textures to one sample. Saves GPU work and memory but can affect rendering.",
            boolean, 10901, 0, 11003, 0, 0, 0, "False" ),
    OPTION( "d3d11.samplerAnisotropy",
            "D3D10/11 texture filtering override. Higher values sharpen angled textures at additional GPU cost; -1 leaves the game setting.",
            anisotropy, 10000, 0, 11003, 0, 0, 0, "-1 / Game" ),
    OPTION( "d3d9.samplerAnisotropy",
            "D3D8/9 texture filtering override. Higher values cost more GPU time; -1 leaves the game setting.",
            anisotropy, 10500, 0, 11003, 0, 0, 0, "-1 / Game" ),
    OPTION( "d3d9.floatEmulation",
            "D3D9 shader floating-point emulation. Auto follows the driver; disabling can improve speed but change rendering.",
            tristate, 10500, 10903, 0, 0, 0, 0, "Auto" ),
    OPTION( "d3d9.floatEmulation",
            "Shader floating-point accuracy. False disables emulation, True is faster, Strict is more accurate; changes can affect rendering.",
            floating, 10903, 0, 11003, 0, 0, 0, "Auto" ),
    OPTION( "dxgi.maxDeviceMemory",
            "Video memory reported to D3D10/11/12 games, in MiB. Changes game decisions, not the actual RAM available.",
            memory, 10000, 0, 11003, 0, 1, 0, "0 / Auto" ),
    OPTION( "dxgi.maxSharedMemory",
            "Shared memory reported to D3D10/11/12 games, in MiB. Does not reserve memory or increase available RAM.",
            memory, 10000, 0, 11003, 0, 1, 0, "0 / Auto" ),
    OPTION( "d3d9.maxAvailableMemory",
            "Initial available texture memory reported to D3D9 games, in MiB. Lower values may reduce game-side allocations.",
            available_memory, 10500, 0, 11003, 0, 1, 0, "4096 MiB" ),
    OPTION( "d3d9.textureMemory",
            "D3D9 texture upload buffer size in MiB. Lower values save memory but can cause more upload stalls.",
            texture_memory, 20000, 0, 0, 0, 1, 0, "100 MiB" ),
    OPTION( "dxgi.maxFrameLatency",
            "Maximum queued frames for DXGI. Lower values reduce latency but can reduce throughput; 0 leaves the application setting.",
            latency, 10000, 0, 11003, 0, 1, 0, "0 / Game" ),
    OPTION( "d3d9.maxFrameLatency",
            "Maximum queued D3D9 frames. Lower values reduce latency but can reduce throughput; 0 leaves the application setting.",
            latency, 10500, 0, 11003, 0, 1, 0, "0 / Game" ),
    OPTION( "dxgi.numBackBuffers",
            "DXGI back-buffer count override. More buffers consume memory; 0 leaves the game setting.",
            buffers, 10000, 20600, 11003, 0, 1, 0, "0 / Game" ),
    OPTION( "d3d9.numBackBuffers",
            "D3D9 back-buffer count override. More buffers consume memory; 0 leaves the game setting.",
            buffers, 10500, 20600, 11003, 0, 1, 0, "0 / Game" ),
    OPTION( "dxvk.framePace",
            "Sarek pacing policy. Lower-latency modes may trade frame rate for responsiveness.",
            pacing, 0, 0, 11300, 0, 1, 0, "max-frame-latency" ),
    OPTION( "d3d11.samplerLodBias",
            "Adds to the D3D11 texture mip bias. Positive values reduce texture detail; negative values may cause shimmering.",
            lod, 20000, 0, 0, 0, 1, 0, "0" ),
    OPTION( "d3d9.samplerLodBias",
            "Adds to the D3D9 texture mip bias. Positive values reduce detail; negative values may cause shimmering.",
            lod, 20300, 0, 0, 0, 1, 0, "0" ),
    OPTION( "d3d11.clampNegativeLodBias",
            "Clamps negative D3D11 mip bias to zero, which can reduce texture shimmering.",
            boolean, 20300, 0, 0, 0, 1, 0, "False" ),
    OPTION( "d3d9.clampNegativeLodBias",
            "Clamps negative D3D9 mip bias to zero, which can reduce texture shimmering.",
            boolean, 20300, 0, 0, 0, 1, 0, "False" ),
    OPTION( "d3d9.forceSwapchainMSAA",
            "D3D9 swap-chain multisampling override. 0 disables it; -1 leaves the game setting.",
            msaa, 10502, 20700, 11003, 0, 1, 0, "-1 / Game" ),
    OPTION( "dxvk.enableMemoryDefrag",
            "Defragments DXVK video memory. Auto respects driver exclusions; disabling it may increase wasted memory.",
            tristate, 20500, 0, 0, 0, 1, 0, "Auto" ),
    OPTION( "dxvk.enableDescriptorBuffer",
            "Use descriptor buffers when supported. Default is False with the HUD enabled to preserve HUD text, otherwise Auto. Descriptor heaps take precedence when enabled.",
            tristate, 20700, 0, 0, 0, 1, 0, "Auto" ),
    OPTION( "dxvk.enableDescriptorHeap",
            "Use descriptor heaps when supported by the driver. Auto follows DXVK's driver policy; this takes precedence over descriptor buffers.",
            tristate, 30000, 0, 0, 0, 1, 0, "Auto" ),
    OPTION( "dxvk.enableUnifiedImageLayouts",
            "Use DXVK's unified image layout path. Disable only to investigate rendering or driver compatibility issues.",
            boolean, 30000, 0, 0, 0, 1, 0, "True" ),
    OPTION( "dxvk.enableImplicitResolves",
            "Resolve multisampled images when a game reads them as regular textures. Disabling can cause striped rendering on NVIDIA GPUs.",
            boolean, 30000, 0, 0, 0, 1, 0, "True" ),
    OPTION( "dxvk.latencySleep",
            "Built-in latency reduction. True enables timing-based waits where supported; it can reduce throughput in some games. Auto follows DXVK's policy.",
            tristate, 20600, 0, 0, 0, 1, 0, "Auto" ),
    OPTION( "dxvk.tearFree",
            "True requests mailbox presentation with VSync off; False allows relaxed FIFO with VSync on. Requires driver support.",
            tristate, 20300, 0, 0, 0, 1, 0, "Auto" ),
    OPTION( "dxvk.useRawSsbo",
            "Raw buffer access path. Auto follows device alignment requirements; forcing it can break unsupported layouts.",
            tristate, 10000, 0, 11003, 0, 1, 0, "Auto" ),
    OPTION( "d3d11.cachedDynamicResources",
            "Use CPU-cached memory for selected dynamic buffers. Can improve readback but reduce GPU performance.",
            resources, 11000, 0, 11003, 0, 1, 0, "None" ),
    OPTION( "d3d9.deviceLocalConstantBuffers",
            "Place D3D9 constants in device-local memory. This changes upload behavior and is not always faster on a shared-memory GPU.",
            boolean, 10801, 30000, 11003, 11300, 1, 0, "False" ),
    OPTION( "d3d9.deviceLocalConstantBuffers",
            "Place D3D9 constants in device-local memory. This changes upload behavior and is not always faster on a shared-memory GPU.",
            tristate, 30000, 0, 11300, 0, 1, 0, "Auto" ),
    OPTION( "d3d9.cachedDynamicBuffers",
            "Use CPU-cached dynamic D3D9 buffers. Useful for readback-heavy games, but may slow GPU access.",
            boolean, 20300, 30000, 11006, 11300, 1, 0, "False" ),
    OPTION( "d3d9.cachedWriteOnlyBuffers",
            "Use CPU-cached write-only D3D9 buffers. Can help games that read from these buffers.",
            boolean, 30000, 0, 11300, 0, 1, 0, "False" ),
    OPTION( "d3d9.allowDirectBufferMapping",
            "Allow direct D3D9 buffer mapping. Disable only for compatibility with games that misuse mapped buffers.",
            boolean, 11000, 0, 11003, 0, 1, 0, "True" ),
    OPTION( "d3d11.disableDirectImageMapping",
            "Work around games that assume a tightly packed row pitch when mapping textures.",
            boolean, 20602, 0, 0, 0, 1, 0, "False" ),
    OPTION( "d3d11.enableContextLock",
            "Serialize D3D11 immediate-context access. Can fix application races but adds synchronization overhead.",
            boolean, 20000, 0, 11300, 0, 1, 0, "False" ),
    OPTION( "d3d11.exposeDriverCommandLists",
            "Report D3D11 driver command-list support. Some games choose a different threading path based on this flag.",
            boolean, 20301, 0, 11300, 0, 1, 0, "True" ),
    OPTION( "d3d11.constantBufferRangeCheck",
            "Emulate out-of-bounds constant-buffer reads. Can fix older games at some shader cost.",
            boolean, 10003, 20000, 11003, 0, 1, 0, "False" ),
    OPTION( "d3d11.zeroInitWorkgroupMemory",
            "Initialize compute workgroup memory to zero for games relying on undefined initial contents.",
            boolean, 10000, 20700, 11003, 0, 1, 0, "False" ),
    OPTION( "d3d11.forceTgsmBarriers",
            "Extra compute shared-memory barriers for games with missing synchronization. May reduce performance.",
            boolean, 10505, 20000, 11003, 11300, 1, 0, "False" ),
    OPTION( "d3d11.forceVolatileTgsmAccess",
            "Treat compute shared-memory accesses as volatile for compatibility. May reduce shader performance.",
            boolean, 20000, 30000, 0, 0, 1, 0, "False" ),
    OPTION( "d3d11.forceComputeLdsBarriers",
            "Insert barriers after compute shared-memory writes. Intended for games with missing synchronization.",
            boolean, 30000, 0, 11300, 0, 1, 0, "False" ),
    OPTION( "d3d11.forceComputeUavBarriers",
            "Insert additional compute UAV barriers. Can work around missing game synchronization at a performance cost.",
            boolean, 20600, 0, 0, 0, 1, 0, "False" ),
    OPTION( "dxvk.lowerSinCos",
            "Use custom sine/cosine approximations. Auto follows the driver's precision requirements.",
            tristate, 30000, 0, 11300, 0, 1, 0, "Auto" ),
    OPTION( "d3d11.sincosEmulation",
            "Legacy D3D11 sine/cosine approximation control. Auto follows driver behavior.",
            tristate, 20602, 30000, 0, 0, 1, 0, "Auto" ),
    OPTION( "d3d9.sincosEmulation",
            "Legacy D3D9 sine/cosine approximation control. Auto follows driver behavior.",
            tristate, 20602, 30000, 0, 0, 1, 0, "Auto" ),
    OPTION( "d3d11.floatControls",
            "Preserve D3D11 floating-point behavior. Disabling can change shader output and precision.",
            boolean, 10800, 30000, 11003, 0, 1, 0, "True" ),
    OPTION( "d3d9.strictPow",
            "More accurate D3D9 power operations. Disabling can improve speed but change lighting.",
            boolean, 10500, 30000, 11003, 0, 1, 0, "True" ),
    OPTION( "d3d9.useFP16",
            "Use half-precision D3D9 shader math where allowed. Can reduce shader cost at the expense of precision.",
            tristate, 30000, 0, 0, 0, 1, 0, "False" ),
    OPTION( "d3d9.generalHazards",
            "Track D3D9 resource hazards. Disabling checks can break games that depend on them.",
            tristate, 10500, 20000, 11003, 0, 1, 0, "Auto" ),
    OPTION( "d3d9.lenientClear",
            "Fast-path nearly full render-target clears. Can help performance but may clear pixels outside the requested area.",
            boolean, 10500, 0, 11003, 0, 1, 0, "False" ),
    OPTION( "d3d9.forceSamplerTypeSpecConstants",
            "Handle games that bind textures with a different type than their shader expects.",
            boolean, 10501, 0, 11003, 0, 1, 0, "False" ),
    OPTION( "dxgi.deferSurfaceCreation",
            "Create the DXGI surface on first Present. Can help games that change graphics APIs during startup.",
            boolean, 10000, 0, 11003, 0, 1, 0, "False" ),
    OPTION( "d3d9.deferSurfaceCreation",
            "Create the D3D9 surface on first Present. Can help games that change graphics APIs during startup.",
            boolean, 10500, 0, 11003, 0, 1, 0, "False" ),
    OPTION( "d3d11.maxFeatureLevel",
            "Maximum D3D11 feature level exposed to the game. This does not add unsupported Vulkan features.",
            feature_level, 10101, 0, 11003, 0, 1, 0, "Device limit" ),
    OPTION( "d3d9.forceAspectRatio",
            "Only advertise display modes with this aspect ratio. Default keeps the full mode list.",
            aspect, 10500, 0, 11003, 0, 1, 0, "All" ),
    OPTION( "dxgi.forceRefreshRate",
            "Restrict DXGI display modes to a refresh rate. Does not change the Switch display refresh rate.",
            refresh, 20700, 0, 11300, 0, 1, 0, "0 / Game" ),
    OPTION( "d3d9.forceRefreshRate",
            "Restrict D3D9 display modes to a refresh rate. Does not change the Switch display refresh rate.",
            refresh, 20700, 0, 11300, 0, 1, 0, "0 / Game" ),
    OPTION( "d3d9.modeCountCompatibility",
            "Limit the number of reported D3D9 display modes for games that cannot handle long lists.",
            boolean, 20701, 0, 0, 0, 1, 0, "False" ),
    OPTION( "dxgi.emulateUMA",
            "Report unified memory to DXGI games. Changes memory reporting, not the physical allocation layout.",
            boolean, 10900, 20401, 11003, 0, 1, 0, "False" ),
    OPTION( "dxgi.hideNvidiaGpu",
            "Hide NVIDIA identity from DXGI games to avoid vendor-specific paths. Auto keeps DXVK's policy.",
            tristate, 20300, 0, 11005, 0, 1, 0, "Auto" ),
    OPTION( "dxgi.hideNvkGpu",
            "Hide NVK identity from DXGI games. Auto keeps DXVK's compatibility policy.",
            tristate, 20301, 0, 11300, 0, 1, 0, "Auto" ),
    OPTION( "d3d9.hideNvidiaGpu",
            "Hide NVIDIA identity from D3D9 games. Auto keeps DXVK's compatibility policy.",
            tristate, 20602, 0, 11300, 0, 1, 0, "Auto" ),
    OPTION( "d3d9.hideNvkGpu",
            "Hide NVK identity from D3D9 games. Auto keeps DXVK's compatibility policy.",
            tristate, 20602, 0, 11300, 0, 1, 0, "Auto" ),
    OPTION( "dxvk.gplAsyncCache",
            "GPLAsync's legacy state-cache integration. Available here for 2.3 through 2.6; removed in 2.7 and newer.",
            boolean, 20300, 20700, 0, 0, 1, 1, "False" ),
};
#undef OPTION

const unsigned int dxvk_option_count = sizeof(dxvk_options) / sizeof(dxvk_options[0]);

unsigned int dxvk_options_version( enum dxvk_source source, const char *text )
{
    unsigned long part[3] = {0};
    unsigned int version;
    const char *p = text;
    char *end;

    if (source < 0 || source >= DXVK_SOURCE_COUNT || !p || !*p) return 0;
    if (*p == 'v') p++;
    for (int i = 0; i < 3; i++)
    {
        if (!isdigit( (unsigned char)*p )) return 0;
        part[i] = strtoul( p, &end, 10 );
        if (part[i] > 99) return 0;
        p = end;
        if (i == 2 || *p != '.') break;
        p++;
    }
    if (*p == '-' && source != DXVK_SOURCE_OFFICIAL)
    {
        p++;
        if (!isdigit( (unsigned char)*p )) return 0;
        while (isdigit( (unsigned char)*p )) p++;
    }
    if (*p || !part[0]) return 0;
    version = part[0] * 10000 + part[1] * 100 + part[2];
    if (source == DXVK_SOURCE_SAREK) return version <= 11300 ? version : 0;
    if (source == DXVK_SOURCE_GPLASYNC && version < 20100) return 0;
    return version <= 30101 ? version : 0;
}

int dxvk_option_supported( const struct dxvk_option *option, enum dxvk_source source, unsigned int version )
{
    const struct dxvk_option_range *range;

    if (!version || source < 0 || source >= DXVK_SOURCE_COUNT ||
        (option->gpl_only && source != DXVK_SOURCE_GPLASYNC)) return 0;
    range = source == DXVK_SOURCE_SAREK && version >= 11003 ? &option->sarek : &option->official;
    return range->first && version >= range->first && (!range->end || version < range->end);
}

int dxvk_option_value( const struct launcher_kv *kv, const struct dxvk_option *option )
{
    char value[64];

    if (!launcher_kv_get( kv, option->name, value, sizeof(value) )) return 0;
    for (unsigned int i = 1; i < option->choice_count; i++)
        if (!strcasecmp( value, option->choices[i].value )) return i;
    return -1;
}

const char *dxvk_option_default( const struct dxvk_option *option, enum dxvk_source source,
                                 unsigned int version, int hud )
{
    if (hud && !strcmp( option->name, "dxvk.enableDescriptorBuffer" )) return "False / HUD";
    if (source == DXVK_SOURCE_GPLASYNC && version >= 20600 &&
        !strcmp( option->name, "dxvk.gplAsyncCache" )) return "True";
    if (source != DXVK_SOURCE_SAREK && version == 10903 &&
        !strcmp( option->name, "d3d9.floatEmulation" )) return "True";
    return option->default_value;
}

int dxvk_options_apply( const struct launcher_kv *kv, enum dxvk_source source, const char *version,
                        char *config, size_t size )
{
    unsigned int release = dxvk_options_version( source, version );
    size_t length = strnlen( config, size ), required = length;

    if (length == size) return 0;
    for (unsigned int i = 0; i < dxvk_option_count; i++)
    {
        const struct dxvk_option *option = dxvk_options + i;
        int value;

        if (!dxvk_option_supported( option, source, release ) ||
            (value = dxvk_option_value( kv, option )) <= 0) continue;
        required += strlen( option->name ) + strlen( option->choices[value].value ) + 4;
        if (required >= size) return 0;
    }
    for (unsigned int i = 0; i < dxvk_option_count; i++)
    {
        const struct dxvk_option *option = dxvk_options + i;
        int value;

        if (!dxvk_option_supported( option, source, release ) ||
            (value = dxvk_option_value( kv, option )) <= 0) continue;
        length += snprintf( config + length, size - length, "%s = %s\n",
                            option->name, option->choices[value].value );
    }
    return 1;
}
