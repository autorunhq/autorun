/*
 * Vulkan display driver loading
 *
 * Copyright (c) 2017 Roderick Colenbrander
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <math.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "win32u_private.h"
#include "ntuser_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

#ifdef WINE_NX_SWAP_POC
#include "../ntdll/unix/horizon_swap.h"
#endif

static PFN_vkGetDeviceProcAddr p_vkGetDeviceProcAddr;
static PFN_vkGetInstanceProcAddr p_vkGetInstanceProcAddr;
static PFN_vkCreateInstance p_vkCreateInstance;
static PFN_vkEnumerateInstanceExtensionProperties p_vkEnumerateInstanceExtensionProperties;

static void *vulkan_handle;
static struct vulkan_funcs vulkan_funcs;
static pthread_mutex_t present_lock = PTHREAD_MUTEX_INITIALIZER;

#ifdef WINE_NX_LSFG
#include "../../wine-nx-probe/source/lsfg.h"
#endif
#ifdef __SWITCH__
#include "../../wine-nx-probe/source/osk.h"
#endif

/* The Upscaling setting a scaled swapchain is made with: 0 bilinear, 1 FSR 1.0
 * (EASU then RCAS, tools/fshack_*.comp), 2 whole-pixel steps with nearest
 * sampling. sharpness is 0 to 1 of RCAS's range. Only the Switch runtime sets
 * them (wine_nx_upscaling_configure). */
static int nx_upscaling_mode;
static float nx_upscaling_sharpness = 0.4f;

#if defined(__SWITCH__) && defined(WINE_NX_MESA_SWITCH)
/* mesa-switch's loaderless NVK (build-mesa-switch.sh) is linked into the Switch
 * runtime, which has no dynamic linker. */
extern PFN_vkVoidFunction wine_nx_vkGetDeviceProcAddr( VkDevice device, const char *name ) __asm__("vkGetDeviceProcAddr");
extern PFN_vkVoidFunction wine_nx_vkGetInstanceProcAddr( VkInstance instance, const char *name ) __asm__("vkGetInstanceProcAddr");
extern VkResult wine_nx_vk_allocate_shared_memory( VkDevice device,
                                                    const VkMemoryAllocateInfo *allocate_info,
                                                    uint32_t nvmap_id, VkDeviceMemory *memory_out );
extern BOOL wine_nx_vk_export_memory( VkDeviceMemory memory, uint32_t *nvmap_id,
                                      void **reference );

static uint64_t nx_frame_interval;
static VkPresentModeKHR nx_present_mode = VK_PRESENT_MODE_FIFO_KHR;

void wine_nx_graphics_configure( int frame_limit, int vsync )
{
    nx_frame_interval = frame_limit > 0 ? 1000000000ull / frame_limit : 0;
    nx_present_mode = vsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
}

void wine_nx_upscaling_configure( int mode, float sharpness )
{
    nx_upscaling_mode = mode;
    nx_upscaling_sharpness = sharpness;
}

static void nx_pace_present( uint64_t *previous )
{
    struct timespec now, delay;
    uint64_t time, remaining;

    if (!nx_frame_interval || clock_gettime( CLOCK_MONOTONIC, &now )) return;
    time = (uint64_t)now.tv_sec * 1000000000ull + now.tv_nsec;
    if (*previous && time < *previous + nx_frame_interval)
    {
        remaining = *previous + nx_frame_interval - time;
        delay.tv_sec = remaining / 1000000000ull;
        delay.tv_nsec = remaining % 1000000000ull;
        while (nanosleep( &delay, &delay ) && errno == EINTR) {}
        if (clock_gettime( CLOCK_MONOTONIC, &now )) return;
        time = (uint64_t)now.tv_sec * 1000000000ull + now.tv_nsec;
    }
    *previous = time;
}
#endif

#ifdef __SWITCH__
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

/* [NXVK] lines in the runtime's log: this build keeps no TRACE output. */
extern void wine_nx_runtime_trace( const char *msg ) __attribute__((weak));

static void nx_vk_trace( const char *format, ... )
{
    char buffer[320];
    va_list args;

    if (!&wine_nx_runtime_trace) return;
    va_start( args, format );
    vsnprintf( buffer, sizeof(buffer), format, args );
    va_end( args );
    wine_nx_runtime_trace( buffer );
}

extern void horizon_get_address_space_limits( void **start, void **limit );

static BOOL nx_uses_32bit_address_space(void)
{
    static int is_32bit = -1;
    void *start, *limit;

    if (is_32bit >= 0) return is_32bit;
    horizon_get_address_space_limits( &start, &limit );
    is_32bit = (ULONG_PTR)limit <= 0x100000000;
    return is_32bit;
}
#endif

WINE_DECLARE_DEBUG_CHANNEL(fps);

static const struct vulkan_driver_funcs *driver_funcs;
static int fshack_enabled = -1;

static const UINT EXTERNAL_MEMORY_WIN32_BITS = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT |
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT |
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT |
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT |
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT |
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
static const UINT EXTERNAL_SEMAPHORE_WIN32_BITS = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT |
                                                  VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT |
                                                  VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
static const UINT EXTERNAL_FENCE_WIN32_BITS = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT |
                                              VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT;

#define ROUND_SIZE(size, mask) ((((SIZE_T)(size) + (mask)) & ~(SIZE_T)(mask)))

static BOOL use_external_memory(void)
{
#ifdef __SWITCH__
    return TRUE;
#else
    return zero_bits != 0;
#endif
}

struct mempool
{
    struct mempool *next;
    size_t mem_used;
    char mem[2048];
};

static void mem_free( struct mempool *pool )
{
    struct mempool *iter, *next = pool->next;
    while ((iter = next)) { next = iter->next; free( iter ); }
    pool->mem_used = 0;
    pool->next = NULL;
}

static void *mem_alloc( struct mempool *pool, size_t size )
{
    struct mempool *next = pool;
    if (pool->mem_used + size > sizeof(pool->mem)) next = pool->next;
    if (next && next->mem_used <= sizeof(next->mem) && next->mem_used + size <= sizeof(next->mem))
    {
        void *ret = next->mem + next->mem_used;
        next->mem_used += ROUND_SIZE( size, sizeof(UINT64) - 1 );
        return ret;
    }
    if (!(next = malloc( max( sizeof(*next), offsetof(struct mempool, mem[size]) ) ))) return NULL;
    next->next = pool->next;
    next->mem_used = size;
    pool->next = next;
    return next->mem;
}

struct instance
{
    struct vulkan_instance obj;
    BOOL enable_win32_surface;

    struct list utils_messengers;
    struct list report_callbacks;

    struct rb_tree objects;
    pthread_rwlock_t objects_lock;
};

static struct instance *instance_from_handle( VkInstance handle )
{
    struct vulkan_instance *object = vulkan_instance_from_handle( handle );
    return CONTAINING_RECORD( object, struct instance, obj );
}

struct device_memory
{
    struct vulkan_device_memory obj;
    VkDeviceSize size;
    void *vm_map;

    D3DKMT_HANDLE local;
    D3DKMT_HANDLE global;
    HANDLE shared;

    D3DKMT_HANDLE sync;
    D3DKMT_HANDLE mutex;
    VkSemaphore semaphore;
    UINT64 semaphore_value;
};

static inline struct device_memory *device_memory_from_handle( VkDeviceMemory handle )
{
    struct vulkan_device_memory *obj = vulkan_device_memory_from_handle( handle );
    return CONTAINING_RECORD( obj, struct device_memory, obj );
}

struct surface
{
    struct vulkan_surface obj;
    struct client_surface *client;
    HWND hwnd;
};

static struct surface *surface_from_handle( VkSurfaceKHR handle )
{
    struct vulkan_surface *obj = vulkan_surface_from_handle( handle );
    return CONTAINING_RECORD( obj, struct surface, obj );
}

/* Return whether integer scaling is on */
static BOOL fs_hack_is_integer(void)
{
    static int is_int = -1;
    if (nx_upscaling_mode == 2) return TRUE;
    if (is_int < 0)
    {
        const char *e = getenv( "WINE_FULLSCREEN_INTEGER_SCALING" );
        is_int = e && strcmp( e, "0" );
        TRACE( "is_integer_scaling: %s\n", is_int ? "TRUE" : "FALSE" );
    }
    return is_int;
}

struct fs_hack_image
{
    uint32_t cmd_queue_idx;
    VkCommandBuffer cmd;
    VkImage swapchain_image;
    VkImage user_image;
    VkSemaphore blit_finished;
    VkImageView user_view, blit_view;
    VkDescriptorSet descriptor_set;
    /* FSR: the game's image read as UNORM, and what EASU leaves for RCAS */
    VkImageView gamma_view, mid_view;
    VkImage mid_image;
};

static const char *debugstr_vkextent2d( const VkExtent2D *ext )
{
    if (!ext) return "(null)";
    return wine_dbg_sprintf( "(%d,%d)", (int)ext->width, (int)ext->height );
}

struct swapchain
{
    struct vulkan_swapchain obj;
#ifdef __SWITCH__
    /* The floating keyboard's copy into the host images (nx_osk_present). */
    VkFormat nx_format;
    VkExtent2D nx_extent;
    BOOL nx_osk_usable;
    struct nx_osk_overlay *nx_osk;
#endif
#ifdef WINE_NX_MESA_SWITCH
    uint64_t nx_last_present;
    VkPresentModeKHR nx_present_mode;
#endif
#ifdef WINE_NX_LSFG
    struct wine_nx_lsfg *lsfg;
    unsigned int lsfg_acquires;
#endif
    struct surface *surface;
    VkExtent2D extents;

    /* fs hack data below */
    UINT fshack_dpi;
    VkExtent2D host_extents;
    VkCommandPool *cmd_pools; /* VkCommandPool[device->queue_count] */
    VkDeviceMemory user_image_memory;
    uint32_t n_images;
    struct fs_hack_image *fs_hack_images; /* struct fs_hack_image[n_images] */
    VkSampler sampler;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    BOOL fsr;                /* pipeline is EASU, and rcas_pipeline follows it */
    VkPipeline rcas_pipeline;
};

#ifdef __SWITCH__
static void nx_osk_free( struct vulkan_device *device, struct nx_osk_overlay *osk );
#endif

static struct swapchain *swapchain_from_handle( VkSwapchainKHR handle )
{
    struct vulkan_swapchain *obj = vulkan_swapchain_from_handle( handle );
    return CONTAINING_RECORD( obj, struct swapchain, obj );
}

struct semaphore
{
    struct vulkan_semaphore obj;
    D3DKMT_HANDLE local;
    D3DKMT_HANDLE global;
    HANDLE shared;
};

static struct semaphore *semaphore_from_handle( VkSemaphore handle )
{
    struct vulkan_semaphore *obj = vulkan_semaphore_from_handle( handle );
    return CONTAINING_RECORD( obj, struct semaphore, obj );
}

struct fence
{
    struct vulkan_fence obj;
    D3DKMT_HANDLE local;
    D3DKMT_HANDLE global;
    HANDLE shared;
};

static struct fence *fence_from_handle( VkFence handle )
{
    struct vulkan_fence *obj = vulkan_fence_from_handle( handle );
    return CONTAINING_RECORD( obj, struct fence, obj );
}

#ifdef __SWITCH__
static LONG nx_host_import_logs;

/* The program's Vulkan memory, logged every 256 allocations and on failures:
 * DXVK sub-allocates, so an allocator that churns or fails shows nowhere else. */
static LONG nx_memory_allocs, nx_memory_frees, nx_memory_imports, nx_memory_failure_logs;
static LONGLONG nx_memory_alloc_bytes, nx_memory_free_bytes;

static void nx_memory_note_alloc( VkDeviceSize size, BOOL imported )
{
    const LONG allocs = __atomic_add_fetch( &nx_memory_allocs, 1, __ATOMIC_RELAXED );
    const LONGLONG bytes = __atomic_add_fetch( &nx_memory_alloc_bytes, (LONGLONG)size, __ATOMIC_RELAXED );
    LONGLONG freed;

    if (imported) __atomic_add_fetch( &nx_memory_imports, 1, __ATOMIC_RELAXED );
    if (allocs % 256) return;
    freed = __atomic_load_n( &nx_memory_free_bytes, __ATOMIC_RELAXED );
    nx_vk_trace( "[NXVK] memory: %d allocations of %lld MB, %d frees of %lld MB, %lld MB live, "
                 "%d with imported host mappings", (int)allocs, bytes >> 20,
                 (int)__atomic_load_n( &nx_memory_frees, __ATOMIC_RELAXED ), freed >> 20,
                 (bytes - freed) >> 20, (int)__atomic_load_n( &nx_memory_imports, __ATOMIC_RELAXED ) );
}

static BOOL nx_memory_log_failure(void)
{
    return __atomic_add_fetch( &nx_memory_failure_logs, 1, __ATOMIC_RELAXED ) <= 32;
}

static BOOL nx_driver_maps_preferred(void)
{
    static LONG logged;
    BOOL native = !zero_bits || nx_uses_32bit_address_space();

    if (__atomic_add_fetch( &logged, 1, __ATOMIC_RELAXED ) == 1)
        nx_vk_trace( "[NXVK] Vulkan memory uses %s",
                     native ? "native driver mappings" : "low host imports for the Win32 guest" );
    return native;
}

static BOOL nx_can_import_fragmented_memory( const VkMemoryAllocateInfo *info )
{
    const VkBaseInStructure *next;

    if (zero_bits || nx_uses_32bit_address_space()) return FALSE;
    for (next = info->pNext; next; next = next->pNext)
    {
        if (next->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO &&
            (((const VkMemoryAllocateFlagsInfo *)next)->flags &
             (VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT | VK_MEMORY_ALLOCATE_ZERO_INITIALIZE_BIT_EXT)))
            return FALSE;
        if (next->sType == VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO &&
            ((const VkMemoryOpaqueCaptureAddressAllocateInfo *)next)->opaqueCaptureAddress)
            return FALSE;
        if (next->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO &&
            (((const VkMemoryDedicatedAllocateInfo *)next)->image || ((const VkMemoryDedicatedAllocateInfo *)next)->buffer))
            return FALSE;
        if (next->sType == VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_MEMORY_ALLOCATE_INFO_NV ||
            next->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_TENSOR_ARM)
            return FALSE;
    }
    return TRUE;
}
#endif

static VkResult import_external_host_memory( struct vulkan_device *device, VkMemoryAllocateInfo *alloc_info,
                                             uint32_t mem_flags, void *mapping,
                                             VkImportMemoryHostPointerInfoEXT *import_info )
{
    struct vulkan_physical_device *physical_device = device->physical_device;
    VkMemoryHostPointerPropertiesEXT props =
    {
        .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
    };
    uint32_t i, align = physical_device->external_memory_align - 1;
    VkResult res;

    if ((res = device->p_vkGetMemoryHostPointerPropertiesEXT( device->host.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                              mapping, &props )))
        return res;

    if (!(props.memoryTypeBits & (1u << alloc_info->memoryTypeIndex)))
    {
        /* If requested memory type is not allowed to use external memory, try to find a supported compatible type. */
        uint32_t mask = mem_flags & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        for (i = 0; i < physical_device->memory_properties.memoryTypeCount; i++)
        {
            if (!(props.memoryTypeBits & (1u << i))) continue;
            if ((physical_device->memory_properties.memoryTypes[i].propertyFlags & mask) != mask) continue;

            TRACE( "Memory type not compatible with host memory, using %u instead\n", i );
            alloc_info->memoryTypeIndex = i;
            break;
        }
        if (i == physical_device->memory_properties.memoryTypeCount)
        {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }

    if (props.memoryTypeBits & (1u << alloc_info->memoryTypeIndex))
    {
        import_info->sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
        import_info->handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        import_info->pHostPointer = mapping;
        import_info->pNext = alloc_info->pNext;
        alloc_info->pNext = import_info;
        alloc_info->allocationSize = (alloc_info->allocationSize + align) & ~align;
#ifdef __SWITCH__
        if (__atomic_add_fetch( &nx_host_import_logs, 1, __ATOMIC_RELAXED ) <= 8)
            nx_vk_trace( "[NXVK] %llu bytes of memory type %u imported from host mapping %p",
                         (unsigned long long)alloc_info->allocationSize, alloc_info->memoryTypeIndex, mapping );
#endif
    }

    return VK_SUCCESS;
}

static VkResult allocate_external_host_memory( struct vulkan_device *device, VkMemoryAllocateInfo *alloc_info, uint32_t mem_flags,
                                               VkImportMemoryHostPointerInfoEXT *import_info )
{
    SIZE_T alloc_size = alloc_info->allocationSize;
    static int once;
    void *mapping = NULL;
    NTSTATUS status;
    VkResult res;

    if (!once++) FIXME( "Using VK_EXT_external_memory_host\n" );
    if ((status = NtAllocateVirtualMemory( GetCurrentProcess(), &mapping, zero_bits, &alloc_size, MEM_COMMIT, PAGE_READWRITE )))
    {
#ifdef __SWITCH__
        if (nx_memory_log_failure())
            nx_vk_trace( "[NXVK] no host mapping for %llu bytes of memory type %u: status %#x",
                         (unsigned long long)alloc_info->allocationSize, alloc_info->memoryTypeIndex, (unsigned int)status );
#endif
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    res = import_external_host_memory( device, alloc_info, mem_flags, mapping, import_info );
    if (res != VK_SUCCESS)
    {
#ifdef __SWITCH__
        if (nx_memory_log_failure())
            nx_vk_trace( "[NXVK] host import at %p for %llu bytes failed: %d", mapping,
                         (unsigned long long)alloc_info->allocationSize, res );
#endif
        alloc_size = 0;
        NtFreeVirtualMemory( GetCurrentProcess(), &mapping, &alloc_size, MEM_RELEASE );
        if (res == VK_ERROR_FEATURE_NOT_PRESENT) return VK_SUCCESS;
    }
    return res;
}

static VkExternalMemoryHandleTypeFlagBits get_host_external_memory_type(void)
{
    struct vulkan_device_extensions extensions = {.has_VK_KHR_external_memory_win32 = 1};
    driver_funcs->p_map_device_extensions( &extensions );
    if (extensions.has_VK_KHR_external_memory_fd) return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    if (extensions.has_VK_EXT_external_memory_dma_buf) return VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    if (extensions.has_VK_EXT_external_memory_host) return VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    return 0;
}

static void map_external_memory_properties( VkExternalMemoryProperties *properties,
                                            VkExternalMemoryHandleTypeFlagBits handle_type )
{
    properties->compatibleHandleTypes = handle_type;
    properties->exportFromImportedHandleTypes = handle_type;
#ifdef __SWITCH__
    if ((handle_type & EXTERNAL_MEMORY_WIN32_BITS) &&
        get_host_external_memory_type() == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT)
        properties->externalMemoryFeatures |= VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT |
                                               VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT;
#endif
}

static VkExternalSemaphoreHandleTypeFlagBits get_host_external_semaphore_type(void)
{
    struct vulkan_device_extensions extensions = {.has_VK_KHR_external_semaphore_win32 = 1};
    driver_funcs->p_map_device_extensions( &extensions );
    if (extensions.has_VK_KHR_external_semaphore_fd) return VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    return 0;
}

static VkExternalFenceHandleTypeFlagBits get_host_external_fence_type(void)
{
    struct vulkan_device_extensions extensions = {.has_VK_KHR_external_fence_win32 = 1};
    driver_funcs->p_map_device_extensions( &extensions );
    if (extensions.has_VK_KHR_external_fence_fd) return VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
    return 0;
}

static void init_shared_resource_path( const WCHAR *name, UNICODE_STRING *str )
{
    UINT len = wcslen( name );
    char buffer[MAX_PATH];

    snprintf( buffer, ARRAY_SIZE(buffer), "\\Sessions\\%u\\BaseNamedObjects\\",
              NtCurrentTeb()->Peb->SessionId );
    str->MaximumLength = asciiz_to_unicode( str->Buffer, buffer );
    str->Length = str->MaximumLength - sizeof(WCHAR);

    memcpy( str->Buffer + str->Length / sizeof(WCHAR), name, (len + 1) * sizeof(WCHAR) );
    str->MaximumLength += len * sizeof(WCHAR);
    str->Length += len * sizeof(WCHAR);
}

static HANDLE create_shared_resource_handle( D3DKMT_HANDLE local, const VkExportMemoryWin32HandleInfoKHR *info )
{
    SECURITY_DESCRIPTOR *security = info->pAttributes ? info->pAttributes->lpSecurityDescriptor : NULL;
    WCHAR bufferW[MAX_PATH * 2];
    UNICODE_STRING name = {.Buffer = bufferW};
    OBJECT_ATTRIBUTES attr;
    NTSTATUS status;
    HANDLE shared;

    if (info->name) init_shared_resource_path( info->name, &name );
    InitializeObjectAttributes( &attr, info->name ? &name : NULL, OBJ_CASE_INSENSITIVE, NULL, security );

    if (!(status = NtGdiDdDDIShareObjects( 1, &local, &attr, info->dwAccess, &shared ))) return shared;
    WARN( "Failed to share resource %#x, status %#x\n", local, status );
    return NULL;
}

HANDLE open_shared_resource_from_name( const WCHAR *name )
{
    D3DKMT_OPENNTHANDLEFROMNAME open_name = {0};
    WCHAR bufferW[MAX_PATH * 2];
    UNICODE_STRING name_str = {.Buffer = bufferW};
    OBJECT_ATTRIBUTES attr;
    NTSTATUS status;

    init_shared_resource_path( name, &name_str );
    InitializeObjectAttributes( &attr, &name_str, OBJ_OPENIF, NULL, NULL );

    open_name.dwDesiredAccess = GENERIC_ALL;
    open_name.pObjAttrib = &attr;
    status = NtGdiDdDDIOpenNtHandleFromName( &open_name );
    if (status) WARN( "Failed to open %s, status %#x\n", debugstr_w( name ), status );
    return open_name.hNtHandle;
}

static const void *find_next_struct( const VkBaseInStructure *header, VkStructureType type )
{
    for (; header; header = header->pNext) if (header->sType == type) return header;
    return NULL;
}

static const void *pop_next_struct( VkBaseOutStructure **next, VkStructureType type )
{
    VkBaseOutStructure *ptr;
    while (*next && (*next)->sType != type) next = &(*next)->pNext;
    if ((ptr = *next)) *next = ptr->pNext;
    return ptr;
}

static int vulkan_object_compare( const void *key, const struct rb_entry *entry )
{
    struct vulkan_object *object = RB_ENTRY_VALUE( entry, struct vulkan_object, entry );
    const uint64_t *host_handle = key;
    if (*host_handle < object->host_handle) return -1;
    if (*host_handle > object->host_handle) return 1;
    return 0;
}

static uint64_t vulkan_instance_client_handle_from_host( struct vulkan_instance *instance, uint64_t host_handle )
{
    struct instance *impl = CONTAINING_RECORD( instance, struct instance, obj );
    struct rb_entry *entry;
    uint64_t result = 0;

    pthread_rwlock_rdlock( &impl->objects_lock );
    if ((entry = rb_get( &impl->objects, &host_handle )))
    {
        struct vulkan_object *object = RB_ENTRY_VALUE( entry, struct vulkan_object, entry );
        result = object->client_handle;
    }
    pthread_rwlock_unlock( &impl->objects_lock );
    return result;
}

static void vulkan_instance_insert_object( struct vulkan_instance *instance, struct vulkan_object *obj )
{
    struct instance *impl = CONTAINING_RECORD( instance, struct instance, obj );
    if (impl->objects.compare)
    {
        pthread_rwlock_wrlock( &impl->objects_lock );
        rb_put( &impl->objects, &obj->host_handle, &obj->entry );
        pthread_rwlock_unlock( &impl->objects_lock );
    }
}

static void vulkan_instance_remove_object( struct vulkan_instance *instance, struct vulkan_object *obj )
{
    struct instance *impl = CONTAINING_RECORD( instance, struct instance, obj );
    if (impl->objects.compare)
    {
        pthread_rwlock_wrlock( &impl->objects_lock );
        rb_remove( &impl->objects, &obj->entry );
        pthread_rwlock_unlock( &impl->objects_lock );
    }
}

static void free_debug_utils_messengers( struct list *messengers )
{
    struct vulkan_debug_utils_messenger *messenger, *next;

    LIST_FOR_EACH_ENTRY_SAFE( messenger, next, messengers, struct vulkan_debug_utils_messenger, entry )
    {
        list_remove( &messenger->entry );
        free( messenger );
    }
}

static void free_debug_report_callbacks( struct list *callbacks )
{
    struct vulkan_debug_report_callback *callback, *next;

    LIST_FOR_EACH_ENTRY_SAFE( callback, next, callbacks, struct vulkan_debug_report_callback, entry )
    {
        list_remove( &callback->entry );
        free( callback );
    }
}

static VkResult convert_instance_create_info( struct mempool *pool, VkInstanceCreateInfo *info, struct instance *instance )
{
    const VkBaseInStructure *header = (const VkBaseInStructure *)info;
    const VkDebugReportCallbackCreateInfoEXT *debug_report_callback;
    const char **extensions;
    uint32_t count = 0;

    while ((header = find_next_struct( header->pNext, VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT )))
    {
        const VkDebugUtilsMessengerCreateInfoEXT *debug_utils_messenger = (const VkDebugUtilsMessengerCreateInfoEXT *)header;
        struct vulkan_debug_utils_messenger *messenger = debug_utils_messenger->pUserData;

        list_remove( &messenger->entry );
        list_add_tail( &instance->utils_messengers, &messenger->entry );
        messenger->instance = &instance->obj;
    }

    if ((debug_report_callback = find_next_struct( info->pNext, VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT )))
    {
        struct vulkan_debug_report_callback *callback = debug_report_callback->pUserData;

        list_remove( &callback->entry );
        list_add_tail( &instance->report_callbacks, &callback->entry );
        callback->instance = &instance->obj;
    }

    if (info->enabledLayerCount)
    {
        FIXME( "Loading explicit layers is not supported!\n" );
        return VK_ERROR_LAYER_NOT_PRESENT;
    }

    driver_funcs->p_map_instance_extensions( &instance->obj.extensions );
    instance->obj.extensions.has_VK_KHR_win32_surface = 0;

    if (instance->obj.extensions.has_VK_EXT_debug_utils || instance->obj.extensions.has_VK_EXT_debug_report)
    {
        rb_init( &instance->objects, vulkan_object_compare );
        pthread_rwlock_init( &instance->objects_lock, NULL );
    }

    if (instance->obj.extensions.has_VK_KHR_win32_surface && vulkan_funcs.host_extensions.has_VK_EXT_surface_maintenance1)
        instance->obj.extensions.has_VK_EXT_surface_maintenance1 = 1;
    if (vulkan_funcs.host_extensions.has_VK_KHR_get_physical_device_properties2)
        instance->obj.extensions.has_VK_KHR_get_physical_device_properties2 = 1;
    if (use_external_memory())
        instance->obj.extensions.has_VK_KHR_external_memory_capabilities = 1;

    /* VK_KHR_win32_keyed_mutex only requires external memory extensions, but we will use
     * external semaphore fds to implement it, so we enable the instance extensions too */
    instance->obj.extensions.has_VK_KHR_external_semaphore_capabilities = 1;

    if (!(extensions = mem_alloc( pool, sizeof(instance->obj.extensions) * 8 * sizeof(*extensions) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
#define USE_VK_EXT(x) if (instance->obj.extensions.has_ ## x) extensions[count++] = #x;
    ALL_VK_INSTANCE_EXTS
#undef USE_VK_EXT

    TRACE( "Enabling %u host instance extensions\n", count );
    for (const char **extension = extensions, **end = extension + count; extension < end; extension++)
        TRACE( "  - %s\n", debugstr_a(*extension) );

    info->ppEnabledExtensionNames = extensions;
    info->enabledExtensionCount = count;
    return VK_SUCCESS;
}

static VkResult init_physical_device( struct vulkan_physical_device *physical_device, VkPhysicalDevice host_physical_device,
                                      VkPhysicalDevice client_physical_device, struct vulkan_instance *instance )
{
    struct vulkan_device_extensions extensions = {0};
    VkExtensionProperties *properties;
    uint32_t count;
    VkResult res;

    vulkan_object_init_ptr( &physical_device->obj, (UINT_PTR)host_physical_device, &client_physical_device->obj );
    physical_device->instance = instance;

    instance->p_vkGetPhysicalDeviceMemoryProperties( host_physical_device, &physical_device->memory_properties );

    if ((res = instance->p_vkEnumerateDeviceExtensionProperties( host_physical_device, NULL, &count, NULL ))) return res;
    if (!(properties = calloc( count, sizeof(*properties) )))
    {
#ifdef __SWITCH__
        nx_vk_trace( "[NXVK] no memory for %u host device extensions; the client sees none", count );
#endif
        return res;
    }
    if ((res = instance->p_vkEnumerateDeviceExtensionProperties( host_physical_device, NULL, &count, properties ))) goto done;

    TRACE( "Host physical device extensions:\n" );
    for (uint32_t i = 0; i < count; i++)
    {
        const char *extension = properties[i].extensionName;
#define USE_VK_EXT(x)                           \
        if (!strcmp( extension, #x ))           \
        {                                       \
            extensions.has_ ## x = 1;           \
            TRACE( "  - %s\n", extension );     \
        } else
        ALL_VK_DEVICE_EXTS
#undef USE_VK_EXT
        WARN( "Extension %s is not supported.\n", debugstr_a(extension) );
    }
    physical_device->extensions = extensions;

    if (zero_bits && physical_device->extensions.has_VK_EXT_map_memory_placed && physical_device->extensions.has_VK_KHR_map_memory2)
    {
        VkPhysicalDeviceMapMemoryPlacedFeaturesEXT map_placed_feature = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &map_placed_feature};

        instance->p_vkGetPhysicalDeviceFeatures2KHR( host_physical_device, &features );
        if (map_placed_feature.memoryMapPlaced && map_placed_feature.memoryUnmapReserve)
        {
            VkPhysicalDeviceMapMemoryPlacedPropertiesEXT map_placed_props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT};
            VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext = &map_placed_props};

            instance->p_vkGetPhysicalDeviceProperties2( host_physical_device, &props );
            physical_device->map_placed_align = map_placed_props.minPlacedMemoryMapAlignment;
            TRACE( "Using placed map with alignment %u\n", physical_device->map_placed_align );
        }
    }

    if (use_external_memory() && physical_device->extensions.has_VK_EXT_external_memory_host && !physical_device->map_placed_align)
    {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_mem_props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &host_mem_props};

        instance->p_vkGetPhysicalDeviceProperties2KHR( host_physical_device, &props );
        physical_device->external_memory_align = host_mem_props.minImportedHostPointerAlignment;
        if (!physical_device->external_memory_align) WARN( "Not using VK_EXT_external_memory_host for memory mapping\n" );
        else TRACE( "Using VK_EXT_external_memory_host for memory mapping with alignment: %u\n", physical_device->external_memory_align );
    }

    driver_funcs->p_map_device_extensions( &extensions );
    if (extensions.has_VK_KHR_external_memory_win32 && zero_bits && !physical_device->map_placed_align &&
        get_host_external_memory_type() != VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT)
    {
        WARN( "Cannot export WOW64 memory without VK_EXT_map_memory_placed\n" );
        extensions.has_VK_KHR_external_memory_win32 = 0;
    }
#ifdef __SWITCH__
    extensions.has_VK_KHR_win32_keyed_mutex = 0;
#else
    extensions.has_VK_KHR_win32_keyed_mutex = extensions.has_VK_KHR_timeline_semaphore &&
                                              extensions.has_VK_KHR_external_semaphore_fd;
#endif

    /* filter out unsupported client device extensions */
#define USE_VK_EXT(x) client_physical_device->extensions.has_ ## x = extensions.has_ ## x;
    ALL_VK_CLIENT_DEVICE_EXTS
#undef USE_VK_EXT

#ifdef __SWITCH__
    {
        struct vulkan_device_extensions bit = {0};
        const unsigned char *raw = (const unsigned char *)&bit;
        unsigned int byte = 0;

        bit.has_VK_KHR_swapchain = 1;
        while (byte < sizeof(bit) && !raw[byte]) byte++;
        nx_vk_trace( "[NXVK] physical device client %p: %u host extensions, VK_KHR_swapchain host %u mapped %u client %u; "
                     "its bit at extensions+%u mask %#x, extensions at client+%u, %u bytes; external_memory_align %u",
                     client_physical_device, count, physical_device->extensions.has_VK_KHR_swapchain,
                     extensions.has_VK_KHR_swapchain, client_physical_device->extensions.has_VK_KHR_swapchain,
                     byte, byte < sizeof(bit) ? raw[byte] : 0, (unsigned)offsetof(struct VkPhysicalDevice_T, extensions),
                     (unsigned)sizeof(bit), physical_device->external_memory_align );
    }
#endif

done:
    free( properties );
    return res;
}

/* Helper function which stores wrapped physical devices in the instance object. */
static VkResult init_physical_devices( struct vulkan_instance *instance, struct vulkan_physical_device *physical_devices )
{
    VkInstance client_instance = instance->client.instance;
    VkPhysicalDevice *host_physical_devices;
    uint32_t physical_device_count;
    unsigned int i;
    VkResult res;

    if ((res = instance->p_vkEnumeratePhysicalDevices( instance->host.instance, &physical_device_count, NULL )))
    {
        ERR( "Failed to enumerate physical devices, res %d\n", res );
        return res;
    }
    if (!physical_device_count) return res;

    if (physical_device_count > client_instance->physical_device_count)
    {
        client_instance->physical_device_count = physical_device_count;
        return VK_ERROR_OUT_OF_POOL_MEMORY;
    }
    client_instance->physical_device_count = physical_device_count;

    if (!(host_physical_devices = calloc( physical_device_count, sizeof(*host_physical_devices) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if ((res = instance->p_vkEnumeratePhysicalDevices( instance->host.instance, &physical_device_count, host_physical_devices ))) goto failed;

    /* Wrap each host physical device handle into a dispatchable object for the ICD loader. */
    for (i = 0; i < physical_device_count; i++)
    {
        VkPhysicalDevice client_physical_device = &client_instance->physical_device[i];
        struct vulkan_physical_device *physical_device = physical_devices + i;
        if ((res = init_physical_device( physical_device, host_physical_devices[i], client_physical_device, instance ))) goto failed;
    }
    instance->physical_device_count = physical_device_count;
    instance->physical_devices = physical_devices;

failed:
    free( host_physical_devices );
    return res;
}

static BOOL add_instance_extension( const char *extension, size_t len, struct vulkan_instance_extensions *extensions )
{
#define USE_VK_EXT(x) \
    if (len == sizeof(#x) - 1 && !strncmp( #x, extension, len ))    \
    {                                                               \
        if (!extensions->has_ ## x) TRACE( "Adding %s\n", #x );     \
        return extensions->has_ ## x = 1;                           \
    }
    ALL_VK_INSTANCE_EXTS
#undef USE_VK_EXT
    WARN( "Extension %s is not supported.\n", debugstr_a(extension) );
    return FALSE;
}

static VkResult win32u_vkCreateInstance( const VkInstanceCreateInfo *client_create_info, const VkAllocationCallbacks *allocator,
                                         VkInstance *client_instance_ptr )
{
    VkInstanceCreateInfo *create_info = (VkInstanceCreateInfo *)client_create_info; /* cast away const, chain has been copied in the thunks */
    VkInstance host_instance = VK_NULL_HANDLE, client_instance = *client_instance_ptr;
    struct vulkan_physical_device *physical_devices;
    struct mempool pool = {0};
    struct instance *instance;
    unsigned int i;
    VkResult res;

    if (!(instance = calloc( 1, sizeof(*instance) + sizeof(*physical_devices) * client_instance->physical_device_count) ))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    physical_devices = (struct vulkan_physical_device *)(instance + 1);
    instance->obj.extensions = client_instance->extensions;
    list_init( &instance->utils_messengers );
    list_init( &instance->report_callbacks );

    if ((res = convert_instance_create_info( &pool, create_info, instance ))) goto failed;
    if ((res = p_vkCreateInstance( create_info, NULL /* allocator */, &host_instance ))) goto failed;

    vulkan_object_init_ptr( &instance->obj.obj, (UINT_PTR)host_instance, &client_instance->obj );
    instance->obj.p_insert_object = vulkan_instance_insert_object;
    instance->obj.p_remove_object = vulkan_instance_remove_object;
    instance->obj.p_client_handle_from_host = vulkan_instance_client_handle_from_host;

#define USE_VK_FUNC( name )                                                                          \
    instance->obj.p_##name = (void *)p_vkGetInstanceProcAddr( instance->obj.host.instance, #name );  \
    if (!instance->obj.p_##name) TRACE( "Instance proc %s not found.\n", #name );
    ALL_VK_INSTANCE_FUNCS
#undef USE_VK_FUNC

    /* Cache physical devices for vkEnumeratePhysicalDevices within the instance as each vkPhysicalDevice is a dispatchable
     * object, which means we need to wrap the host physical devices and present those to the application.
     */
    if ((res = init_physical_devices( &instance->obj, physical_devices ))) goto failed;

    TRACE( "Created instance %p, host_instance %p.\n", instance, instance->obj.host.instance );
    for (i = 0; i < instance->obj.physical_device_count; i++)
    {
        struct vulkan_physical_device *physical_device = &instance->obj.physical_devices[i];
        vulkan_instance_insert_object( &instance->obj, &physical_device->obj );
    }
    vulkan_instance_insert_object( &instance->obj, &instance->obj.obj );

failed:
    if (res)
    {
        WARN( "Failed to create vulkan instance, res %d\n", res );
        if (host_instance) instance->obj.p_vkDestroyInstance( host_instance, NULL /* allocator */ );
        free_debug_utils_messengers( &instance->utils_messengers );
        free_debug_report_callbacks( &instance->report_callbacks );
        free( instance );
    }
    mem_free( &pool );
    return res;
}

static void win32u_vkDestroyInstance( VkInstance client_instance, const VkAllocationCallbacks *allocator )
{
    struct instance *instance = instance_from_handle( client_instance );

    if (!instance) return;

    instance->obj.p_vkDestroyInstance( instance->obj.host.instance, NULL /* allocator */ );
    for (int i = 0; i < instance->obj.physical_device_count; i++)
        vulkan_instance_remove_object( &instance->obj, &instance->obj.physical_devices[i].obj );
    vulkan_instance_remove_object( &instance->obj, &instance->obj.obj );

    if (instance->objects.compare) pthread_rwlock_destroy( &instance->objects_lock );
    free_debug_utils_messengers( &instance->utils_messengers );
    free_debug_report_callbacks( &instance->report_callbacks );
    free( instance );
}

static VkResult convert_device_create_info( struct vulkan_physical_device *physical_device, VkDeviceCreateInfo *info,
                                            struct mempool *pool, struct vulkan_device *device )
{
    struct vulkan_instance *instance = physical_device->instance;
    const char **extensions;
    uint32_t count = 0;

    /* Should be filtered out by loader as ICDs don't support layers. */
    info->enabledLayerCount = 0;
    info->ppEnabledLayerNames = NULL;

    if (device->extensions.has_VK_KHR_win32_keyed_mutex)
    {
        device->extensions.has_VK_KHR_timeline_semaphore = 1;
        device->extensions.has_VK_KHR_external_semaphore_fd = 1;
        device->extensions.has_VK_KHR_external_semaphore = 1;
    }

    driver_funcs->p_map_device_extensions( &device->extensions );
    device->extensions.has_VK_KHR_win32_keyed_mutex = 0;
    device->extensions.has_VK_KHR_external_memory_win32 = 0;
    device->extensions.has_VK_KHR_external_fence_win32 = 0;
    device->extensions.has_VK_KHR_external_semaphore_win32 = 0;

    if (device->extensions.has_VK_EXT_external_memory_dma_buf)
        device->extensions.has_VK_KHR_external_memory_fd = 1;

    if (physical_device->map_placed_align)
    {
        VkPhysicalDeviceMapMemoryPlacedFeaturesEXT *map_placed_features;

        if (!(map_placed_features = mem_alloc( pool, sizeof(*map_placed_features) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
        map_placed_features->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT;
        map_placed_features->pNext = (void *)info->pNext;
        map_placed_features->memoryMapPlaced = VK_TRUE;
        map_placed_features->memoryMapRangePlaced = VK_FALSE;
        map_placed_features->memoryUnmapReserve = VK_TRUE;
        info->pNext = map_placed_features;

        device->extensions.has_VK_EXT_map_memory_placed = 1;
        device->extensions.has_VK_KHR_map_memory2 = 1;
    }
    else if (physical_device->external_memory_align)
    {
        device->extensions.has_VK_KHR_external_memory = 1;
        device->extensions.has_VK_EXT_external_memory_host = 1;
    }

    /* win32u uses VkSwapchainPresentScalingCreateInfoEXT if available. */
    if (device->extensions.has_VK_KHR_swapchain && instance->extensions.has_VK_EXT_surface_maintenance1 &&
        physical_device->extensions.has_VK_EXT_swapchain_maintenance1)
        device->extensions.has_VK_EXT_swapchain_maintenance1 = 1;

    if (!(extensions = mem_alloc( pool, sizeof(device->extensions) * 8 * sizeof(*extensions) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
#define USE_VK_EXT(x) if (device->extensions.has_ ## x) extensions[count++] = #x;
    ALL_VK_DEVICE_EXTS
#undef USE_VK_EXT

    TRACE( "Enabling %u host device extensions\n", count );
    for (const char **extension = extensions, **end = extension + count; extension < end; extension++)
        TRACE( "  - %s\n", debugstr_a(*extension) );

    info->ppEnabledExtensionNames = extensions;
    info->enabledExtensionCount = count;
    return VK_SUCCESS;
}

static void init_device_queues( struct vulkan_device *device, const VkDeviceQueueCreateInfo *create_info, VkDevice client_device )
{
    VkDeviceQueueInfo2 info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
    VkQueue client_queues = client_device->queues + device->queue_count;
    struct vulkan_queue *queues = device->queues + device->queue_count;

    TRACE( "Queue family index %u, queue count %u.\n", create_info->queueFamilyIndex, create_info->queueCount );

    info.flags = create_info->flags;
    info.queueFamilyIndex = create_info->queueFamilyIndex;
    for (info.queueIndex = 0; info.queueIndex < create_info->queueCount; info.queueIndex++)
    {
        VkQueue host_queue, client_queue = client_queues + info.queueIndex;
        struct vulkan_queue *queue = queues + info.queueIndex;

        if (info.flags && device->p_vkGetDeviceQueue2) device->p_vkGetDeviceQueue2( device->host.device, &info, &host_queue );
        else device->p_vkGetDeviceQueue( device->host.device, info.queueFamilyIndex, info.queueIndex, &host_queue );
        vulkan_object_init_ptr( &queue->obj, (UINT_PTR)host_queue, &client_queue->obj );
        queue->device = device;
        queue->info = info;

        TRACE( "Got device %p queue %p, host_queue %p.\n", device, queue, queue->host.queue );
    }

    device->queue_count += create_info->queueCount;
}

static VkResult win32u_vkCreateDevice( VkPhysicalDevice client_physical_device, const VkDeviceCreateInfo *client_create_info,
                                       const VkAllocationCallbacks *allocator, VkDevice *client_device_ptr )
{
    VkDeviceCreateInfo *create_info = (VkDeviceCreateInfo *)client_create_info; /* cast away const, chain has been copied in the thunks */
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;
    VkDevice host_device, client_device = *client_device_ptr;
    unsigned int queue_count, props_count, i;
    /* create_info may point at this until the host device is created */
    VkPhysicalDeviceFeatures features = {0};
    struct vulkan_device *device;
    struct mempool pool = {0};
    VkResult res;

    if (TRACE_ON(vulkan))
    {
        VkPhysicalDeviceProperties properties = {0};
        instance->p_vkGetPhysicalDeviceProperties( physical_device->host.physical_device, &properties );
        TRACE( "Device name: %s.\n", debugstr_a(properties.deviceName) );
        TRACE( "Vendor ID: %#x, Device ID: %#x.\n", properties.vendorID, properties.deviceID );
        TRACE( "Driver version: %#x.\n", properties.driverVersion );
    }

    /* We need to cache all queues within the device as each requires wrapping since queues are dispatchable objects. */
    for (queue_count = 0, i = 0; i < create_info->queueCreateInfoCount; i++) queue_count += create_info->pQueueCreateInfos[i].queueCount;
    instance->p_vkGetPhysicalDeviceQueueFamilyProperties(physical_device->host.physical_device, &props_count, NULL);

    if (!(device = calloc( 1, sizeof(*device) + queue_count * sizeof(*device->queues) + props_count * sizeof(*device->queue_props) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    device->extensions = client_device->extensions;
    device->queue_props = (void *)(device->queues + queue_count);

{
        VkPhysicalDeviceFeatures2 *features2;

        /* Enable shaderStorageImageWriteWithoutFormat for fshack
         * This is available on all hardware and driver combinations we care about.
         */
        if (create_info->pEnabledFeatures)
        {
            features = *create_info->pEnabledFeatures;
            features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
            create_info->pEnabledFeatures = &features;
        }
        if ((features2 = (void *)find_next_struct((const void *)create_info, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)))
        {
            features2->features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        }
        else if (!create_info->pEnabledFeatures)
        {
            features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
            create_info->pEnabledFeatures = &features;
        }
#ifdef WINE_NX_LSFG
        wine_nx_lsfg_device_features( instance->host.instance, physical_device->host.physical_device,
                                     features2 ? &features2->features : &features );
#endif
}

    if ((res = convert_device_create_info( physical_device, create_info, &pool, device ))) goto failed;
    if ((res = instance->p_vkCreateDevice( physical_device->host.physical_device, create_info, NULL /* allocator */, &host_device ))) goto failed;

    vulkan_object_init_ptr( &device->obj, (UINT_PTR)host_device, &client_device->obj );
    device->physical_device = physical_device;

#define USE_VK_FUNC( name )                                                          \
    device->p_##name = (void *)p_vkGetDeviceProcAddr( device->host.device, #name );  \
    if (!device->p_##name) TRACE( "Device proc %s not found.\n", #name );
    ALL_VK_DEVICE_FUNCS
#undef USE_VK_FUNC

    for (i = 0; i < create_info->queueCreateInfoCount; i++) init_device_queues( device, create_info->pQueueCreateInfos + i, client_device );
    instance->p_vkGetPhysicalDeviceQueueFamilyProperties( physical_device->host.physical_device, &props_count, device->queue_props );

    TRACE( "Created device %p, host_device %p.\n", device, device->host.device );
    for (struct vulkan_queue *queue = device->queues; queue < device->queues + device->queue_count; queue++)
        instance->p_insert_object( instance, &queue->obj );
    instance->p_insert_object( instance, &device->obj );

failed:
    if (res)
    {
#ifdef __SWITCH__
        {
            VkPhysicalDeviceFeatures supported = {0};
            const VkBool32 *want = (const VkBool32 *)create_info->pEnabledFeatures, *have = (const VkBool32 *)&supported;
            char names[160] = "", unsupported[96] = "";
            int len = 0, flen = 0;

            instance->p_vkGetPhysicalDeviceFeatures( physical_device->host.physical_device, &supported );
            for (i = 0; want && i < sizeof(supported) / sizeof(VkBool32); i++)
                if (want[i] && !have[i] && flen < (int)sizeof(unsupported) - 4)
                    flen += snprintf( unsupported + flen, sizeof(unsupported) - flen, " %u", i );
            for (i = 0; i < create_info->enabledExtensionCount && len < (int)sizeof(names) - 2; i++)
                len += snprintf( names + len, sizeof(names) - len, " %s", create_info->ppEnabledExtensionNames[i] );
            nx_vk_trace( "[NXVK] device creation failed, res %d; host extensions:%s; enabled features the GPU lacks (VkPhysicalDeviceFeatures indices):%s",
                         res, names, flen ? unsupported : " none" );
        }
#endif
        WARN( "Failed to create device, res %d\n", res );
        free( device );
    }
    mem_free( &pool );
    return res;
}

static void win32u_vkDestroyDevice( VkDevice client_device, const VkAllocationCallbacks *allocator )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_instance *instance = device->physical_device->instance;
    unsigned int i;

    if (!device) return;

    device->p_vkDestroyDevice( device->host.device, NULL /* pAllocator */ );
    for (i = 0; i < device->queue_count; i++)
        instance->p_remove_object( instance, &device->queues[i].obj );
    instance->p_remove_object( instance, &device->obj );

    free( device );
}

static VkQueue device_find_queue( VkDevice client_device, const VkDeviceQueueInfo2 *info )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );

    for (struct vulkan_queue *queue = device->queues; queue < device->queues + device->queue_count; queue++)
        if (!memcmp( &queue->info, info, sizeof(*info) )) return queue->client.queue;

    return VK_NULL_HANDLE;
}

static void win32u_vkGetDeviceQueue( VkDevice client_device, uint32_t family_index, uint32_t queue_index, VkQueue *client_queue )
{
    VkDeviceQueueInfo2 info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
    info.queueFamilyIndex = family_index;
    info.queueIndex = queue_index;

    *client_queue = device_find_queue( client_device, &info );
}

static void win32u_vkGetDeviceQueue2( VkDevice client_device, const VkDeviceQueueInfo2 *client_info, VkQueue *client_queue )
{
    VkDeviceQueueInfo2 info = *client_info;
    if (info.pNext) FIXME( "pNext not implemented\n" );
    info.pNext = NULL;

    *client_queue = device_find_queue( client_device, &info );
}

static VkResult win32u_vkAllocateMemory( VkDevice client_device, const VkMemoryAllocateInfo *client_alloc_info,
                                         const VkAllocationCallbacks *allocator, VkDeviceMemory *ret )
{
    VkImportMemoryFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    VkMemoryAllocateInfo *alloc_info = (VkMemoryAllocateInfo *)client_alloc_info; /* cast away const, chain has been copied in the thunks */
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)alloc_info;
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct vulkan_instance *instance = device->physical_device->instance;
    VkImportMemoryHostPointerInfoEXT host_pointer_info = {0}, *pointer_info = NULL;
    VkExportMemoryWin32HandleInfoKHR export_win32 = {.dwAccess = GENERIC_ALL};
    VkImportMemoryWin32HandleInfoKHR *import_win32 = NULL;
    VkDeviceMemory host_device_memory = VK_NULL_HANDLE;
    VkExportMemoryAllocateInfo *export_info = NULL;
    VkExternalMemoryHandleTypeFlagBits host_memory_type = get_host_external_memory_type();
    struct device_memory *memory;
    BOOL nt_shared = FALSE;
    BOOL native_shared_request;
    uint32_t shared_nvmap_id = 0;
    uint32_t mem_flags;
    void *mapping = NULL;
    VkResult res;
#ifdef __SWITCH__
    BOOL host_import_retry;
#endif
#ifdef WINE_NX_SWAP_POC
    struct horizon_swap_reclaim_budget reclaim_budget = { .remaining = SIZE_MAX };
    struct timespec alloc_start, alloc_end;
    unsigned int alloc_attempts = 0;
    BOOL reclaim_host_memory;

    clock_gettime( CLOCK_MONOTONIC, &alloc_start );
#endif

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_MEMORY_ALLOCATE_INFO_NV: break;
        case VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO:
            export_info = (VkExportMemoryAllocateInfo *)*next;
            if (!(export_info->handleTypes & EXTERNAL_MEMORY_WIN32_BITS))
                FIXME( "Unsupported handle types %#x\n", export_info->handleTypes );
            else
            {
                nt_shared = !(export_info->handleTypes & (VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT |
                                                          VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT));
                export_info->handleTypes = host_memory_type;
#ifdef __SWITCH__
                if (host_memory_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT)
                {
                    *next = (*next)->pNext; next = &prev;
                }
#endif
            }
            break;
        case VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR:
            export_win32 = *(VkExportMemoryWin32HandleInfoKHR *)*next;
            *next = (*next)->pNext; next = &prev;
            break;
        case VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT:
            pointer_info = (VkImportMemoryHostPointerInfoEXT *)*next;
            break;
        case VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR:
            import_win32 = (VkImportMemoryWin32HandleInfoKHR *)*next;
            *next = (*next)->pNext; next = &prev;
            break;
        case VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO: break;
        case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO: break;
        case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_TENSOR_ARM: break;
        case VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO: break;
        case VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    native_shared_request = host_memory_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT &&
                            (export_info || import_win32);
    /* For host visible memory, we try to use VK_EXT_external_memory_host on wow64 to ensure that mapped pointer is 32-bit. */
    mem_flags = physical_device->memory_properties.memoryTypes[alloc_info->memoryTypeIndex].propertyFlags;
#ifdef __SWITCH__
    host_import_retry = !export_info && !import_win32 && !pointer_info && physical_device->external_memory_align &&
                        (mem_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && nx_can_import_fragmented_memory( alloc_info );
#endif
    if (!native_shared_request && physical_device->external_memory_align &&
        (mem_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !pointer_info &&
#ifdef __SWITCH__
        !nx_driver_maps_preferred() &&
#endif
        (res = allocate_external_host_memory( device, alloc_info, mem_flags, &host_pointer_info )))
        return res;
    mapping = host_pointer_info.pHostPointer;

    if (!(memory = calloc( 1, sizeof(*memory) )))
    {
        if (mapping)
        {
            SIZE_T alloc_size = 0;
            NtFreeVirtualMemory( GetCurrentProcess(), &mapping, &alloc_size, MEM_RELEASE );
        }
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    if (import_win32)
    {
        switch (import_win32->handleType)
        {
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT:
            memory->global = PtrToUlong( import_win32->handle );
            memory->local = d3dkmt_open_resource( memory->global, NULL, &memory->mutex, &memory->sync );
            break;
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT:
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT:
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT:
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT:
        {
            HANDLE shared = import_win32->handle;
            if (import_win32->name && !(shared = open_shared_resource_from_name( import_win32->name ))) break;
            memory->local = d3dkmt_open_resource( 0, shared, &memory->mutex, &memory->sync );
            if (shared && shared != import_win32->handle) NtClose( shared );
            break;
        }
        default:
            FIXME( "Unsupported handle type %#x\n", import_win32->handleType );
            break;
        }

#if defined(__SWITCH__) && defined(WINE_NX_MESA_SWITCH)
        if (native_shared_request)
        {
            SIZE_T shared_size;

            if (!memory->local || !d3dkmt_resource_get_nvmap( memory->local, &shared_nvmap_id, &shared_size ) ||
                alloc_info->allocationSize > shared_size)
            {
                res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
                goto failed;
            }
            alloc_info->allocationSize = shared_size;
        }
        else
#endif
        {
            if (device->client.device->extensions.has_VK_KHR_win32_keyed_mutex && memory->sync)
            {
                VkSemaphoreTypeCreateInfo semaphore_type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
                VkSemaphoreCreateInfo semaphore_create = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &semaphore_type};
                VkImportSemaphoreFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};

                semaphore_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
                if ((res = device->p_vkCreateSemaphore( device->host.device, &semaphore_create, NULL, &memory->semaphore ))) goto failed;

                fd_info.handleType = get_host_external_semaphore_type();
                fd_info.semaphore = memory->semaphore;
                if ((fd_info.fd = d3dkmt_object_get_fd( memory->sync )) < 0)
                {
                    res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
                    goto failed;
                }

                if ((res = device->p_vkImportSemaphoreFdKHR( device->host.device, &fd_info ))) goto failed;
            }

            if ((fd_info.fd = d3dkmt_object_get_fd( memory->local )) < 0)
            {
                res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
                goto failed;
            }

            fd_info.handleType = get_host_external_memory_type();
            fd_info.pNext = alloc_info->pNext;
            alloc_info->pNext = &fd_info;
        }
    }

#ifdef WINE_NX_SWAP_POC
    reclaim_host_memory = !import_win32 && !pointer_info && !mapping && !native_shared_request;
    do
    {
        alloc_attempts++;
        /* Let the driver trim its BO cache before reclaiming guest pages. */
        if (reclaim_host_memory) horizon_swap_native_begin();
#endif
#if defined(__SWITCH__) && defined(WINE_NX_MESA_SWITCH)
    if (native_shared_request)
        res = wine_nx_vk_allocate_shared_memory( device->host.device, alloc_info,
                                                 shared_nvmap_id, &host_device_memory );
    else
#endif
        res = device->p_vkAllocateMemory( device->host.device, alloc_info, NULL, &host_device_memory );
#ifdef WINE_NX_SWAP_POC
        if (reclaim_host_memory) horizon_swap_native_end();
#endif
#ifdef __SWITCH__
    if (res == VK_ERROR_OUT_OF_HOST_MEMORY && host_import_retry)
    {
        VkResult import_res;

        /* Chunked Wine backing can use holes too small for one native allocation. */
        host_import_retry = FALSE;
        import_res = allocate_external_host_memory( device, alloc_info, mem_flags, &host_pointer_info );
        if (!import_res && (mapping = host_pointer_info.pHostPointer))
        {
#ifdef WINE_NX_SWAP_POC
            alloc_attempts++;
            reclaim_host_memory = FALSE;
#endif
            res = device->p_vkAllocateMemory( device->host.device, alloc_info, NULL, &host_device_memory );
        }
        else if (import_res) res = import_res;
    }
#endif
#ifdef WINE_NX_SWAP_POC
    } while (res == VK_ERROR_OUT_OF_HOST_MEMORY && reclaim_host_memory &&
             horizon_swap_native_reclaim( alloc_info->allocationSize, &reclaim_budget ));
    clock_gettime( CLOCK_MONOTONIC, &alloc_end );
    unsigned long long alloc_ms = (alloc_end.tv_sec - alloc_start.tv_sec) * 1000LL +
                                 (alloc_end.tv_nsec - alloc_start.tv_nsec) / 1000000LL;
    if (alloc_attempts > 1 || alloc_ms >= 50 || alloc_info->allocationSize >= 64 * UINT64_C(1048576))
        nx_vk_trace( "[NXVK-ALLOC] size_kb=%llu backing=%s attempts=%u elapsed_ms=%llu result=%d",
                     (unsigned long long)alloc_info->allocationSize >> 10, mapping ? "host" : "native",
                     alloc_attempts, alloc_ms, res );
#endif
    if (res) goto failed;

    if (export_info)
    {
        if (!memory->local)
        {
#if defined(__SWITCH__) && defined(WINE_NX_MESA_SWITCH)
            if (native_shared_request)
            {
                void *reference;

                if (!wine_nx_vk_export_memory( host_device_memory, &shared_nvmap_id, &reference ))
                {
                    res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
                    goto failed;
                }
                memory->local = d3dkmt_create_nvmap_resource( alloc_info->allocationSize,
                                                               shared_nvmap_id, reference,
                                                               nt_shared ? NULL : &memory->global );
                if (!memory->local)
                {
                    res = VK_ERROR_OUT_OF_HOST_MEMORY;
                    goto failed;
                }
            }
            else
#endif
            {
                VkMemoryGetFdInfoKHR get_fd_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = host_device_memory};
                int fd = -1;

                switch ((get_fd_info.handleType = get_host_external_memory_type()))
                {
                case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT:
                case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
                    if ((res = device->p_vkGetMemoryFdKHR( device->host.device, &get_fd_info, &fd ))) goto failed;
                    break;
                default:
                    FIXME( "Unsupported handle type %#x\n", get_fd_info.handleType );
                    res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
                    break;
                }

                if (fd < 0) goto failed;

                memory->local = d3dkmt_create_resource( fd, nt_shared ? NULL : &memory->global );
                close( fd );

                if (!memory->local)
                {
                    res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
                    goto failed;
                }
            }
        }
        if (nt_shared && !(memory->shared = create_shared_resource_handle( memory->local, &export_win32 )))
        {
            res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
            goto failed;
        }
    }

    vulkan_object_init( &memory->obj.obj, host_device_memory );
    memory->size = alloc_info->allocationSize;
    memory->vm_map = mapping;
    instance->p_insert_object( instance, &memory->obj.obj );
#ifdef __SWITCH__
    nx_memory_note_alloc( memory->size, mapping != NULL );
#endif

    *ret = memory->obj.client.device_memory;
    return VK_SUCCESS;

failed:
    WARN( "Failed to allocate memory, res %d\n", res );
#ifdef __SWITCH__
    if (nx_memory_log_failure())
    {
        nx_vk_trace( "[NXVK] vkAllocateMemory of %llu bytes of memory type %u failed: %d%s",
                     (unsigned long long)alloc_info->allocationSize, alloc_info->memoryTypeIndex, res,
                     mapping ? " (with an imported host mapping)" : "" );
    }
#endif
    if (host_device_memory) device->p_vkFreeMemory( device->host.device, host_device_memory, NULL );
    if (mapping)
    {
        SIZE_T alloc_size = 0;
        NtFreeVirtualMemory( GetCurrentProcess(), &mapping, &alloc_size, MEM_RELEASE );
    }
    if (memory->semaphore) device->p_vkDestroySemaphore( device->host.device, memory->semaphore, NULL );
    d3dkmt_destroy_resource( memory->local );
    d3dkmt_destroy_mutex( memory->mutex );
    d3dkmt_destroy_sync( memory->sync );
    free( memory );
    return res;
}

static void win32u_vkFreeMemory( VkDevice client_device, VkDeviceMemory client_memory, const VkAllocationCallbacks *allocator )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct vulkan_instance *instance = device->physical_device->instance;
    struct device_memory *memory;

    if (!client_memory) return;
    memory = device_memory_from_handle( client_memory );

    if (memory->vm_map && !physical_device->external_memory_align)
    {
        const VkMemoryUnmapInfoKHR info =
        {
            .sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR,
            .memory = memory->obj.host.device_memory,
            .flags = VK_MEMORY_UNMAP_RESERVE_BIT_EXT,
        };
        device->p_vkUnmapMemory2KHR( device->host.device, &info );
    }

    device->p_vkFreeMemory( device->host.device, memory->obj.host.device_memory, NULL );
    instance->p_remove_object( instance, &memory->obj.obj );
#ifdef __SWITCH__
    __atomic_add_fetch( &nx_memory_frees, 1, __ATOMIC_RELAXED );
    __atomic_add_fetch( &nx_memory_free_bytes, (LONGLONG)memory->size, __ATOMIC_RELAXED );
#endif

    if (memory->vm_map)
    {
        SIZE_T alloc_size = 0;
        NtFreeVirtualMemory( GetCurrentProcess(), &memory->vm_map, &alloc_size, MEM_RELEASE );
    }

    if (memory->semaphore) device->p_vkDestroySemaphore( device->host.device, memory->semaphore, NULL );
    if (memory->shared) NtClose( memory->shared );
    d3dkmt_destroy_resource( memory->local );
    d3dkmt_destroy_mutex( memory->mutex );
    d3dkmt_destroy_sync( memory->sync );
    free( memory );
}

static VkResult win32u_vkGetMemoryWin32HandleKHR( VkDevice client_device, const VkMemoryGetWin32HandleInfoKHR *handle_info, HANDLE *handle )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct device_memory *memory = device_memory_from_handle( handle_info->memory );

    TRACE( "device %p, handle_info %p, handle %p\n", device, handle_info, handle );

    switch (handle_info->handleType)
    {
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT:
        if (!memory->global) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        TRACE( "Returning global D3DKMT handle %#x\n", memory->global );
        *handle = UlongToPtr( memory->global );
        return VK_SUCCESS;

    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT:
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT:
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT:
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT:
        if (!memory->shared || NtDuplicateObject( NtCurrentProcess(), memory->shared, NtCurrentProcess(), handle,
                                                  0, 0, DUPLICATE_SAME_ATTRIBUTES | DUPLICATE_SAME_ACCESS ))
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        TRACE( "Returning NT shared handle %p -> %p\n", memory->shared, *handle );
        return VK_SUCCESS;

    default:
        FIXME( "Unsupported handle type %#x\n", handle_info->handleType );
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
}

static BOOL is_d3dkmt_global( D3DKMT_HANDLE handle )
{
    return (handle & 0xc0000000) && (handle & 0x3f) == 2;
}

static VkResult win32u_vkGetMemoryWin32HandlePropertiesKHR( VkDevice client_device, VkExternalMemoryHandleTypeFlagBits handle_type, HANDLE handle,
                                                            VkMemoryWin32HandlePropertiesKHR *handle_properties )
{
    static const UINT d3dkmt_type_bits = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT | VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
    struct vulkan_device *device = vulkan_device_from_handle( client_device );

    TRACE( "device %p, handle_type %#x, handle %p, handle_properties %p\n", device, handle_type, handle, handle_properties );

    if (is_d3dkmt_global( HandleToULong( handle ) )) handle_properties->memoryTypeBits = d3dkmt_type_bits;
    else handle_properties->memoryTypeBits = EXTERNAL_MEMORY_WIN32_BITS & ~d3dkmt_type_bits;

    return VK_SUCCESS;
}

static VkResult win32u_vkMapMemory2KHR( VkDevice client_device, const VkMemoryMapInfoKHR *map_info, void **data )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct device_memory *memory = device_memory_from_handle( map_info->memory );
    VkMemoryMapInfoKHR info = *map_info;
    VkMemoryMapPlacedInfoEXT placed_info =
    {
        .sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT,
    };
    VkResult res;

    info.memory = memory->obj.host.device_memory;
    if (memory->vm_map)
    {
        *data = (char *)memory->vm_map + info.offset;
        TRACE( "returning %p\n", *data );
        res = VK_SUCCESS;
        goto done;
    }

    if (physical_device->map_placed_align)
    {
        SIZE_T alloc_size = memory->size;

        placed_info.pNext = info.pNext;
        info.pNext = &placed_info;
        info.offset = 0;
        info.size = VK_WHOLE_SIZE;
        info.flags |= VK_MEMORY_MAP_PLACED_BIT_EXT;

        if (NtAllocateVirtualMemory( GetCurrentProcess(), &placed_info.pPlacedAddress, zero_bits,
                                     &alloc_size, MEM_COMMIT, PAGE_READWRITE ))
        {
            ERR( "NtAllocateVirtualMemory failed\n" );
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    if (device->p_vkMapMemory2KHR)
        res = device->p_vkMapMemory2KHR( device->host.device, &info, data );
    else
    {
        if (info.pNext) FIXME( "struct extension chain not implemented!\n" );
        res = device->p_vkMapMemory( device->host.device, info.memory, info.offset, info.size, info.flags, data );
    }

    if (placed_info.pPlacedAddress)
    {
        if (res != VK_SUCCESS)
        {
            SIZE_T alloc_size = 0;
            ERR( "vkMapMemory2EXT failed: %d\n", res );
            NtFreeVirtualMemory( GetCurrentProcess(), &placed_info.pPlacedAddress, &alloc_size, MEM_RELEASE );
            return res;
        }
        memory->vm_map = placed_info.pPlacedAddress;
        *data = (char *)memory->vm_map + map_info->offset;
        TRACE( "Using placed mapping %p\n", memory->vm_map );
    }

#ifdef __SWITCH__
    {
        static LONG map_logs;

        if (res == VK_SUCCESS && !memory->vm_map && __atomic_add_fetch( &map_logs, 1, __ATOMIC_RELAXED ) <= 8)
            nx_vk_trace( "[NXVK] driver mapping %p for %llu bytes", *data, (unsigned long long)memory->size );
    }
#endif
done:
#ifdef _WIN64
    if (NtCurrentTeb()->WowTebOffset && res == VK_SUCCESS &&
        ((UINT_PTR)*data > UINT32_MAX ||
         (map_info->size == VK_WHOLE_SIZE ? memory->size - map_info->offset : map_info->size) >
         0x100000000ULL - (UINT_PTR)*data))
    {
#ifdef __SWITCH__
        nx_vk_trace( "[NXVK] rejected Vulkan mapping %p: range exceeds the Win32 address space", *data );
#endif
        FIXME( "returned mapping %p does not fit 32-bit pointer\n", *data );
        if (!memory->vm_map) device->p_vkUnmapMemory( device->host.device, memory->obj.host.device_memory );
        *data = NULL;
        res = VK_ERROR_MEMORY_MAP_FAILED;
    }
#endif

    return res;
}

static VkResult win32u_vkMapMemory( VkDevice client_device, VkDeviceMemory client_memory, VkDeviceSize offset,
                                    VkDeviceSize size, VkMemoryMapFlags flags, void **data )
{
    const VkMemoryMapInfoKHR info =
    {
        .sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR,
        .flags = flags,
        .memory = client_memory,
        .offset = offset,
        .size = size,
    };

    return win32u_vkMapMemory2KHR( client_device, &info, data );
}

static VkResult win32u_vkUnmapMemory2KHR( VkDevice client_device, const VkMemoryUnmapInfoKHR *unmap_info )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct device_memory *memory = device_memory_from_handle( unmap_info->memory );
    VkMemoryUnmapInfoKHR info;
    VkResult res;

    if (memory->vm_map && physical_device->external_memory_align) return VK_SUCCESS;

    if (!device->p_vkUnmapMemory2KHR)
    {
        if (unmap_info->pNext || memory->vm_map) FIXME( "Not implemented\n" );
        device->p_vkUnmapMemory( device->host.device, memory->obj.host.device_memory );
        return VK_SUCCESS;
    }

    info = *unmap_info;
    info.memory = memory->obj.host.device_memory;
    if (memory->vm_map) info.flags |= VK_MEMORY_UNMAP_RESERVE_BIT_EXT;

    res = device->p_vkUnmapMemory2KHR( device->host.device, &info );

    if (res == VK_SUCCESS && memory->vm_map)
    {
        SIZE_T size = 0;
        NtFreeVirtualMemory( GetCurrentProcess(), &memory->vm_map, &size, MEM_RELEASE );
        memory->vm_map = NULL;
    }
    return res;
}

static void win32u_vkUnmapMemory( VkDevice client_device, VkDeviceMemory client_memory )
{
    const VkMemoryUnmapInfoKHR info =
    {
        .sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR,
        .memory = client_memory,
    };

    win32u_vkUnmapMemory2KHR( client_device, &info );
}

static VkResult win32u_vkCreateBuffer( VkDevice client_device, const VkBufferCreateInfo *create_info,
                                       const VkAllocationCallbacks *allocator, VkBuffer *buffer )
{
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)create_info; /* cast away const, chain has been copied in the thunks */
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    VkExternalMemoryBufferCreateInfo host_external_info, *external_info = NULL;

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_BUFFER_CREATE_INFO_NV: break;
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO:
            external_info = (VkExternalMemoryBufferCreateInfo *)*next;
            if (!(external_info->handleTypes & EXTERNAL_MEMORY_WIN32_BITS))
                FIXME( "Unsupported handle types %#x\n", external_info->handleTypes );
            else
                external_info->handleTypes = get_host_external_memory_type();
            break;
        case VK_STRUCTURE_TYPE_OPAQUE_CAPTURE_DESCRIPTOR_DATA_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    if (physical_device->external_memory_align && !external_info)
    {
        host_external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        host_external_info.pNext = create_info->pNext;
        host_external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        ((VkBufferCreateInfo *)create_info)->pNext = &host_external_info; /* cast away const, it has been copied in the thunks */
    }

    return device->p_vkCreateBuffer( device->host.device, create_info, NULL, buffer );
}

static void win32u_vkGetDeviceBufferMemoryRequirements( VkDevice client_device, const VkDeviceBufferMemoryRequirements *buffer_requirements,
                                                        VkMemoryRequirements2 *memory_requirements )
{
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)buffer_requirements->pCreateInfo; /* cast away const, chain has been copied in the thunks */
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    VkExternalMemoryBufferCreateInfo *external_info;

    TRACE( "device %p, buffer_requirements %p, memory_requirements %p\n", device, buffer_requirements, memory_requirements );

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_BUFFER_CREATE_INFO_NV: break;
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO:
            external_info = (VkExternalMemoryBufferCreateInfo *)*next;
            if (!(external_info->handleTypes & EXTERNAL_MEMORY_WIN32_BITS))
                FIXME( "Unsupported handle types %#x\n", external_info->handleTypes );
            else
                external_info->handleTypes = get_host_external_memory_type();
            break;
        case VK_STRUCTURE_TYPE_OPAQUE_CAPTURE_DESCRIPTOR_DATA_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    device->p_vkGetDeviceBufferMemoryRequirements( device->host.device, buffer_requirements, memory_requirements );
}

static void get_physical_device_external_buffer_properties( struct vulkan_physical_device *physical_device, const VkPhysicalDeviceExternalBufferInfo *client_buffer_info,
                                                            VkExternalBufferProperties *buffer_properties, PFN_vkGetPhysicalDeviceExternalBufferProperties p_vkGetPhysicalDeviceExternalBufferProperties )
{
    VkPhysicalDeviceExternalBufferInfo *buffer_info = (VkPhysicalDeviceExternalBufferInfo *)client_buffer_info; /* cast away const, it has been copied in the thunks */
    VkExternalMemoryHandleTypeFlagBits handle_type = 0;

    handle_type = buffer_info->handleType;
    if (handle_type & EXTERNAL_MEMORY_WIN32_BITS) buffer_info->handleType = get_host_external_memory_type();

    p_vkGetPhysicalDeviceExternalBufferProperties( physical_device->host.physical_device, buffer_info, buffer_properties );
    map_external_memory_properties( &buffer_properties->externalMemoryProperties, handle_type );
}

static void win32u_vkGetPhysicalDeviceExternalBufferProperties( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalBufferInfo *buffer_info,
                                                                VkExternalBufferProperties *buffer_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, buffer_info %p, buffer_properties %p\n", physical_device, buffer_info, buffer_properties );

    get_physical_device_external_buffer_properties( physical_device, buffer_info, buffer_properties, instance->p_vkGetPhysicalDeviceExternalBufferProperties );
}

static void win32u_vkGetPhysicalDeviceExternalBufferPropertiesKHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalBufferInfo *buffer_info,
                                                                   VkExternalBufferProperties *buffer_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, buffer_info %p, buffer_properties %p\n", physical_device, buffer_info, buffer_properties );

    get_physical_device_external_buffer_properties( physical_device, buffer_info, buffer_properties, instance->p_vkGetPhysicalDeviceExternalBufferPropertiesKHR );
}

static VkResult win32u_vkCreateImage( VkDevice client_device, const VkImageCreateInfo *create_info,
                                      const VkAllocationCallbacks *allocator, VkImage *image )
{
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)create_info; /* cast away const, chain has been copied in the thunks */
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    VkExternalMemoryImageCreateInfo host_external_info, *external_info = NULL;

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_IMAGE_CREATE_INFO_NV: break;
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO:
            external_info = (VkExternalMemoryImageCreateInfo *)*next;
            if (!(external_info->handleTypes & EXTERNAL_MEMORY_WIN32_BITS))
                FIXME( "Unsupported handle types %#x\n", external_info->handleTypes );
            else
                external_info->handleTypes = get_host_external_memory_type();
            break;
        case VK_STRUCTURE_TYPE_IMAGE_ALIGNMENT_CONTROL_CREATE_INFO_MESA: break;
        case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT: break;
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR: break;
        case VK_STRUCTURE_TYPE_OPAQUE_CAPTURE_DESCRIPTOR_DATA_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV: break;
        case VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    if (physical_device->external_memory_align && !external_info)
    {
        host_external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        host_external_info.pNext = create_info->pNext;
        host_external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        ((VkImageCreateInfo *)create_info)->pNext = &host_external_info; /* cast away const, it has been copied in the thunks */
    }

    return device->p_vkCreateImage( device->host.device, create_info, NULL, image );
}

static void win32u_vkGetDeviceImageMemoryRequirements( VkDevice client_device, const VkDeviceImageMemoryRequirements *image_requirements,
                                                       VkMemoryRequirements2 *memory_requirements )
{
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)image_requirements->pCreateInfo; /* cast away const, chain has been copied in the thunks */
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    VkExternalMemoryImageCreateInfo *external_info;

    TRACE( "device %p, image_requirements %p, memory_requirements %p\n", device, image_requirements, memory_requirements );

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_IMAGE_CREATE_INFO_NV: break;
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO:
            external_info = (VkExternalMemoryImageCreateInfo *)*next;
            if (!(external_info->handleTypes & EXTERNAL_MEMORY_WIN32_BITS))
                FIXME( "Unsupported handle types %#x\n", external_info->handleTypes );
            else
                external_info->handleTypes = get_host_external_memory_type();
            break;
        case VK_STRUCTURE_TYPE_IMAGE_ALIGNMENT_CONTROL_CREATE_INFO_MESA: break;
        case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT: break;
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR: break;
        case VK_STRUCTURE_TYPE_OPAQUE_CAPTURE_DESCRIPTOR_DATA_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV: break;
        case VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    device->p_vkGetDeviceImageMemoryRequirements( device->host.device, image_requirements, memory_requirements );
}

static VkResult get_physical_device_image_format_properties( struct vulkan_physical_device *physical_device, const VkPhysicalDeviceImageFormatInfo2 *format_info,
                                                             VkImageFormatProperties2 *format_properties, PFN_vkGetPhysicalDeviceImageFormatProperties2 p_vkGetPhysicalDeviceImageFormatProperties2 )
{
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)format_info; /* cast away const, chain has been copied in the thunks */
    VkExternalMemoryHandleTypeFlagBits handle_type = 0;
    VkResult res;

    TRACE( "physical_device %p, format_info %p, format_properties %p\n", physical_device, format_info, format_properties );

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT: break;
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV: break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO:
        {
            VkPhysicalDeviceExternalImageFormatInfo *external_info = (VkPhysicalDeviceExternalImageFormatInfo *)*next;
            handle_type = external_info->handleType;
            if (handle_type & EXTERNAL_MEMORY_WIN32_BITS) external_info->handleType = get_host_external_memory_type();
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_IMAGE_FORMAT_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    res = p_vkGetPhysicalDeviceImageFormatProperties2( physical_device->host.physical_device, format_info, format_properties );
    for (prev = (VkBaseOutStructure *)format_properties, next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES:
        {
            VkExternalImageFormatProperties *props = (VkExternalImageFormatProperties *)*next;
            map_external_memory_properties( &props->externalMemoryProperties, handle_type );
            break;
        }
        case VK_STRUCTURE_TYPE_FILTER_CUBIC_IMAGE_VIEW_IMAGE_FORMAT_PROPERTIES_EXT: break;
        case VK_STRUCTURE_TYPE_HOST_IMAGE_COPY_DEVICE_PERFORMANCE_QUERY: break;
        case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_PROPERTIES_EXT: break;
        case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES: break;
        case VK_STRUCTURE_TYPE_TEXTURE_LOD_GATHER_FORMAT_PROPERTIES_AMD: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    return res;
}

static VkResult win32u_vkGetPhysicalDeviceImageFormatProperties2( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceImageFormatInfo2 *format_info,
                                                                  VkImageFormatProperties2 *format_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, format_info %p, format_properties %p\n", physical_device, format_info, format_properties );

    return get_physical_device_image_format_properties( physical_device, format_info, format_properties, instance->p_vkGetPhysicalDeviceImageFormatProperties2 );
}

static VkResult win32u_vkGetPhysicalDeviceImageFormatProperties2KHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceImageFormatInfo2 *format_info,
                                                                     VkImageFormatProperties2 *format_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, format_info %p, format_properties %p\n", physical_device, format_info, format_properties );

    return get_physical_device_image_format_properties( physical_device, format_info, format_properties, instance->p_vkGetPhysicalDeviceImageFormatProperties2KHR );
}

static VkResult win32u_vkCreateWin32SurfaceKHR( VkInstance client_instance, const VkWin32SurfaceCreateInfoKHR *create_info,
                                                const VkAllocationCallbacks *allocator, VkSurfaceKHR *ret )
{
    struct vulkan_instance *instance = vulkan_instance_from_handle( client_instance );
    VkSurfaceKHR host_surface;
    struct surface *surface;
    HWND dummy = NULL;
    VkResult res;

    TRACE( "client_instance %p, create_info %p, allocator %p, ret %p\n", client_instance, create_info, allocator, ret );
    if (allocator) FIXME( "Support for allocation callbacks not implemented yet\n" );

    if (!(surface = calloc( 1, sizeof(*surface) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;

    /* Windows allows surfaces to be created with no HWND, they return VK_ERROR_SURFACE_LOST_KHR later */
    if (!(surface->hwnd = create_info->hwnd))
    {
        static const WCHAR staticW[] = {'s','t','a','t','i','c',0};
        UNICODE_STRING static_us = RTL_CONSTANT_STRING( staticW );
        dummy = NtUserCreateWindowEx( 0, &static_us, NULL, &static_us, WS_POPUP, 0, 0, 0, 0,
                                      NULL, NULL, NULL, NULL, 0, NULL, NULL, FALSE );
        WARN( "Created dummy window %p for null surface window\n", dummy );
        surface->hwnd = dummy;
    }

    if (!(surface->client = get_unused_client_surface( surface->hwnd, 0, fshack_enabled )))
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
    else
    {
        res = driver_funcs->p_vulkan_surface_create( surface->client, instance, &host_surface );
        use_window_client_surface( surface->client, !res );
    }
    if (res)
    {
        if (surface->client) client_surface_release( surface->client );
        if (dummy) NtUserDestroyWindow( dummy );
        free( surface );
        return res;
    }
    set_window_pixel_format( surface->hwnd, -1, TRUE );

    vulkan_object_init( &surface->obj.obj, host_surface );
    surface->obj.instance = instance;
    instance->p_insert_object( instance, &surface->obj.obj );

    if (dummy) NtUserDestroyWindow( dummy );

    *ret = surface->obj.client.surface;
    return VK_SUCCESS;
}

static void win32u_vkDestroySurfaceKHR( VkInstance client_instance, VkSurfaceKHR client_surface,
                                        const VkAllocationCallbacks *allocator )
{
    struct vulkan_instance *instance = vulkan_instance_from_handle( client_instance );
    struct surface *surface = surface_from_handle( client_surface );

    if (!surface) return;

    TRACE( "instance %p, handle 0x%s, allocator %p\n", instance, wine_dbgstr_longlong( client_surface ), allocator );
    if (allocator) FIXME( "Support for allocation callbacks not implemented yet\n" );

    instance->p_vkDestroySurfaceKHR( instance->host.instance, surface->obj.host.surface, NULL /* allocator */ );
#if defined(__SWITCH__) && defined(WINE_NX_MESA_SWITCH)
    {
        extern void wine_nx_gl_release_window(void);
        wine_nx_gl_release_window();
    }
#endif
    use_window_client_surface( surface->client, FALSE );
    client_surface_release( surface->client );

    instance->p_remove_object( instance, &surface->obj.obj );

    free( surface );
}

static BOOL get_surface_rect( HWND hwnd, RECT *rect, UINT dpi )
{
    if (!NtUserGetPresentRect( hwnd, rect, dpi ) && !NtUserGetClientRect( hwnd, rect, dpi )) return FALSE;
    OffsetRect( rect, -rect->left, -rect->top );
    return TRUE;
}

static void adjust_surface_capabilities( struct vulkan_instance *instance, struct surface *surface,
                                         VkSurfaceCapabilitiesKHR *capabilities )
{
    VkExtent2D host_max = capabilities->maxImageExtent;
    RECT client_rect = {0};
    UINT width, height;

    /* Many Windows games, for example Strange Brigade, No Man's Sky, Path of Exile
     * and World War Z, do not expect that maxImageCount can be set to 0.
     * A value of 0 means that there is no limit on the number of images.
     * Nvidia reports 8 on Windows, AMD 16.
     * https://vulkan.gpuinfo.org/displayreport.php?id=9122#surface
     * https://vulkan.gpuinfo.org/displayreport.php?id=9121#surface
     */
    if (!capabilities->maxImageCount) capabilities->maxImageCount = max( capabilities->minImageCount, 16 );

    /* Update the image extents to match what the Win32 WSI would provide. */
    /* FIXME: handle DPI scaling, somehow */
    if (!get_surface_rect( surface->hwnd, &client_rect, NtUserGetDpiForWindow( surface->hwnd ) ))
    {
        /* Nothing to take them from: what the driver said stands. Reading them
         * out of the rect anyway takes them from whatever was on the stack,
         * and an extent of millions is a swapchain no device can make. */
        WARN( "surface %p, hwnd %p has no rect to take the extents from\n", surface, surface->hwnd );
        return;
    }
    width = client_rect.right - client_rect.left;
    height = client_rect.bottom - client_rect.top;
    /* Never more than the device can make, whatever the rect said. */
    if (host_max.width && width > host_max.width) width = host_max.width;
    if (host_max.height && height > host_max.height) height = host_max.height;
    capabilities->minImageExtent.width = width;
    capabilities->minImageExtent.height = height;
    capabilities->maxImageExtent.width = width;
    capabilities->maxImageExtent.height = height;
    capabilities->currentExtent.width = width;
    capabilities->currentExtent.height = height;
}

static VkResult win32u_vkGetPhysicalDeviceSurfaceCapabilitiesKHR( VkPhysicalDevice client_physical_device, VkSurfaceKHR client_surface,
                                                                  VkSurfaceCapabilitiesKHR *capabilities )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct surface *surface = surface_from_handle( client_surface );
    struct vulkan_instance *instance = physical_device->instance;
    VkResult res;

    if (!NtUserIsWindow( surface->hwnd )) return VK_ERROR_SURFACE_LOST_KHR;
    res = instance->p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR( physical_device->host.physical_device,
                                                       surface->obj.host.surface, capabilities );
    if (!res) adjust_surface_capabilities( instance, surface, capabilities );
    return res;
}

static VkResult win32u_vkGetPhysicalDeviceSurfaceCapabilities2KHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceSurfaceInfo2KHR *surface_info,
                                                                   VkSurfaceCapabilities2KHR *capabilities )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct surface *surface = surface_from_handle( surface_info->surface );
    VkPhysicalDeviceSurfaceInfo2KHR surface_info_host = *surface_info;
    struct vulkan_instance *instance = physical_device->instance;
    VkResult res;

    if (!instance->p_vkGetPhysicalDeviceSurfaceCapabilities2KHR)
    {
        /* Until the loader version exporting this function is common, emulate it using the older non-2 version. */
        if (surface_info->pNext || capabilities->pNext) FIXME( "Emulating vkGetPhysicalDeviceSurfaceCapabilities2KHR, ignoring pNext.\n" );
        return win32u_vkGetPhysicalDeviceSurfaceCapabilitiesKHR( client_physical_device, surface_info->surface,
                                                                 &capabilities->surfaceCapabilities );
    }

    surface_info_host.surface = surface->obj.host.surface;

    if (!NtUserIsWindow( surface->hwnd )) return VK_ERROR_SURFACE_LOST_KHR;
    res = instance->p_vkGetPhysicalDeviceSurfaceCapabilities2KHR( physical_device->host.physical_device,
                                                                     &surface_info_host, capabilities );
    if (!res) adjust_surface_capabilities( instance, surface, &capabilities->surfaceCapabilities );
    return res;
}

static VkResult win32u_vkGetPhysicalDevicePresentRectanglesKHR( VkPhysicalDevice client_physical_device, VkSurfaceKHR client_surface,
                                                                uint32_t *rect_count, VkRect2D *rects )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct surface *surface = surface_from_handle( client_surface );
    struct vulkan_instance *instance = physical_device->instance;

    if (!NtUserIsWindow( surface->hwnd ))
    {
        if (rects && !*rect_count) return VK_INCOMPLETE;
        if (rects) memset( rects, 0, sizeof(VkRect2D) );
        *rect_count = 1;
        return VK_SUCCESS;
    }

    return instance->p_vkGetPhysicalDevicePresentRectanglesKHR( physical_device->host.physical_device,
                                                                   surface->obj.host.surface, rect_count, rects );
}

static void *find_vk_struct( void *s, VkStructureType t )
{
    VkBaseOutStructure *header;

    for (header = s; header; header = header->pNext)
    {
        if (header->sType == t) return header;
    }

    return NULL;
}

static void get_physical_device_properties2( struct vulkan_physical_device *physical_device, VkPhysicalDeviceProperties2 *properties2,
                                             PFN_vkGetPhysicalDeviceProperties2 p_vkGetPhysicalDeviceProperties2 )
{
    VkPhysicalDeviceIDProperties id_host = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 properties2_host;
    VkPhysicalDeviceVulkan11Properties *vk11;
    VkPhysicalDeviceIDProperties *id;
    VkBool32 device_luid_valid;
    UINT32 node_mask = 0;
    const GUID *uuid;
    LUID luid;

    vk11 = find_vk_struct( properties2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES );
    id = find_vk_struct( properties2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES );

    if (!vk11 && !id)
    {
        properties2_host = *properties2;
        id_host.pNext = properties2->pNext;
        properties2_host.pNext = &id_host;
        p_vkGetPhysicalDeviceProperties2( physical_device->host.physical_device, &properties2_host );
        properties2->properties = properties2_host.properties;
    }
    else p_vkGetPhysicalDeviceProperties2( physical_device->host.physical_device, properties2 );

    if (id)        uuid = (const GUID *)id->deviceUUID;
    else if (vk11) uuid = (const GUID *)vk11->deviceUUID;
    else           uuid = (const GUID *)id_host.deviceUUID;

    device_luid_valid = get_gpu_info_from_uuid( uuid, &luid, &node_mask, properties2->properties.deviceName );
    if (!device_luid_valid) WARN( "luid for %s not found\n", debugstr_guid(uuid) );

    if (id)
    {
        if (device_luid_valid) memcpy( &id->deviceLUID, &luid, sizeof(id->deviceLUID) );
        id->deviceLUIDValid = device_luid_valid;
        id->deviceNodeMask = node_mask;
    }

    if (vk11)
    {
        if (device_luid_valid) memcpy( &vk11->deviceLUID, &luid, sizeof(vk11->deviceLUID) );
        vk11->deviceLUIDValid = device_luid_valid;
        vk11->deviceNodeMask = node_mask;
    }


    TRACE( "deviceName:%s deviceLUIDValid:%d LUID:%08x:%08x.\n",
           properties2->properties.deviceName, device_luid_valid, luid.HighPart, luid.LowPart );
}

static void win32u_vkGetPhysicalDeviceProperties( VkPhysicalDevice client_physical_device, VkPhysicalDeviceProperties *properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    VkPhysicalDeviceProperties2 properties2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };

    if (!physical_device->instance->extensions.has_VK_KHR_get_physical_device_properties2)
    {
        physical_device->instance->p_vkGetPhysicalDeviceProperties( physical_device->host.physical_device, properties );
        return;
    }
    get_physical_device_properties2( physical_device, &properties2,
                                     physical_device->instance->p_vkGetPhysicalDeviceProperties2KHR );
    *properties = properties2.properties;
}

static void win32u_vkGetPhysicalDeviceProperties2( VkPhysicalDevice client_physical_device, VkPhysicalDeviceProperties2 *properties2 )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );

    get_physical_device_properties2( physical_device, properties2, physical_device->instance->p_vkGetPhysicalDeviceProperties2 );
}

static void win32u_vkGetPhysicalDeviceProperties2KHR( VkPhysicalDevice client_physical_device, VkPhysicalDeviceProperties2 *properties2 )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );

    get_physical_device_properties2( physical_device, properties2, physical_device->instance->p_vkGetPhysicalDeviceProperties2KHR );
}

static VkResult win32u_vkGetPhysicalDeviceSurfaceFormatsKHR( VkPhysicalDevice client_physical_device, VkSurfaceKHR client_surface,
                                                             uint32_t *format_count, VkSurfaceFormatKHR *formats )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct surface *surface = surface_from_handle( client_surface );
    struct vulkan_instance *instance = physical_device->instance;

    return instance->p_vkGetPhysicalDeviceSurfaceFormatsKHR( physical_device->host.physical_device,
                                                                surface->obj.host.surface, format_count, formats );
}

static VkResult win32u_vkGetPhysicalDeviceSurfaceFormats2KHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceSurfaceInfo2KHR *surface_info,
                                                              uint32_t *format_count, VkSurfaceFormat2KHR *formats )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct surface *surface = surface_from_handle( surface_info->surface );
    VkPhysicalDeviceSurfaceInfo2KHR surface_info_host = *surface_info;
    struct vulkan_instance *instance = physical_device->instance;
    VkResult res;

    if (!instance->p_vkGetPhysicalDeviceSurfaceFormats2KHR)
    {
        VkSurfaceFormatKHR *surface_formats;
        UINT i;

        /* Until the loader version exporting this function is common, emulate it using the older non-2 version. */
        if (surface_info->pNext) FIXME( "Emulating vkGetPhysicalDeviceSurfaceFormats2KHR, ignoring pNext.\n" );
        if (!formats) return win32u_vkGetPhysicalDeviceSurfaceFormatsKHR( client_physical_device, surface_info->surface, format_count, NULL );

        surface_formats = calloc( *format_count, sizeof(*surface_formats) );
        if (!surface_formats) return VK_ERROR_OUT_OF_HOST_MEMORY;

        res = win32u_vkGetPhysicalDeviceSurfaceFormatsKHR( client_physical_device, surface_info->surface, format_count, surface_formats );
        if (!res || res == VK_INCOMPLETE) for (i = 0; i < *format_count; i++) formats[i].surfaceFormat = surface_formats[i];

        free( surface_formats );
        return res;
    }

    surface_info_host.surface = surface->obj.host.surface;

    return instance->p_vkGetPhysicalDeviceSurfaceFormats2KHR( physical_device->host.physical_device,
                                                                 &surface_info_host, format_count, formats );
}

static VkBool32 win32u_vkGetPhysicalDeviceWin32PresentationSupportKHR( VkPhysicalDevice client_physical_device, uint32_t queue )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    return driver_funcs->p_get_physical_device_presentation_support( physical_device, queue );
}

static BOOL extents_equals( const VkExtent2D *extents, const RECT *rect )
{
    return extents->width == rect->right - rect->left && extents->height == rect->bottom - rect->top;
}

/*
#version 460

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(binding = 0) uniform sampler2D texSampler;
layout(binding = 1) uniform writeonly image2D outImage;
layout(push_constant) uniform pushConstants {
    //both in real image coords
    vec2 offset;
    vec2 extents;
} constants;

void main()
{
    vec2 texcoord = (vec2(gl_GlobalInvocationID.xy) - constants.offset) / constants.extents;
    vec4 c = texture(texSampler, texcoord);

    // Convert linear -> srgb
    bvec3 isLo = lessThanEqual(c.rgb, vec3(0.0031308f));
    vec3 loPart = c.rgb * 12.92f;
    vec3 hiPart = pow(c.rgb, vec3(5.0f / 12.0f)) * 1.055f - 0.055f;
    c.rgb = mix(hiPart, loPart, isLo);

    imageStore(outImage, ivec2(gl_GlobalInvocationID.xy), c);
}

*/
const uint32_t blit_comp_spv[] =
{
    0x07230203, 0x00010000, 0x0008000a, 0x0000005e, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
    0x00000038, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e,
    0x00000000, 0x00000001, 0x0006000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000d,
    0x00060010, 0x00000004, 0x00000011, 0x00000008, 0x00000008, 0x00000001, 0x00030003, 0x00000002,
    0x000001cc, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00050005, 0x00000009, 0x63786574,
    0x64726f6f, 0x00000000, 0x00080005, 0x0000000d, 0x475f6c67, 0x61626f6c, 0x766e496c, 0x7461636f,
    0x496e6f69, 0x00000044, 0x00060005, 0x00000012, 0x68737570, 0x736e6f43, 0x746e6174, 0x00000073,
    0x00050006, 0x00000012, 0x00000000, 0x7366666f, 0x00007465, 0x00050006, 0x00000012, 0x00000001,
    0x65747865, 0x0073746e, 0x00050005, 0x00000014, 0x736e6f63, 0x746e6174, 0x00000073, 0x00030005,
    0x00000021, 0x00000063, 0x00050005, 0x00000025, 0x53786574, 0x6c706d61, 0x00007265, 0x00040005,
    0x0000002d, 0x6f4c7369, 0x00000000, 0x00040005, 0x00000035, 0x61506f6c, 0x00007472, 0x00040005,
    0x0000003a, 0x61506968, 0x00007472, 0x00050005, 0x00000055, 0x4974756f, 0x6567616d, 0x00000000,
    0x00040047, 0x0000000d, 0x0000000b, 0x0000001c, 0x00050048, 0x00000012, 0x00000000, 0x00000023,
    0x00000000, 0x00050048, 0x00000012, 0x00000001, 0x00000023, 0x00000008, 0x00030047, 0x00000012,
    0x00000002, 0x00040047, 0x00000025, 0x00000022, 0x00000000, 0x00040047, 0x00000025, 0x00000021,
    0x00000000, 0x00040047, 0x00000055, 0x00000022, 0x00000000, 0x00040047, 0x00000055, 0x00000021,
    0x00000001, 0x00030047, 0x00000055, 0x00000019, 0x00040047, 0x0000005d, 0x0000000b, 0x00000019,
    0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020,
    0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040020, 0x00000008, 0x00000007, 0x00000007,
    0x00040015, 0x0000000a, 0x00000020, 0x00000000, 0x00040017, 0x0000000b, 0x0000000a, 0x00000003,
    0x00040020, 0x0000000c, 0x00000001, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000001,
    0x00040017, 0x0000000e, 0x0000000a, 0x00000002, 0x0004001e, 0x00000012, 0x00000007, 0x00000007,
    0x00040020, 0x00000013, 0x00000009, 0x00000012, 0x0004003b, 0x00000013, 0x00000014, 0x00000009,
    0x00040015, 0x00000015, 0x00000020, 0x00000001, 0x0004002b, 0x00000015, 0x00000016, 0x00000000,
    0x00040020, 0x00000017, 0x00000009, 0x00000007, 0x0004002b, 0x00000015, 0x0000001b, 0x00000001,
    0x00040017, 0x0000001f, 0x00000006, 0x00000004, 0x00040020, 0x00000020, 0x00000007, 0x0000001f,
    0x00090019, 0x00000022, 0x00000006, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000001,
    0x00000000, 0x0003001b, 0x00000023, 0x00000022, 0x00040020, 0x00000024, 0x00000000, 0x00000023,
    0x0004003b, 0x00000024, 0x00000025, 0x00000000, 0x0004002b, 0x00000006, 0x00000028, 0x00000000,
    0x00020014, 0x0000002a, 0x00040017, 0x0000002b, 0x0000002a, 0x00000003, 0x00040020, 0x0000002c,
    0x00000007, 0x0000002b, 0x00040017, 0x0000002e, 0x00000006, 0x00000003, 0x0004002b, 0x00000006,
    0x00000031, 0x3b4d2e1c, 0x0006002c, 0x0000002e, 0x00000032, 0x00000031, 0x00000031, 0x00000031,
    0x00040020, 0x00000034, 0x00000007, 0x0000002e, 0x0004002b, 0x00000006, 0x00000038, 0x414eb852,
    0x0004002b, 0x00000006, 0x0000003d, 0x3ed55555, 0x0006002c, 0x0000002e, 0x0000003e, 0x0000003d,
    0x0000003d, 0x0000003d, 0x0004002b, 0x00000006, 0x00000040, 0x3f870a3d, 0x0004002b, 0x00000006,
    0x00000042, 0x3d6147ae, 0x0004002b, 0x0000000a, 0x00000049, 0x00000000, 0x00040020, 0x0000004a,
    0x00000007, 0x00000006, 0x0004002b, 0x0000000a, 0x0000004d, 0x00000001, 0x0004002b, 0x0000000a,
    0x00000050, 0x00000002, 0x00090019, 0x00000053, 0x00000006, 0x00000001, 0x00000000, 0x00000000,
    0x00000000, 0x00000002, 0x00000000, 0x00040020, 0x00000054, 0x00000000, 0x00000053, 0x0004003b,
    0x00000054, 0x00000055, 0x00000000, 0x00040017, 0x00000059, 0x00000015, 0x00000002, 0x0004002b,
    0x0000000a, 0x0000005c, 0x00000008, 0x0006002c, 0x0000000b, 0x0000005d, 0x0000005c, 0x0000005c,
    0x0000004d, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005,
    0x0004003b, 0x00000008, 0x00000009, 0x00000007, 0x0004003b, 0x00000020, 0x00000021, 0x00000007,
    0x0004003b, 0x0000002c, 0x0000002d, 0x00000007, 0x0004003b, 0x00000034, 0x00000035, 0x00000007,
    0x0004003b, 0x00000034, 0x0000003a, 0x00000007, 0x0004003d, 0x0000000b, 0x0000000f, 0x0000000d,
    0x0007004f, 0x0000000e, 0x00000010, 0x0000000f, 0x0000000f, 0x00000000, 0x00000001, 0x00040070,
    0x00000007, 0x00000011, 0x00000010, 0x00050041, 0x00000017, 0x00000018, 0x00000014, 0x00000016,
    0x0004003d, 0x00000007, 0x00000019, 0x00000018, 0x00050083, 0x00000007, 0x0000001a, 0x00000011,
    0x00000019, 0x00050041, 0x00000017, 0x0000001c, 0x00000014, 0x0000001b, 0x0004003d, 0x00000007,
    0x0000001d, 0x0000001c, 0x00050088, 0x00000007, 0x0000001e, 0x0000001a, 0x0000001d, 0x0003003e,
    0x00000009, 0x0000001e, 0x0004003d, 0x00000023, 0x00000026, 0x00000025, 0x0004003d, 0x00000007,
    0x00000027, 0x00000009, 0x00070058, 0x0000001f, 0x00000029, 0x00000026, 0x00000027, 0x00000002,
    0x00000028, 0x0003003e, 0x00000021, 0x00000029, 0x0004003d, 0x0000001f, 0x0000002f, 0x00000021,
    0x0008004f, 0x0000002e, 0x00000030, 0x0000002f, 0x0000002f, 0x00000000, 0x00000001, 0x00000002,
    0x000500bc, 0x0000002b, 0x00000033, 0x00000030, 0x00000032, 0x0003003e, 0x0000002d, 0x00000033,
    0x0004003d, 0x0000001f, 0x00000036, 0x00000021, 0x0008004f, 0x0000002e, 0x00000037, 0x00000036,
    0x00000036, 0x00000000, 0x00000001, 0x00000002, 0x0005008e, 0x0000002e, 0x00000039, 0x00000037,
    0x00000038, 0x0003003e, 0x00000035, 0x00000039, 0x0004003d, 0x0000001f, 0x0000003b, 0x00000021,
    0x0008004f, 0x0000002e, 0x0000003c, 0x0000003b, 0x0000003b, 0x00000000, 0x00000001, 0x00000002,
    0x0007000c, 0x0000002e, 0x0000003f, 0x00000001, 0x0000001a, 0x0000003c, 0x0000003e, 0x0005008e,
    0x0000002e, 0x00000041, 0x0000003f, 0x00000040, 0x00060050, 0x0000002e, 0x00000043, 0x00000042,
    0x00000042, 0x00000042, 0x00050083, 0x0000002e, 0x00000044, 0x00000041, 0x00000043, 0x0003003e,
    0x0000003a, 0x00000044, 0x0004003d, 0x0000002e, 0x00000045, 0x0000003a, 0x0004003d, 0x0000002e,
    0x00000046, 0x00000035, 0x0004003d, 0x0000002b, 0x00000047, 0x0000002d, 0x000600a9, 0x0000002e,
    0x00000048, 0x00000047, 0x00000046, 0x00000045, 0x00050041, 0x0000004a, 0x0000004b, 0x00000021,
    0x00000049, 0x00050051, 0x00000006, 0x0000004c, 0x00000048, 0x00000000, 0x0003003e, 0x0000004b,
    0x0000004c, 0x00050041, 0x0000004a, 0x0000004e, 0x00000021, 0x0000004d, 0x00050051, 0x00000006,
    0x0000004f, 0x00000048, 0x00000001, 0x0003003e, 0x0000004e, 0x0000004f, 0x00050041, 0x0000004a,
    0x00000051, 0x00000021, 0x00000050, 0x00050051, 0x00000006, 0x00000052, 0x00000048, 0x00000002,
    0x0003003e, 0x00000051, 0x00000052, 0x0004003d, 0x00000053, 0x00000056, 0x00000055, 0x0004003d,
    0x0000000b, 0x00000057, 0x0000000d, 0x0007004f, 0x0000000e, 0x00000058, 0x00000057, 0x00000057,
    0x00000000, 0x00000001, 0x0004007c, 0x00000059, 0x0000005a, 0x00000058, 0x0004003d, 0x0000001f,
    0x0000005b, 0x00000021, 0x00040063, 0x00000056, 0x0000005a, 0x0000005b, 0x000100fd, 0x00010038,
};

/* The blit reads offset and extents; the FSR passes read all of it, with
 * offset and extents as the whole-pixel rectangle the picture is shown in. */
struct fs_hack_constants
{
    float offset[2];
    float extents[2];
    float src_extents[2];
    float sharpness;
    float padding;
};

#include "fshack_fsr_spv.h"

static VkResult create_pipeline( struct vulkan_device *device, struct swapchain *swapchain, const uint32_t *code,
                                 size_t size, VkPipeline *pipeline )
{
    VkShaderModuleCreateInfo shaderInfo = {0};
    VkShaderModule shaderModule;
    VkComputePipelineCreateInfo pipelineInfo = {0};
    VkResult res;

    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = size;
    shaderInfo.pCode = code;
    if ((res = device->p_vkCreateShaderModule( device->host.device, &shaderInfo, NULL, &shaderModule )))
    {
        ERR( "vkCreateShaderModule: %d\n", res );
        return res;
    }

    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = swapchain->pipeline_layout;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    res = device->p_vkCreateComputePipelines( device->host.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, pipeline );
    if (res) ERR( "vkCreateComputePipelines: %d\n", res );
    device->p_vkDestroyShaderModule( device->host.device, shaderModule, NULL );
    return res;
}

static VkResult create_descriptor_set( struct vulkan_device *device, struct swapchain *swapchain, struct fs_hack_image *hack )
{
    VkDescriptorImageInfo userDescriptorImageInfo = {0}, realDescriptorImageInfo = {0};
    VkDescriptorSetAllocateInfo descriptorAllocInfo = {0};
    VkDescriptorImageInfo midDescriptorImageInfo = {0};
    VkWriteDescriptorSet descriptorWrites[3] = {{0}, {0}, {0}};
    VkResult res;

    descriptorAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorAllocInfo.descriptorPool = swapchain->descriptor_pool;
    descriptorAllocInfo.descriptorSetCount = 1;
    descriptorAllocInfo.pSetLayouts = &swapchain->descriptor_set_layout;

    if ((res = device->p_vkAllocateDescriptorSets( device->host.device, &descriptorAllocInfo, &hack->descriptor_set )))
    {
        ERR( "vkAllocateDescriptorSets: %d\n", res );
        return res;
    }

    userDescriptorImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    userDescriptorImageInfo.imageView = swapchain->fsr ? hack->gamma_view : hack->user_view;
    userDescriptorImageInfo.sampler = swapchain->sampler;

    realDescriptorImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    realDescriptorImageInfo.imageView = hack->blit_view;

    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].dstSet = hack->descriptor_set;
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].dstArrayElement = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pImageInfo = &userDescriptorImageInfo;

    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].dstSet = hack->descriptor_set;
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].dstArrayElement = 0;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].pImageInfo = &realDescriptorImageInfo;

    midDescriptorImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    midDescriptorImageInfo.imageView = hack->mid_view;

    descriptorWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[2].dstSet = hack->descriptor_set;
    descriptorWrites[2].dstBinding = 2;
    descriptorWrites[2].dstArrayElement = 0;
    descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    descriptorWrites[2].descriptorCount = 1;
    descriptorWrites[2].pImageInfo = &midDescriptorImageInfo;

    device->p_vkUpdateDescriptorSets( device->host.device, swapchain->fsr ? 3 : 2, descriptorWrites, 0, NULL );
    return VK_SUCCESS;
}

static VkResult init_blit_images( struct vulkan_device *device, struct swapchain *swapchain )
{
    VkResult res;
    VkSamplerCreateInfo samplerInfo = {0};
    VkDescriptorPoolSize poolSizes[2] = {{0}, {0}};
    VkDescriptorPoolCreateInfo poolInfo = {0};
    VkDescriptorSetLayoutBinding layoutBindings[3] = {{0}, {0}, {0}};
    VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo = {0};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {0};
    VkPushConstantRange pushConstants;
    VkImageViewCreateInfo viewInfo = {0};
    uint32_t i;

    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = samplerInfo.minFilter = fs_hack_is_integer() ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    /* The blit's bars are the border; EASU draws its own, and its taps past
     * the image's edge must see the edge, not black. */
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW =
        swapchain->fsr ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;

    if ((res = device->p_vkCreateSampler( device->host.device, &samplerInfo, NULL, &swapchain->sampler )))
    {
        WARN( "vkCreateSampler failed, res=%d\n", res );
        return res;
    }

    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = swapchain->n_images;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = swapchain->n_images * (swapchain->fsr ? 2 : 1);

    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = swapchain->n_images;

    if ((res = device->p_vkCreateDescriptorPool( device->host.device, &poolInfo, NULL, &swapchain->descriptor_pool )))
    {
        ERR( "vkCreateDescriptorPool: %d\n", res );
        goto fail;
    }

    layoutBindings[0].binding = 0;
    layoutBindings[0].descriptorCount = 1;
    layoutBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    layoutBindings[0].pImmutableSamplers = NULL;
    layoutBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    layoutBindings[1].binding = 1;
    layoutBindings[1].descriptorCount = 1;
    layoutBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    layoutBindings[1].pImmutableSamplers = NULL;
    layoutBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    layoutBindings[2] = layoutBindings[1];
    layoutBindings[2].binding = 2;

    descriptorLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorLayoutInfo.bindingCount = swapchain->fsr ? 3 : 2;
    descriptorLayoutInfo.pBindings = layoutBindings;

    if ((res = device->p_vkCreateDescriptorSetLayout( device->host.device, &descriptorLayoutInfo,
                                                      NULL, &swapchain->descriptor_set_layout )))
    {
        ERR( "vkCreateDescriptorSetLayout: %d\n", res );
        goto fail;
    }

    pushConstants.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstants.offset = 0;
    pushConstants.size = sizeof(struct fs_hack_constants);

    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &swapchain->descriptor_set_layout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstants;

    if ((res = device->p_vkCreatePipelineLayout( device->host.device, &pipelineLayoutInfo, NULL,
                                                 &swapchain->pipeline_layout )))
    {
        ERR( "vkCreatePipelineLayout: %d\n", res );
        goto fail;
    }

    if (swapchain->fsr)
    {
        if ((res = create_pipeline( device, swapchain, easu_comp_spv, sizeof(easu_comp_spv), &swapchain->pipeline )) ||
            (res = create_pipeline( device, swapchain, rcas_comp_spv, sizeof(rcas_comp_spv), &swapchain->rcas_pipeline )))
            goto fail;
    }
    else if ((res = create_pipeline( device, swapchain, blit_comp_spv, sizeof(blit_comp_spv), &swapchain->pipeline )))
        goto fail;

    for (i = 0; i < swapchain->n_images; ++i)
    {
        struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = hack->swapchain_image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if ((res = device->p_vkCreateImageView( device->host.device, &viewInfo, NULL, &hack->blit_view )))
        {
            ERR( "vkCreateImageView(blit): %d\n", res );
            goto fail;
        }

        if ((res = create_descriptor_set( device, swapchain, hack ))) goto fail;
    }

    return VK_SUCCESS;

fail:
    for (i = 0; i < swapchain->n_images; ++i)
    {
        struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

        device->p_vkDestroyImageView( device->host.device, hack->blit_view, NULL );
        hack->blit_view = VK_NULL_HANDLE;
    }

    device->p_vkDestroyPipeline( device->host.device, swapchain->pipeline, NULL );
    swapchain->pipeline = VK_NULL_HANDLE;
    device->p_vkDestroyPipeline( device->host.device, swapchain->rcas_pipeline, NULL );
    swapchain->rcas_pipeline = VK_NULL_HANDLE;

    device->p_vkDestroyPipelineLayout( device->host.device, swapchain->pipeline_layout, NULL );
    swapchain->pipeline_layout = VK_NULL_HANDLE;

    device->p_vkDestroyDescriptorSetLayout( device->host.device, swapchain->descriptor_set_layout, NULL );
    swapchain->descriptor_set_layout = VK_NULL_HANDLE;

    device->p_vkDestroyDescriptorPool( device->host.device, swapchain->descriptor_pool, NULL );
    swapchain->descriptor_pool = VK_NULL_HANDLE;

    device->p_vkDestroySampler( device->host.device, swapchain->sampler, NULL );
    swapchain->sampler = VK_NULL_HANDLE;

    return res;
}

static void destroy_fs_hack_image( struct vulkan_device *device, struct swapchain *swapchain, struct fs_hack_image *hack )
{
    device->p_vkDestroyImageView( device->host.device, hack->user_view, NULL );
    device->p_vkDestroyImageView( device->host.device, hack->blit_view, NULL );
    device->p_vkDestroyImageView( device->host.device, hack->gamma_view, NULL );
    device->p_vkDestroyImageView( device->host.device, hack->mid_view, NULL );
    device->p_vkDestroyImage( device->host.device, hack->user_image, NULL );
    device->p_vkDestroyImage( device->host.device, hack->mid_image, NULL );
    if (hack->cmd) device->p_vkFreeCommandBuffers( device->host.device, swapchain->cmd_pools[hack->cmd_queue_idx], 1, &hack->cmd );
    device->p_vkDestroySemaphore( device->host.device, hack->blit_finished, NULL );
}

static VkResult init_fs_hack_images( struct vulkan_device *device, struct swapchain *swapchain,
                                     const VkSwapchainCreateInfoKHR *createinfo )
{
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct vulkan_instance *instance = physical_device->instance;
    VkResult res;
    VkImage *real_images = NULL;
    VkDeviceSize userMemTotal = 0, offs;
    VkImageCreateInfo imageInfo = {0};
    VkSemaphoreCreateInfo semaphoreInfo = {0};
    VkMemoryRequirements userMemReq;
    VkMemoryAllocateInfo allocInfo = {0};
    VkPhysicalDeviceMemoryProperties memProperties;
    VkImageViewCreateInfo viewInfo = {0};
    uint32_t count, i = 0, user_memory_type = -1, type_bits = ~0u;

    if ((res = device->p_vkGetSwapchainImagesKHR( device->host.device, swapchain->obj.host.swapchain, &count, NULL )))
    {
        WARN( "vkGetSwapchainImagesKHR failed, res=%d\n", res );
        return res;
    }

    real_images = malloc( count * sizeof(VkImage) );
    swapchain->cmd_pools = calloc( device->queue_count, sizeof(VkCommandPool) );
    swapchain->fs_hack_images = calloc( count, sizeof(struct fs_hack_image) );
    if (!real_images || !swapchain->cmd_pools || !swapchain->fs_hack_images) goto fail;

    if ((res = device->p_vkGetSwapchainImagesKHR( device->host.device, swapchain->obj.host.swapchain, &count, real_images )))
    {
        WARN( "vkGetSwapchainImagesKHR failed, res=%d\n", res );
        goto fail;
    }

    /* create user images */
    for (i = 0; i < count; ++i)
    {
        struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

        hack->swapchain_image = real_images[i];

        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if ((res = device->p_vkCreateSemaphore( device->host.device, &semaphoreInfo, NULL, &hack->blit_finished )))
        {
            WARN( "vkCreateSemaphore failed, res=%d\n", res );
            goto fail;
        }

        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = swapchain->extents.width;
        imageInfo.extent.height = swapchain->extents.height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = createinfo->imageArrayLayers;
        imageInfo.format = createinfo->imageFormat;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = createinfo->imageUsage | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = createinfo->imageSharingMode;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.queueFamilyIndexCount = createinfo->queueFamilyIndexCount;
        imageInfo.pQueueFamilyIndices = createinfo->pQueueFamilyIndices;

        if (createinfo->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)
            imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        else if (createinfo->imageFormat != VK_FORMAT_B8G8R8A8_SRGB || swapchain->fsr)
            imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

        if ((res = device->p_vkCreateImage( device->host.device, &imageInfo, NULL, &hack->user_image )))
        {
            ERR( "vkCreateImage failed: %d\n", res );
            goto fail;
        }

        device->p_vkGetImageMemoryRequirements( device->host.device, hack->user_image, &userMemReq );

        offs = userMemTotal % userMemReq.alignment;
        if (offs) userMemTotal += userMemReq.alignment - offs;

        userMemTotal += userMemReq.size;
        type_bits &= userMemReq.memoryTypeBits;

        swapchain->n_images++;

        /* What EASU writes and RCAS reads, the size of the screen buffer: one
         * per image, so frames in flight do not share it. */
        if (swapchain->fsr)
        {
            VkImageCreateInfo midInfo = {0};

            midInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            midInfo.imageType = VK_IMAGE_TYPE_2D;
            midInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            midInfo.extent.width = swapchain->host_extents.width;
            midInfo.extent.height = swapchain->host_extents.height;
            midInfo.extent.depth = 1;
            midInfo.mipLevels = 1;
            midInfo.arrayLayers = 1;
            midInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            midInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            midInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT;
            midInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            midInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if ((res = device->p_vkCreateImage( device->host.device, &midInfo, NULL, &hack->mid_image )))
            {
                ERR( "vkCreateImage(mid) failed: %d\n", res );
                goto fail;
            }
            device->p_vkGetImageMemoryRequirements( device->host.device, hack->mid_image, &userMemReq );
            offs = userMemTotal % userMemReq.alignment;
            if (offs) userMemTotal += userMemReq.alignment - offs;
            userMemTotal += userMemReq.size;
            type_bits &= userMemReq.memoryTypeBits;
        }
    }

    /* allocate backing memory */
    instance->p_vkGetPhysicalDeviceMemoryProperties( physical_device->host.physical_device, &memProperties );

    for (i = 0; i < memProperties.memoryTypeCount; i++)
    {
        UINT flag = memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        if (flag == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        {
            if (type_bits & (1 << i))
            {
                user_memory_type = i;
                break;
            }
        }
    }

    if (user_memory_type == -1)
    {
        ERR( "unable to find suitable memory type\n" );
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto fail;
    }

    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = userMemTotal;
    allocInfo.memoryTypeIndex = user_memory_type;

    if ((res = device->p_vkAllocateMemory( device->host.device, &allocInfo, NULL, &swapchain->user_image_memory )))
    {
        ERR( "vkAllocateMemory: %d\n", res );
        goto fail;
    }

    /* bind backing memory and create imageviews */
    userMemTotal = 0;
    for (i = 0; i < count; ++i)
    {
        device->p_vkGetImageMemoryRequirements( device->host.device, swapchain->fs_hack_images[i].user_image, &userMemReq );

        offs = userMemTotal % userMemReq.alignment;
        if (offs) userMemTotal += userMemReq.alignment - offs;

        if ((res = device->p_vkBindImageMemory( device->host.device, swapchain->fs_hack_images[i].user_image,
                                                swapchain->user_image_memory, userMemTotal )))
        {
            ERR( "vkBindImageMemory: %d\n", res );
            goto fail;
        }

        userMemTotal += userMemReq.size;

        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapchain->fs_hack_images[i].user_image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_B8G8R8A8_SRGB;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if ((res = device->p_vkCreateImageView( device->host.device, &viewInfo, NULL,
                                                &swapchain->fs_hack_images[i].user_view )))
        {
            ERR( "vkCreateImageView(user): %d\n", res );
            goto fail;
        }
        if (!swapchain->fsr) continue;

        /* The same bytes without the sRGB decode: FSR filters perceptual values. */
        viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        if ((res = device->p_vkCreateImageView( device->host.device, &viewInfo, NULL,
                                                &swapchain->fs_hack_images[i].gamma_view )))
        {
            ERR( "vkCreateImageView(gamma): %d\n", res );
            goto fail;
        }

        device->p_vkGetImageMemoryRequirements( device->host.device, swapchain->fs_hack_images[i].mid_image, &userMemReq );
        offs = userMemTotal % userMemReq.alignment;
        if (offs) userMemTotal += userMemReq.alignment - offs;
        if ((res = device->p_vkBindImageMemory( device->host.device, swapchain->fs_hack_images[i].mid_image,
                                                swapchain->user_image_memory, userMemTotal )))
        {
            ERR( "vkBindImageMemory(mid): %d\n", res );
            goto fail;
        }
        userMemTotal += userMemReq.size;

        viewInfo.image = swapchain->fs_hack_images[i].mid_image;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        if ((res = device->p_vkCreateImageView( device->host.device, &viewInfo, NULL,
                                                &swapchain->fs_hack_images[i].mid_view )))
        {
            ERR( "vkCreateImageView(mid): %d\n", res );
            goto fail;
        }
    }

    free( real_images );

    return VK_SUCCESS;

fail:
    for (i = 0; i < swapchain->n_images; ++i) destroy_fs_hack_image( device, swapchain, &swapchain->fs_hack_images[i] );
    free( real_images );
    free( swapchain->cmd_pools );
    free( swapchain->fs_hack_images );
    return res;
}

static BOOL surface_get_fshack_dpi( struct surface *surface )
{
    UINT dpi = NtUserGetDpiForWindow( surface->hwnd ), raw = NtUserGetWinMonitorDpi( surface->hwnd, MDT_RAW_DPI );
    return fshack_enabled && dpi != raw ? raw : 0;
}

#ifdef __SWITCH__
/* The Switch screen shows buffers of its own size (1280x720 handheld), the only
 * size tico-dolphin gives it. A swapchain of another size, such as 800x600 for a
 * game in full screen, gets host images of the screen's size instead, and the
 * program's images are scaled into them by the fullscreen hack's compute blit.
 * Its host swapchain does not depend on DPI, so it never needs recreating. */
static BOOL nx_scaled_host_extent( const VkSurfaceCapabilitiesKHR *capabilities, const VkExtent2D *extent,
                                   VkExtent2D *host_extent )
{
    const VkExtent2D *screen = &capabilities->currentExtent;

    if (screen->width == 0xffffffff || !screen->width || !screen->height) return FALSE;
    if (screen->width == extent->width && screen->height == extent->height) return FALSE;
    *host_extent = *screen;
    return TRUE;
}
#endif

static BOOL swapchain_fshack_changed( struct swapchain *swapchain, struct surface *surface )
{
#ifdef __SWITCH__
    return FALSE;
#else
    return swapchain->fshack_dpi != surface_get_fshack_dpi( surface );
#endif
}

static VkResult win32u_vkCreateSwapchainKHR( VkDevice client_device, const VkSwapchainCreateInfoKHR *create_info,
                                             const VkAllocationCallbacks *allocator, VkSwapchainKHR *ret )
{
    VkSwapchainPresentScalingCreateInfoEXT scaling = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT};
    struct swapchain *swapchain, *old_swapchain = swapchain_from_handle( create_info->oldSwapchain );
    struct surface *surface = surface_from_handle( create_info->surface );
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct vulkan_instance *instance = physical_device->instance;
    VkSwapchainCreateInfoKHR create_info_host = *create_info;
    VkSurfaceCapabilitiesKHR capabilities;
    VkSwapchainKHR host_swapchain;
#ifdef WINE_NX_LSFG
    BOOL lsfg;
#endif
#ifdef WINE_NX_MESA_SWITCH
    VkSwapchainPresentModesCreateInfoKHR *present_modes;
#endif
    RECT client_rect;
    VkResult res;

    if (!NtUserIsWindow( surface->hwnd ))
    {
        ERR( "surface %p, hwnd %p is invalid!\n", surface, surface->hwnd );
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (surface) create_info_host.surface = surface->obj.host.surface;
    if (old_swapchain) create_info_host.oldSwapchain = old_swapchain->obj.host.swapchain;

    /* Windows allows client rect to be empty, but host Vulkan often doesn't, adjust extents back to the host capabilities */
    res = instance->p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR( physical_device->host.physical_device, surface->obj.host.surface, &capabilities );
    if (res) return res;

    create_info_host.imageExtent.width = max( create_info_host.imageExtent.width, capabilities.minImageExtent.width );
    create_info_host.imageExtent.height = max( create_info_host.imageExtent.height, capabilities.minImageExtent.height );

    /* If the swapchain image size is not equal to the presentation size (e.g. because of DPI virtualization or
     * display mode change emulation), MoltenVK's vkQueuePresentKHR returns VK_SUBOPTIMAL_KHR.
     * Create the swapchain with VkSwapchainPresentScalingCreateInfoEXT to avoid this.
     */
    if (get_surface_rect( surface->hwnd, &client_rect, NtUserGetWinMonitorDpi( surface->hwnd, MDT_RAW_DPI ) ) &&
        !extents_equals( &create_info_host.imageExtent, &client_rect ) &&
        instance->extensions.has_VK_EXT_surface_maintenance1 &&
        physical_device->extensions.has_VK_KHR_swapchain_maintenance1)
    {
        scaling.scalingBehavior = VK_PRESENT_SCALING_STRETCH_BIT_EXT;
        create_info_host.pNext = &scaling;
    }

    if (!(swapchain = calloc( 1, sizeof(*swapchain) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;

#ifdef __SWITCH__
    if (nx_scaled_host_extent( &capabilities, &create_info_host.imageExtent, &swapchain->host_extents ))
    {
        swapchain->fshack_dpi = 1;
        create_info_host.imageExtent = swapchain->host_extents;
        create_info_host.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
        /* Transfer source for the [NXVK] read-back of the scaled frame. */
        create_info_host.imageUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        nx_vk_trace( "[NXVK] swapchain %ux%u format %d scaled into %ux%u screen buffers (%s)",
                     create_info->imageExtent.width, create_info->imageExtent.height, create_info->imageFormat,
                     swapchain->host_extents.width, swapchain->host_extents.height,
                     create_info->presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo" );
    }
    else
#endif
    if ((swapchain->fshack_dpi = surface_get_fshack_dpi( surface )))
    {
        VkSurfaceCapabilitiesKHR caps = {0};

        if ((res = instance->p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR( physical_device->host.physical_device,
                                                                          create_info_host.surface, &caps )))
        {
            TRACE( "vkGetPhysicalDeviceSurfaceCapabilities failed, res=%d\n", res );
            free( swapchain );
            return res;
        }

        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT))
            FIXME( "Swapchain does not support required VK_IMAGE_USAGE_STORAGE_BIT\n" );

        swapchain->host_extents = capabilities.minImageExtent;
        create_info_host.imageExtent = capabilities.minImageExtent;
        create_info_host.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
        create_info_host.imageUsage = VK_IMAGE_USAGE_STORAGE_BIT;

        if (create_info->imageFormat != VK_FORMAT_B8G8R8A8_UNORM && create_info->imageFormat != VK_FORMAT_B8G8R8A8_SRGB)
            FIXME( "swapchain image format is not BGRA8 UNORM/SRGB. Things may go badly. %d\n",
                   create_info_host.imageFormat );
    }

#ifdef WINE_NX_MESA_SWITCH
    create_info_host.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (nx_present_mode != VK_PRESENT_MODE_FIFO_KHR)
    {
        VkPresentModeKHR *modes;
        uint32_t count = 0;

        if (!instance->p_vkGetPhysicalDeviceSurfacePresentModesKHR( physical_device->host.physical_device,
              create_info_host.surface, &count, NULL ) && count && (modes = malloc( count * sizeof(*modes) )))
        {
            if (!instance->p_vkGetPhysicalDeviceSurfacePresentModesKHR( physical_device->host.physical_device,
                  create_info_host.surface, &count, modes ))
                for (uint32_t i = 0; i < count; ++i)
                    if (modes[i] == nx_present_mode) create_info_host.presentMode = nx_present_mode;
            free( modes );
        }
    }
#endif
#ifdef __SWITCH__
    /* The floating keyboard is copied into the images the screen shows. */
    swapchain->nx_format = create_info_host.imageFormat;
    swapchain->nx_extent = create_info_host.imageExtent;
    if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
        (swapchain->nx_format == VK_FORMAT_B8G8R8A8_UNORM || swapchain->nx_format == VK_FORMAT_B8G8R8A8_SRGB ||
         swapchain->nx_format == VK_FORMAT_R8G8B8A8_UNORM || swapchain->nx_format == VK_FORMAT_R8G8B8A8_SRGB))
    {
        create_info_host.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        swapchain->nx_osk_usable = TRUE;
    }
#endif
#ifdef WINE_NX_LSFG
    lsfg = wine_nx_lsfg_prepare_swapchain( instance->host.instance, physical_device->host.physical_device,
                                          &create_info_host );
#endif
#ifdef WINE_NX_MESA_SWITCH
    swapchain->nx_present_mode = create_info_host.presentMode;
    present_modes = (void *)find_next_struct( (const void *)&create_info_host, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_KHR );
    if (present_modes)
    {
        present_modes->presentModeCount = 1;
        present_modes->pPresentModes = &create_info_host.presentMode;
    }
#endif
    if ((res = device->p_vkCreateSwapchainKHR( device->host.device, &create_info_host, NULL, &host_swapchain )))
    {
        free( swapchain );
        return res;
    }

    vulkan_object_init( &swapchain->obj.obj, host_swapchain );
    swapchain->surface = surface;
    swapchain->extents = create_info->imageExtent;
    instance->p_insert_object( instance, &swapchain->obj.obj );

    if (swapchain->fshack_dpi)
    {
        /* EASU is made for enlarging; a game drawing more than the screen
         * holds is scaled down by the blit as before. */
        swapchain->fsr = nx_upscaling_mode == 1 && swapchain->host_extents.width >= swapchain->extents.width &&
                         swapchain->host_extents.height >= swapchain->extents.height;
        if ((res = init_fs_hack_images( device, swapchain, create_info )))
        {
            ERR( "creating fs hack images failed: %d\n", res );
            device->p_vkDestroySwapchainKHR( device->host.device, swapchain->obj.host.swapchain, NULL );
            free( swapchain );
            return res;
        }

        if ((res = init_blit_images( device, swapchain )))
        {
            ERR( "creating blit images failed: %d\n", res );
            device->p_vkDestroySwapchainKHR( device->host.device, swapchain->obj.host.swapchain, NULL );
            free( swapchain );
            return res;
        }

        WARN( "Enabled fullscreen hack on swapchain %p, scalind from %s -> %s\n", swapchain,
              debugstr_vkextent2d(&swapchain->extents), debugstr_vkextent2d(&swapchain->host_extents) );
    }

#ifdef WINE_NX_LSFG
    if (lsfg)
        swapchain->lsfg = wine_nx_lsfg_create( instance->host.instance, physical_device->host.physical_device,
                                             device->host.device, host_swapchain, &create_info_host );
#endif
    *ret = swapchain->obj.client.swapchain;
    return VK_SUCCESS;
}

void win32u_vkDestroySwapchainKHR( VkDevice client_device, VkSwapchainKHR client_swapchain,
                                   const VkAllocationCallbacks *allocator )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_instance *instance = device->physical_device->instance;
    struct swapchain *swapchain = swapchain_from_handle( client_swapchain );

    if (allocator) FIXME( "Support for allocation callbacks not implemented yet\n" );
    if (!swapchain) return;

#ifdef WINE_NX_LSFG
    wine_nx_lsfg_destroy( swapchain->lsfg );
#endif
    if (swapchain->fshack_dpi)
    {
        for (uint32_t i = 0; i < swapchain->n_images; ++i)
        {
            destroy_fs_hack_image( device, swapchain, &swapchain->fs_hack_images[i] );
        }
        for (uint32_t i = 0; i < device->queue_count; ++i)
        {
            if (!swapchain->cmd_pools[i]) continue;
            device->p_vkDestroyCommandPool( device->host.device, swapchain->cmd_pools[i], NULL );
        }

        device->p_vkDestroyPipeline( device->host.device, swapchain->pipeline, NULL );
        device->p_vkDestroyPipeline( device->host.device, swapchain->rcas_pipeline, NULL );
        device->p_vkDestroyPipelineLayout( device->host.device, swapchain->pipeline_layout, NULL );
        device->p_vkDestroyDescriptorSetLayout( device->host.device, swapchain->descriptor_set_layout, NULL );
        device->p_vkDestroyDescriptorPool( device->host.device, swapchain->descriptor_pool, NULL );
        device->p_vkDestroySampler( device->host.device, swapchain->sampler, NULL );
        device->p_vkFreeMemory( device->host.device, swapchain->user_image_memory, NULL );
        free( swapchain->cmd_pools );
        free( swapchain->fs_hack_images );
    }

#ifdef __SWITCH__
    if (swapchain->nx_osk)
    {
        device->p_vkDeviceWaitIdle( device->host.device );
        nx_osk_free( device, swapchain->nx_osk );
    }
#endif
    device->p_vkDestroySwapchainKHR( device->host.device, swapchain->obj.host.swapchain, NULL );
    instance->p_remove_object( instance, &swapchain->obj.obj );

    free( swapchain );
}

static VkResult win32u_vkAcquireNextImage2KHR( VkDevice client_device, const VkAcquireNextImageInfoKHR *acquire_info,
                                               uint32_t *image_index )
{
    struct vulkan_semaphore *semaphore = acquire_info->semaphore ? vulkan_semaphore_from_handle( acquire_info->semaphore ) : NULL;
    struct vulkan_fence *fence = acquire_info->fence ? vulkan_fence_from_handle( acquire_info->fence ) : NULL;
    struct swapchain *swapchain = swapchain_from_handle( acquire_info->swapchain );
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    VkAcquireNextImageInfoKHR acquire_info_host = *acquire_info;
    struct surface *surface = swapchain->surface;
    RECT client_rect;
    VkResult res;

    acquire_info_host.swapchain = swapchain->obj.host.swapchain;
    acquire_info_host.semaphore = semaphore ? semaphore->host.semaphore : 0;
    acquire_info_host.fence = fence ? fence->host.fence : 0;
#ifdef WINE_NX_LSFG
    if (swapchain->lsfg)
    {
        pthread_mutex_lock( &present_lock );
        ++swapchain->lsfg_acquires;
        pthread_mutex_unlock( &present_lock );
    }
#endif
    res = device->p_vkAcquireNextImage2KHR( device->host.device, &acquire_info_host, image_index );
#ifdef WINE_NX_LSFG
    if (swapchain->lsfg)
    {
        pthread_mutex_lock( &present_lock );
        --swapchain->lsfg_acquires;
        pthread_mutex_unlock( &present_lock );
    }
#endif

    if (!res && swapchain_fshack_changed( swapchain, surface ))
    {
        WARN( "window %p swapchain %p needs fullscreen hack VK_SUBOPTIMAL_KHR\n", surface->hwnd, swapchain );
        return VK_SUBOPTIMAL_KHR;
    }

    if (!res && get_surface_rect( surface->hwnd, &client_rect, NtUserGetDpiForWindow( surface->hwnd ) ) &&
        !extents_equals( &swapchain->extents, &client_rect ))
    {
        WARN( "Swapchain size %dx%d does not match client rect %s, returning VK_SUBOPTIMAL_KHR\n",
              swapchain->extents.width, swapchain->extents.height, wine_dbgstr_rect( &client_rect ) );
        return VK_SUBOPTIMAL_KHR;
    }

    return res;
}

static VkResult win32u_vkAcquireNextImageKHR( VkDevice client_device, VkSwapchainKHR client_swapchain, uint64_t timeout,
                                              VkSemaphore client_semaphore, VkFence client_fence, uint32_t *image_index )
{
    struct vulkan_semaphore *semaphore = client_semaphore ? vulkan_semaphore_from_handle( client_semaphore ) : NULL;
    struct vulkan_fence *fence = client_fence ? vulkan_fence_from_handle( client_fence ) : NULL;
    struct swapchain *swapchain = swapchain_from_handle( client_swapchain );
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct surface *surface = swapchain->surface;
    RECT client_rect;
    VkResult res;

#ifdef WINE_NX_LSFG
    if (swapchain->lsfg)
    {
        pthread_mutex_lock( &present_lock );
        ++swapchain->lsfg_acquires;
        pthread_mutex_unlock( &present_lock );
    }
#endif
    res = device->p_vkAcquireNextImageKHR( device->host.device, swapchain->obj.host.swapchain, timeout,
                                              semaphore ? semaphore->host.semaphore : 0, fence ? fence->host.fence : 0,
                                              image_index );
#ifdef WINE_NX_LSFG
    if (swapchain->lsfg)
    {
        pthread_mutex_lock( &present_lock );
        --swapchain->lsfg_acquires;
        pthread_mutex_unlock( &present_lock );
    }
#endif

    if (!res && swapchain_fshack_changed( swapchain, surface ))
    {
        WARN( "window %p swapchain %p needs fullscreen hack VK_SUBOPTIMAL_KHR\n", surface->hwnd, swapchain );
        return VK_SUBOPTIMAL_KHR;
    }

    if (!res && get_surface_rect( surface->hwnd, &client_rect, NtUserGetDpiForWindow( surface->hwnd ) ) &&
        !extents_equals( &swapchain->extents, &client_rect ))
    {
        WARN( "Swapchain size %dx%d does not match client rect %s, returning VK_SUBOPTIMAL_KHR\n",
              swapchain->extents.width, swapchain->extents.height, wine_dbgstr_rect( &client_rect ) );
        return VK_SUBOPTIMAL_KHR;
    }

    return res;
}

static VkResult win32u_vkGetSwapchainImagesKHR( VkDevice client_device, VkSwapchainKHR client_swapchain,
                                                uint32_t *count, VkImage *images )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct swapchain *swapchain = swapchain_from_handle( client_swapchain );
    uint32_t i;

    if (images && swapchain->fshack_dpi)
    {
        if (*count > swapchain->n_images) *count = swapchain->n_images;
        for (i = 0; i < *count; ++i) images[i] = swapchain->fs_hack_images[i].user_image;
        return *count == swapchain->n_images ? VK_SUCCESS : VK_INCOMPLETE;
    }

    return device->p_vkGetSwapchainImagesKHR( device->host.device, swapchain->obj.host.swapchain, count, images );
}

static VkCommandBuffer create_hack_cmd( struct vulkan_queue *queue, struct swapchain *swapchain, uint32_t queue_idx )
{
    VkCommandBufferAllocateInfo allocInfo = {0};
    VkCommandBuffer cmd;
    VkResult res;

    if (!swapchain->cmd_pools[queue_idx])
    {
        VkCommandPoolCreateInfo poolInfo = {0};

        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = queue_idx;

        if ((res = queue->device->p_vkCreateCommandPool( queue->device->host.device, &poolInfo, NULL,
                                                         &swapchain->cmd_pools[queue_idx] )))
        {
            ERR( "vkCreateCommandPool failed, res=%d\n", res );
            return NULL;
        }
    }

    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = swapchain->cmd_pools[queue_idx];
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    if ((res = queue->device->p_vkAllocateCommandBuffers( queue->device->host.device, &allocInfo, &cmd )))
    {
        ERR( "vkAllocateCommandBuffers failed, res=%d\n", res );
        return NULL;
    }

    return cmd;
}

static VkResult record_compute_cmd( struct vulkan_device *device, struct swapchain *swapchain, struct fs_hack_image *hack )
{
    VkResult res;
    VkImageMemoryBarrier barriers[3] = {{0}};
    VkCommandBufferBeginInfo beginInfo = {0};
    struct fs_hack_constants constants = {0};

    TRACE( "recording compute command\n" );

    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;

    device->p_vkBeginCommandBuffer( hack->cmd, &beginInfo );

    /* for the cs we run... */
    /* transition user image from PRESENT_SRC to SHADER_READ */
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image = hack->user_image;
    barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[0].subresourceRange.baseMipLevel = 0;
    barriers[0].subresourceRange.levelCount = 1;
    barriers[0].subresourceRange.baseArrayLayer = 0;
    barriers[0].subresourceRange.layerCount = 1;
    barriers[0].srcAccessMask = 0;
    barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    /* storage image... */
    /* transition swapchain image from whatever to GENERAL */
    barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].image = hack->swapchain_image;
    barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[1].subresourceRange.baseMipLevel = 0;
    barriers[1].subresourceRange.levelCount = 1;
    barriers[1].subresourceRange.baseArrayLayer = 0;
    barriers[1].subresourceRange.layerCount = 1;
    barriers[1].srcAccessMask = 0;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

    device->p_vkCmdPipelineBarrier( hack->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    0, 0, NULL, 0, NULL, 2, barriers );

    if (swapchain->fsr)
    {
        /* the intermediate image: its old contents are not needed */
        barriers[2].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[2].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[2].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[2].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[2].image = hack->mid_image;
        barriers[2].subresourceRange = barriers[1].subresourceRange;
        barriers[2].srcAccessMask = 0;
        barriers[2].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        device->p_vkCmdPipelineBarrier( hack->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barriers[2] );
    }

    /* perform blit shader */
    device->p_vkCmdBindPipeline( hack->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, swapchain->pipeline );

    device->p_vkCmdBindDescriptorSets( hack->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                       swapchain->pipeline_layout, 0, 1, &hack->descriptor_set, 0, NULL );

#ifdef __SWITCH__
    {
        /* Keep the aspect ratio: the image is centred, and the sampler's black
         * border fills the bars. texcoord = (id + 0.5 - offset) / extents.
         * Integer scaling takes the largest whole multiple that fits, and FSR
         * a rectangle of whole pixels, both on whole-pixel bars. */
        float scale = min( (float)swapchain->host_extents.width / swapchain->extents.width,
                           (float)swapchain->host_extents.height / swapchain->extents.height );
        float width, height;

        if (!swapchain->fsr && fs_hack_is_integer() && scale >= 1.0f) scale = floorf( scale );
        width = swapchain->extents.width * scale;
        height = swapchain->extents.height * scale;
        if (swapchain->fsr || fs_hack_is_integer())
        {
            width = min( floorf( width + 0.5f ), swapchain->host_extents.width );
            height = min( floorf( height + 0.5f ), swapchain->host_extents.height );
            constants.offset[0] = floorf( (swapchain->host_extents.width - width) / 2 );
            constants.offset[1] = floorf( (swapchain->host_extents.height - height) / 2 );
        }
        else
        {
            constants.offset[0] = (swapchain->host_extents.width - width) / 2;
            constants.offset[1] = (swapchain->host_extents.height - height) / 2;
        }
        if (!swapchain->fsr)
        {
            constants.offset[0] -= 0.5f;
            constants.offset[1] -= 0.5f;
        }
        constants.extents[0] = width;
        constants.extents[1] = height;
    }
#else
    if (swapchain->fsr)
    {
        constants.offset[0] = constants.offset[1] = 0;
    }
    else
    {
        /* vec2: blit dst offset in real coords */
        /* offset by 0.5f because sampling is relative to pixel center */
        constants.offset[0] = -0.5f * swapchain->host_extents.width / swapchain->extents.width;
        constants.offset[1] = -0.5f * swapchain->host_extents.height / swapchain->extents.height;
    }

    /* vec2: blit dst extents in real coords */
    constants.extents[0] = swapchain->host_extents.width;
    constants.extents[1] = swapchain->host_extents.height;
#endif
    constants.src_extents[0] = swapchain->extents.width;
    constants.src_extents[1] = swapchain->extents.height;
    /* RCAS counts sharpness in stops down from its strongest: 100% is 0 stops,
     * 20% is 1.6. 0% leaves EASU's result as it is. */
    constants.sharpness = nx_upscaling_sharpness > 0 ? exp2f( -2.0f * (1.0f - nx_upscaling_sharpness) ) : 0;
    device->p_vkCmdPushConstants( hack->cmd, swapchain->pipeline_layout,
                                  VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants );

    /* local sizes in shader are 8 */
    device->p_vkCmdDispatch( hack->cmd, ceil( swapchain->host_extents.width / 8. ),
                             ceil( swapchain->host_extents.height / 8. ), 1 );

    if (swapchain->fsr)
    {
        /* EASU's writes, before RCAS reads them */
        barriers[2].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[2].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[2].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        device->p_vkCmdPipelineBarrier( hack->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barriers[2] );
        device->p_vkCmdBindPipeline( hack->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, swapchain->rcas_pipeline );
        device->p_vkCmdDispatch( hack->cmd, ceil( swapchain->host_extents.width / 8. ),
                                 ceil( swapchain->host_extents.height / 8. ), 1 );
    }

    /* transition user image from SHADER_READ back to PRESENT_SRC */
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image = hack->user_image;
    barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[0].subresourceRange.baseMipLevel = 0;
    barriers[0].subresourceRange.levelCount = 1;
    barriers[0].subresourceRange.baseArrayLayer = 0;
    barriers[0].subresourceRange.layerCount = 1;
    barriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[0].dstAccessMask = 0;

    /* transition swapchain image from GENERAL to PRESENT_SRC */
    barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].image = hack->swapchain_image;
    barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[1].subresourceRange.baseMipLevel = 0;
    barriers[1].subresourceRange.levelCount = 1;
    barriers[1].subresourceRange.baseArrayLayer = 0;
    barriers[1].subresourceRange.layerCount = 1;
    barriers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[1].dstAccessMask = 0;

    device->p_vkCmdPipelineBarrier( hack->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                    0, 0, NULL, 0, NULL, 2, barriers );

    if ((res = device->p_vkEndCommandBuffer( hack->cmd )))
    {
        ERR( "vkEndCommandBuffer: %d\n", res );
        return res;
    }

    return VK_SUCCESS;
}

#ifdef __SWITCH__
/* Counted in the runtime's [PROGRESS] frames. */
unsigned int wine_nx_vk_presents;
static LONG nx_present_logs;

static void nx_vk_note_present( const VkSwapchainKHR *client_swapchains, uint32_t count, VkResult res )
{
    const unsigned int presents = __atomic_add_fetch( &wine_nx_vk_presents, 1, __ATOMIC_RELAXED );
    struct swapchain *swapchain;
    RECT client_rect = {0};
    HWND hwnd;

    if (res == VK_SUCCESS && presents > 4 && presents % 1800) return;
    if (res != VK_SUCCESS && __atomic_add_fetch( &nx_present_logs, 1, __ATOMIC_RELAXED ) > 32) return;
    if (!count) return;
    swapchain = swapchain_from_handle( client_swapchains[0] );
    hwnd = swapchain->surface->hwnd;
    get_surface_rect( hwnd, &client_rect, NtUserGetDpiForWindow( hwnd ) );
    nx_vk_trace( "[NXVK] present %u: result %d, swapchain %ux%u, hwnd %p client %s, style %#x%s, foreground %p",
                 presents, res, swapchain->extents.width, swapchain->extents.height, hwnd,
                 wine_dbgstr_rect( &client_rect ), (int)get_window_long( hwnd, GWL_STYLE ),
                 is_iconic( hwnd ) ? " (minimized)" : "", NtUserGetForegroundWindow() );
}

static LONG nx_scaled_presents;

/* Submits the scaling blit. On a few presents it also copies the scaled screen
 * buffer back and logs what reaches the screen: the centre, a bar pixel, and
 * how many samples are not black or are white (pe32-d3d9's triangle). */
static void nx_submit_scaled( struct vulkan_queue *queue, VkSubmitInfo *submit_info, struct swapchain *swapchain,
                              struct fs_hack_image *hack, struct mempool *pool )
{
    const LONG presents = __atomic_add_fetch( &nx_scaled_presents, 1, __ATOMIC_RELAXED );
    struct vulkan_device *device = queue->device;
    const VkExtent2D extent = swapchain->host_extents;
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    VkMemoryAllocateInfo alloc_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkCommandBufferAllocateInfo cmd_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    VkBufferImageCopy copy = {0};
    VkPhysicalDeviceMemoryProperties properties;
    VkMemoryRequirements requirements;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE, *cmds;
    VkFence fence = VK_NULL_HANDLE;
    unsigned int x, y, samples = 0, lit = 0, white = 0;
    const DWORD *pixels;
    VkResult res;
    void *map;

    if (presents != 30 && presents != 90 && presents % 1800)
    {
        device->p_vkQueueSubmit( queue->host.queue, 1, submit_info, VK_NULL_HANDLE );
        return;
    }

    buffer_info.size = (VkDeviceSize)extent.width * extent.height * 4;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if ((res = device->p_vkCreateBuffer( device->host.device, &buffer_info, NULL, &buffer ))) goto failed;
    device->p_vkGetBufferMemoryRequirements( device->host.device, buffer, &requirements );
    device->physical_device->instance->p_vkGetPhysicalDeviceMemoryProperties( device->physical_device->host.physical_device,
                                                                             &properties );
    for (alloc_info.memoryTypeIndex = 0; alloc_info.memoryTypeIndex < properties.memoryTypeCount; alloc_info.memoryTypeIndex++)
        if ((requirements.memoryTypeBits & (1u << alloc_info.memoryTypeIndex)) &&
            (properties.memoryTypes[alloc_info.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            break;
    alloc_info.allocationSize = requirements.size;
    if ((res = device->p_vkAllocateMemory( device->host.device, &alloc_info, NULL, &memory ))) goto failed;
    if ((res = device->p_vkBindBufferMemory( device->host.device, buffer, memory, 0 ))) goto failed;

    cmd_info.commandPool = swapchain->cmd_pools[hack->cmd_queue_idx];
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    if ((res = device->p_vkAllocateCommandBuffers( device->host.device, &cmd_info, &cmd ))) goto failed;
    device->p_vkBeginCommandBuffer( cmd, &begin_info );
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = hack->swapchain_image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    device->p_vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    0, 0, NULL, 0, NULL, 1, &barrier );
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = extent.width;
    copy.imageExtent.height = extent.height;
    copy.imageExtent.depth = 1;
    device->p_vkCmdCopyImageToBuffer( cmd, hack->swapchain_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy );
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    device->p_vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                    0, 0, NULL, 0, NULL, 1, &barrier );
    if ((res = device->p_vkEndCommandBuffer( cmd ))) goto failed;
    if ((res = device->p_vkCreateFence( device->host.device, &fence_info, NULL, &fence ))) goto failed;

    if (!(cmds = mem_alloc( pool, (submit_info->commandBufferCount + 1) * sizeof(*cmds) ))) { res = VK_ERROR_OUT_OF_HOST_MEMORY; goto failed; }
    memcpy( cmds, submit_info->pCommandBuffers, submit_info->commandBufferCount * sizeof(*cmds) );
    cmds[submit_info->commandBufferCount] = cmd;
    submit_info->pCommandBuffers = cmds;
    submit_info->commandBufferCount++;
    if ((res = device->p_vkQueueSubmit( queue->host.queue, 1, submit_info, fence ))) goto done;
    if ((res = device->p_vkWaitForFences( device->host.device, 1, &fence, VK_TRUE, 5000000000ull )))
    {
        /* Still queued: leave its objects alive rather than free them under the GPU. */
        nx_vk_trace( "[NXVK] scaled present %d read back failed: %d", (int)presents, res );
        return;
    }
    if ((res = device->p_vkMapMemory( device->host.device, memory, 0, VK_WHOLE_SIZE, 0, &map ))) goto done;

    pixels = map;
    for (y = 0; y < extent.height; y += 8)
        for (x = 0; x < extent.width; x += 8)
        {
            const DWORD pixel = pixels[y * extent.width + x] & 0xffffff;
            samples++;
            if (pixel) lit++;
            if ((pixel & 0xf0f0f0) == 0xf0f0f0) white++;
        }
    nx_vk_trace( "[NXVK] scaled present %d read back %ux%u: centre %06x, left bar %06x, %u of %u samples lit, %u white",
                 (int)presents, extent.width, extent.height,
                 (unsigned int)(pixels[extent.height / 2 * extent.width + extent.width / 2] & 0xffffff),
                 (unsigned int)(pixels[extent.height / 2 * extent.width + 4] & 0xffffff), lit, samples, white );
    device->p_vkUnmapMemory( device->host.device, memory );
    goto done;

failed:
    device->p_vkQueueSubmit( queue->host.queue, 1, submit_info, VK_NULL_HANDLE );
done:
    if (res) nx_vk_trace( "[NXVK] scaled present %d read back failed: %d", (int)presents, res );
    if (fence) device->p_vkDestroyFence( device->host.device, fence, NULL );
    if (cmd) device->p_vkFreeCommandBuffers( device->host.device, swapchain->cmd_pools[hack->cmd_queue_idx], 1, &cmd );
    if (buffer) device->p_vkDestroyBuffer( device->host.device, buffer, NULL );
    if (memory) device->p_vkFreeMemory( device->host.device, memory, NULL );
}
#endif

#ifdef __SWITCH__
/* The floating keyboard (wine-nx-probe/source/osk.c), copied into the image
 * about to be shown, on the present's own queue: after everything drawn into
 * it, before the presentation engine takes it. Each image has its own staging
 * buffer, command buffer and semaphore. An image only comes back to the
 * program once it has been shown, which is after the copy into it finished,
 * so its buffer is free to fill again by then. */
struct nx_osk_image
{
    VkCommandBuffer cmd;
    VkSemaphore done;
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *pixels;
    unsigned int generation;
};

struct nx_osk_overlay
{
    uint32_t count, family;
    VkImage *images;
    struct nx_osk_image *per;
    VkCommandPool pool;
    int x, y, width, height;   /* what the command buffers copy, and where */
    BOOL failed;
};

static void nx_osk_release( struct vulkan_device *device, struct nx_osk_overlay *osk )
{
    uint32_t i;

    for (i = 0; osk->per && i < osk->count; i++)
    {
        struct nx_osk_image *per = &osk->per[i];

        device->p_vkDestroySemaphore( device->host.device, per->done, NULL );
        device->p_vkDestroyBuffer( device->host.device, per->buffer, NULL );
        device->p_vkFreeMemory( device->host.device, per->memory, NULL );
    }
    if (osk->pool) device->p_vkDestroyCommandPool( device->host.device, osk->pool, NULL );
    osk->pool = VK_NULL_HANDLE;
    memset( osk->per, 0, osk->count * sizeof(*osk->per) );
    osk->width = osk->height = 0;
}

static void nx_osk_free( struct vulkan_device *device, struct nx_osk_overlay *osk )
{
    nx_osk_release( device, osk );
    free( osk->per );
    free( osk->images );
    free( osk );
}

static VkResult nx_osk_record( struct vulkan_device *device, struct nx_osk_overlay *osk, uint32_t i )
{
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    VkBufferImageCopy region = {0};
    VkCommandBuffer cmd = osk->per[i].cmd;

    begin.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    device->p_vkBeginCommandBuffer( cmd, &begin );
    /* Whatever wrote the image before, in this queue or waited for, is done:
     * the program's frame stays, the keyboard goes over it. */
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = osk->images[i];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = barrier.subresourceRange.layerCount = 1;
    device->p_vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    0, 0, NULL, 0, NULL, 1, &barrier );
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset.x = osk->x;
    region.imageOffset.y = osk->y;
    region.imageExtent.width = osk->width;
    region.imageExtent.height = osk->height;
    region.imageExtent.depth = 1;
    device->p_vkCmdCopyBufferToImage( cmd, osk->per[i].buffer, osk->images[i],
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    device->p_vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                    0, 0, NULL, 0, NULL, 1, &barrier );
    return device->p_vkEndCommandBuffer( cmd );
}

/* Buffers, command buffers and semaphores for a keyboard of that size there,
 * made again when it moves, grows or the queue family changes. */
static BOOL nx_osk_prepare( struct vulkan_device *device, struct swapchain *swapchain, uint32_t family,
                            const struct wine_nx_osk_frame *frame )
{
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct nx_osk_overlay *osk = swapchain->nx_osk;
    VkPhysicalDeviceMemoryProperties props;
    uint32_t i, t;

    if (!osk)
    {
        if (!(osk = swapchain->nx_osk = calloc( 1, sizeof(*osk) ))) return FALSE;
        if (device->p_vkGetSwapchainImagesKHR( device->host.device, swapchain->obj.host.swapchain, &osk->count, NULL ) ||
            !(osk->images = calloc( osk->count, sizeof(*osk->images) )) ||
            !(osk->per = calloc( osk->count, sizeof(*osk->per) )) ||
            device->p_vkGetSwapchainImagesKHR( device->host.device, swapchain->obj.host.swapchain, &osk->count, osk->images ))
            osk->failed = TRUE;
    }
    if (osk->failed) return FALSE;
    if (osk->pool && osk->family == family && osk->x == frame->x && osk->y == frame->y &&
        osk->width == frame->width && osk->height == frame->height)
        return TRUE;

    if (osk->pool) device->p_vkDeviceWaitIdle( device->host.device );
    nx_osk_release( device, osk );
    physical_device->instance->p_vkGetPhysicalDeviceMemoryProperties( physical_device->host.physical_device, &props );
    {
        VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};

        pool_info.queueFamilyIndex = family;
        if (device->p_vkCreateCommandPool( device->host.device, &pool_info, NULL, &osk->pool )) goto failed;
    }
    osk->family = family;
    osk->x = frame->x;
    osk->y = frame->y;
    osk->width = frame->width;
    osk->height = frame->height;
    for (i = 0; i < osk->count; i++)
    {
        struct nx_osk_image *per = &osk->per[i];
        VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        VkCommandBufferAllocateInfo cmd_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        VkSemaphoreCreateInfo semaphore_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkMemoryRequirements req;

        buffer_info.size = (VkDeviceSize)frame->width * frame->height * 4;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (device->p_vkCreateBuffer( device->host.device, &buffer_info, NULL, &per->buffer )) goto failed;
        device->p_vkGetBufferMemoryRequirements( device->host.device, per->buffer, &req );
        for (t = 0; t < props.memoryTypeCount; t++)
            if ((req.memoryTypeBits & (1u << t)) &&
                (props.memoryTypes[t].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                break;
        if (t == props.memoryTypeCount) goto failed;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = t;
        if (device->p_vkAllocateMemory( device->host.device, &alloc, NULL, &per->memory ) ||
            device->p_vkBindBufferMemory( device->host.device, per->buffer, per->memory, 0 ) ||
            device->p_vkMapMemory( device->host.device, per->memory, 0, buffer_info.size, 0, &per->pixels ) ||
            device->p_vkCreateSemaphore( device->host.device, &semaphore_info, NULL, &per->done ))
            goto failed;
        cmd_info.commandPool = osk->pool;
        cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_info.commandBufferCount = 1;
        if (device->p_vkAllocateCommandBuffers( device->host.device, &cmd_info, &per->cmd ) ||
            nx_osk_record( device, osk, i ))
            goto failed;
    }
    nx_vk_trace( "[NXVK] floating keyboard %dx%d at %d,%d on %u images", frame->width, frame->height,
                 frame->x, frame->y, osk->count );
    return TRUE;

failed:
    ERR( "could not set up the floating keyboard's copy\n" );
    nx_vk_trace( "[NXVK] floating keyboard: could not set up its copy into the swapchain images" );
    nx_osk_release( device, osk );
    osk->failed = TRUE;
    return FALSE;
}

/* The keyboard over the image this present shows, when it is up. The present
 * then waits for the copy, which waits for what the present waited for. */
static void nx_osk_present( struct vulkan_queue *queue, VkPresentInfoKHR *present_info,
                            const VkSwapchainKHR *client_swapchains, struct mempool *pool )
{
    struct vulkan_device *device = queue->device;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
    struct wine_nx_osk_frame frame;
    struct nx_osk_image *per;
    struct swapchain *swapchain;
    VkPipelineStageFlags *stages;
    uint32_t index, i;

    if (present_info->swapchainCount != 1) return;
    swapchain = swapchain_from_handle( client_swapchains[0] );
    if (!swapchain->nx_osk_usable) return;
    if (!wine_nx_osk_frame( swapchain->nx_extent.width, swapchain->nx_extent.height, &frame )) return;
    if (frame.x < 0 || frame.y < 0 || frame.x + frame.width > (int)swapchain->nx_extent.width ||
        frame.y + frame.height > (int)swapchain->nx_extent.height)
        return;
    if (!nx_osk_prepare( device, swapchain, queue->info.queueFamilyIndex, &frame )) return;
    index = present_info->pImageIndices[0];
    if (index >= swapchain->nx_osk->count) return;
    per = &swapchain->nx_osk->per[index];
    if (per->generation != frame.generation &&
        !(per->generation = wine_nx_osk_copy( swapchain->nx_extent.width, swapchain->nx_extent.height, per->pixels,
                                              frame.width * 4, swapchain->nx_format == VK_FORMAT_R8G8B8A8_UNORM ||
                                                               swapchain->nx_format == VK_FORMAT_R8G8B8A8_SRGB )))
        return;  /* hidden meanwhile */

    if (!(stages = mem_alloc( pool, sizeof(*stages) * (present_info->waitSemaphoreCount + 1) ))) return;
    for (i = 0; i < present_info->waitSemaphoreCount; i++) stages[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    submit.waitSemaphoreCount = present_info->waitSemaphoreCount;
    submit.pWaitSemaphores = present_info->pWaitSemaphores;
    submit.pWaitDstStageMask = stages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &per->cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &per->done;
    if (device->p_vkQueueSubmit( queue->host.queue, 1, &submit, VK_NULL_HANDLE )) return;
    present_info->waitSemaphoreCount = 1;
    present_info->pWaitSemaphores = &per->done;
}
#endif

static VkResult win32u_vkQueuePresentKHR( VkQueue client_queue, const VkPresentInfoKHR *client_present_info )
{
    VkPresentInfoKHR *present_info = (VkPresentInfoKHR *)client_present_info; /* cast away const, it has been copied in the thunks */
    struct vulkan_queue *queue = vulkan_queue_from_handle( client_queue );
    struct vulkan_device *device = queue->device;
    VkResult res = VK_ERROR_OUT_OF_HOST_MEMORY;
    const VkSwapchainKHR *client_swapchains;
    VkSwapchainKHR *swapchains;
    VkCommandBuffer *blit_cmds;
    struct mempool pool = {0};
    uint32_t blit_count = 0;
    VkSemaphore blit_sema;
#ifdef __SWITCH__
    struct swapchain *blit_swapchain = NULL;
    struct fs_hack_image *blit_hack = NULL;
#endif

    TRACE( "queue %p, present_info %p\n", queue, present_info );

    if (!(swapchains = mem_alloc( &pool, present_info->swapchainCount * sizeof(*swapchains) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!(blit_cmds = mem_alloc( &pool, present_info->swapchainCount * sizeof(blit_cmds) ))) goto failed;

    for (uint32_t i = 0; i < present_info->swapchainCount; ++i)
    {
        struct swapchain *swapchain = swapchain_from_handle( present_info->pSwapchains[i] );
        struct fs_hack_image *hack = &swapchain->fs_hack_images[present_info->pImageIndices[i]];

        if (!swapchain->fshack_dpi) continue;
        blit_sema = hack->blit_finished;
#ifdef __SWITCH__
        blit_swapchain = swapchain;
        blit_hack = hack;
#endif

        if (!hack->cmd || hack->cmd_queue_idx != queue->info.queueFamilyIndex)
        {
            if (hack->cmd) device->p_vkFreeCommandBuffers( queue->device->host.device, swapchain->cmd_pools[hack->cmd_queue_idx], 1, &hack->cmd );
            if (!(queue->device->queue_props[queue->info.queueFamilyIndex].queueFlags & VK_QUEUE_COMPUTE_BIT)) goto failed; /* TODO */

            if (!(hack->cmd = create_hack_cmd( queue, swapchain, queue->info.queueFamilyIndex )) || record_compute_cmd( queue->device, swapchain, hack ))
            {
                device->p_vkFreeCommandBuffers( queue->device->host.device, swapchain->cmd_pools[hack->cmd_queue_idx], 1, &hack->cmd );
                hack->cmd = NULL;
                goto failed;
            }

            hack->cmd_queue_idx = queue->info.queueFamilyIndex;
        }

        blit_cmds[blit_count++] = hack->cmd;
    }

    for (uint32_t i = 0; i < present_info->waitSemaphoreCount; i++)
    {
        VkSemaphore *semaphores = (VkSemaphore *)present_info->pWaitSemaphores; /* cast away const, it has been copied in the thunks */
        struct vulkan_semaphore *semaphore = vulkan_semaphore_from_handle( semaphores[i] );
        semaphores[i] = semaphore->host.semaphore;
    }

    for (uint32_t i = 0; i < present_info->swapchainCount; i++)
    {
        struct swapchain *swapchain = swapchain_from_handle( present_info->pSwapchains[i] );
        swapchains[i] = swapchain->obj.host.swapchain;
    }

    client_swapchains = present_info->pSwapchains;
    present_info->pSwapchains = swapchains;

    for (uint32_t i = 0; i < present_info->swapchainCount; i++)
    {
        struct swapchain *swapchain = swapchain_from_handle( client_swapchains[i] );
        struct surface *surface = swapchain->surface;
        client_surface_update( surface->client );
    }

    if (blit_count)
    {
        VkSubmitInfo submit_info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
        VkPipelineStageFlags *stages;

        if (!(stages = mem_alloc( &pool, sizeof(VkPipelineStageFlags) * present_info->waitSemaphoreCount ))) goto failed;
        for (uint32_t i = 0; i < present_info->waitSemaphoreCount; ++i) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

        /* blit user image to real image */
        submit_info.waitSemaphoreCount = present_info->waitSemaphoreCount;
        submit_info.pWaitSemaphores = present_info->pWaitSemaphores;
        submit_info.pWaitDstStageMask = stages;
        submit_info.commandBufferCount = blit_count;
        submit_info.pCommandBuffers = blit_cmds;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &blit_sema;
#ifdef __SWITCH__
        nx_submit_scaled( queue, &submit_info, blit_swapchain, blit_hack, &pool );
#else
        device->p_vkQueueSubmit( queue->host.queue, 1, &submit_info, VK_NULL_HANDLE );
#endif

        present_info->waitSemaphoreCount = 1;
        present_info->pWaitSemaphores = &blit_sema;
    }

    pthread_mutex_lock( &present_lock );
#ifdef __SWITCH__
    nx_osk_present( queue, present_info, client_swapchains, &pool );
#endif
#ifdef WINE_NX_MESA_SWITCH
    if (present_info->swapchainCount)
    {
        VkSwapchainPresentModeInfoKHR *modes;
        VkPresentModeKHR *values;

        nx_pace_present( &swapchain_from_handle( client_swapchains[0] )->nx_last_present );
        modes = (void *)find_next_struct( (const void *)present_info, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR );
        if (modes)
        {
            if (!(values = mem_alloc( &pool, present_info->swapchainCount * sizeof(*values) )))
            {
                pthread_mutex_unlock( &present_lock );
                goto failed;
            }
            for (uint32_t i = 0; i < present_info->swapchainCount; ++i)
                values[i] = swapchain_from_handle( client_swapchains[i] )->nx_present_mode;
            modes->pPresentModes = values;
        }
    }
#endif
#ifdef WINE_NX_LSFG
    if (!present_info->swapchainCount ||
        !wine_nx_lsfg_present( swapchain_from_handle( client_swapchains[0] )->lsfg,
                               queue->host.queue, queue->info.queueFamilyIndex,
                               !swapchain_from_handle( client_swapchains[0] )->lsfg_acquires, present_info, &res ))
#endif
    res = device->p_vkQueuePresentKHR( queue->host.queue, present_info );
    pthread_mutex_unlock( &present_lock );

    for (uint32_t i = 0; i < present_info->swapchainCount; i++)
    {
        struct swapchain *swapchain = swapchain_from_handle( client_swapchains[i] );
        VkResult swapchain_res = present_info->pResults ? present_info->pResults[i] : res;
        struct surface *surface = swapchain->surface;
        RECT client_rect;

        client_surface_present( surface->client );

        if (swapchain_res < VK_SUCCESS) continue;
        if (!get_surface_rect( surface->hwnd, &client_rect, NtUserGetDpiForWindow( surface->hwnd ) ))
        {
            WARN( "Swapchain window %p is invalid, returning VK_ERROR_OUT_OF_DATE_KHR\n", surface->hwnd );
            if (present_info->pResults) present_info->pResults[i] = VK_ERROR_OUT_OF_DATE_KHR;
            if (res >= VK_SUCCESS) res = VK_ERROR_OUT_OF_DATE_KHR;
        }
        else if (swapchain_res)
            WARN( "Present returned status %d for swapchain %p\n", swapchain_res, swapchain );
        else if (!extents_equals( &swapchain->extents, &client_rect ))
        {
            WARN( "Swapchain size %dx%d does not match client rect %s, returning VK_SUBOPTIMAL_KHR\n",
                  swapchain->extents.width, swapchain->extents.height, wine_dbgstr_rect( &client_rect ) );
            if (present_info->pResults) present_info->pResults[i] = VK_SUBOPTIMAL_KHR;
            if (!res) res = VK_SUBOPTIMAL_KHR;
        }
    }
#ifdef __SWITCH__
    nx_vk_note_present( client_swapchains, present_info->swapchainCount, res );
#endif

    if (TRACE_ON( fps ))
    {
        static unsigned long frames, frames_total;
        static long prev_time, start_time;
        DWORD time;

        time = NtGetTickCount();
        frames++;
        frames_total++;

        if (time - prev_time > 1500)
        {
            TRACE_(fps)( "%p @ approx %.2ffps, total %.2ffps\n", queue, 1000.0 * frames / (time - prev_time),
                         1000.0 * frames_total / (time - start_time) );
            prev_time = time;
            frames = 0;

            if (!start_time) start_time = time;
        }
    }

failed:
    mem_free( &pool );
    return res;
}

static LARGE_INTEGER *get_nt_timeout( LARGE_INTEGER *time, DWORD timeout )
{
    if (timeout == INFINITE) return NULL;
    time->QuadPart = (ULONGLONG)timeout * -10000;
    return time;
}

static VkResult acquire_keyed_mutexes( VkWin32KeyedMutexAcquireReleaseInfoKHR *mutex_info, struct mempool *pool,
                                       const VkSemaphoreSubmitInfo **semaphores, UINT *semaphores_count )
{
    UINT i, count = *semaphores_count;
    VkSemaphoreSubmitInfo *submits;
    NTSTATUS status;

    if (!(submits = mem_alloc( pool, (count + mutex_info->acquireCount) * sizeof(*submits) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy( submits, *semaphores, count * sizeof(*submits) );
    memset( submits + count, 0, mutex_info->acquireCount * sizeof(*submits) );

    for (i = 0; i < mutex_info->acquireCount; i++)
    {
        LARGE_INTEGER timeout;
        struct device_memory *memory = device_memory_from_handle( mutex_info->pAcquireSyncs[i] );
        D3DKMT_ACQUIREKEYEDMUTEX acquire =
        {
            .hKeyedMutex = memory->mutex,
            .Key = mutex_info->pAcquireKeys[i],
            .pTimeout = get_nt_timeout( &timeout, mutex_info->pAcquireTimeouts[i] ),
        };
        VkSemaphoreSubmitInfo submit =
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = memory->semaphore,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0,
        };

        if ((status = NtGdiDdDDIAcquireKeyedMutex( &acquire ))) goto error;
        submit.value = memory->semaphore_value = acquire.FenceValue;
        submits[count++] = submit;
    }

    *semaphores = submits;
    *semaphores_count = count;
    return VK_SUCCESS;

error:
    WARN( "Failed to acquire keyed mutex 0x%s key 0x%s, status %#x\n", wine_dbgstr_longlong( mutex_info->pAcquireSyncs[i] ),
          wine_dbgstr_longlong( mutex_info->pAcquireKeys[i] ), status );

    while (i--)
    {
        struct device_memory *memory = device_memory_from_handle( mutex_info->pAcquireSyncs[i] );
        D3DKMT_RELEASEKEYEDMUTEX release =
        {
            .hKeyedMutex = memory->mutex,
            .Key = mutex_info->pAcquireKeys[i],
            .FenceValue = memory->semaphore_value,
        };
        NtGdiDdDDIReleaseKeyedMutex( &release );
    }
    return status == STATUS_TIMEOUT ? VK_TIMEOUT : VK_ERROR_UNKNOWN;
}

static VkResult release_keyed_mutexes( VkWin32KeyedMutexAcquireReleaseInfoKHR *mutex_info, struct mempool *pool,
                                       const VkSemaphoreSubmitInfo **semaphores, UINT *semaphores_count )
{
    UINT i, count = *semaphores_count;
    VkSemaphoreSubmitInfo *submits;
    NTSTATUS status;

    if (!(submits = mem_alloc( pool, (count + mutex_info->releaseCount) * sizeof(*submits) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy( submits, *semaphores, count * sizeof(*submits) );
    memset( submits + count, 0, mutex_info->releaseCount * sizeof(*submits) );

    for (i = 0; i < mutex_info->releaseCount; i++)
    {
        struct device_memory *memory = device_memory_from_handle( mutex_info->pReleaseSyncs[i] );
        D3DKMT_RELEASEKEYEDMUTEX release =
        {
            .hKeyedMutex = memory->mutex,
            .Key = mutex_info->pReleaseKeys[i],
            .FenceValue = memory->semaphore_value + 1,
        };
        VkSemaphoreSubmitInfo submit =
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = memory->semaphore,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0,
        };

        if ((status = NtGdiDdDDIReleaseKeyedMutex( &release ))) goto failed;
        submit.value = memory->semaphore_value + 1;
        submits[count++] = submit;
    }

    *semaphores = submits;
    *semaphores_count = count;
    return VK_SUCCESS;

failed:
    WARN( "Failed to release keyed mutex 0x%s key 0x%s, status %#x\n", wine_dbgstr_longlong( mutex_info->pReleaseSyncs[i] ),
          wine_dbgstr_longlong( mutex_info->pReleaseKeys[i] ), status );
    return VK_ERROR_UNKNOWN;
}

static VkResult win32u_vkQueueSubmit( VkQueue client_queue, uint32_t count, const VkSubmitInfo *submits, VkFence client_fence )
{
    struct vulkan_fence *fence = client_fence ? vulkan_fence_from_handle( client_fence ) : NULL;
    struct vulkan_queue *queue = vulkan_queue_from_handle( client_queue );
    struct vulkan_device *device = queue->device;
    VkResult res = VK_ERROR_OUT_OF_HOST_MEMORY;
    VkTimelineSemaphoreSubmitInfo *timelines;
    struct mempool pool = {0};

    TRACE( "queue %p, count %u, submits %p, fence %p\n", queue, count, submits, fence );

    if (!(timelines = mem_alloc( &pool, count * sizeof(*timelines) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset( timelines, 0, count * sizeof(*timelines) );

    for (uint32_t i = 0; i < count; i++)
    {
        VkSubmitInfo *submit = (VkSubmitInfo *)submits + i; /* cast away const, chain has been copied in the thunks */
        const VkSemaphoreSubmitInfo *wait_infos = NULL, *signal_infos = NULL;
        VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)submit;
        VkTimelineSemaphoreSubmitInfo *timeline = timelines + i;
        VkSemaphore *wait_semaphores, *signal_semaphores;
        VkDeviceGroupSubmitInfo *device_group = NULL;
        UINT wait_count = 0, signal_count = 0;
        VkPipelineStageFlags *wait_stages;
        uint32_t *indexes;
        uint64_t *values;

        for (uint32_t j = 0; j < submit->commandBufferCount; j++)
        {
            VkCommandBuffer *command_buffers = (VkCommandBuffer *)submit->pCommandBuffers; /* cast away const, chain has been copied in the thunks */
            struct vulkan_command_buffer *command_buffer = vulkan_command_buffer_from_handle( command_buffers[j] );
            command_buffers[j] = command_buffer->host.command_buffer;
        }

        for (uint32_t j = 0; j < submit->waitSemaphoreCount; j++)
        {
            VkSemaphore *semaphores = (VkSemaphore *)submit->pWaitSemaphores; /* cast away const, it has been copied in the thunks */
            struct vulkan_semaphore *semaphore = vulkan_semaphore_from_handle( semaphores[j] );
            semaphores[j] = semaphore->host.semaphore;
        }

        for (uint32_t j = 0; j < submit->signalSemaphoreCount; j++)
        {
            VkSemaphore *semaphores = (VkSemaphore *)submit->pSignalSemaphores; /* cast away const, it has been copied in the thunks */
            struct vulkan_semaphore *semaphore = vulkan_semaphore_from_handle( semaphores[j] );
            semaphores[j] = semaphore->host.semaphore;
        }

        for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
        {
            switch ((*next)->sType)
            {
            case VK_STRUCTURE_TYPE_D3D12_FENCE_SUBMIT_INFO_KHR:
                FIXME( "VK_STRUCTURE_TYPE_D3D12_FENCE_SUBMIT_INFO_KHR not implemented!\n" );
                *next = (*next)->pNext; next = &prev;
                break;
            case VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO:
                device_group = (VkDeviceGroupSubmitInfo *)*next;
                break;
            case VK_STRUCTURE_TYPE_FRAME_BOUNDARY_EXT: break;
            case VK_STRUCTURE_TYPE_FRAME_BOUNDARY_TENSORS_ARM: break;
            case VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV: break;
            case VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR: break;
            case VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO: break;
            case VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO:
                if (timeline->sType) ERR( "Duplicated timeline semaphore submit info!\n" );
                *timeline = *(VkTimelineSemaphoreSubmitInfo *)*next;
                *next = (*next)->pNext; next = &prev; /* remove it from the chain, we'll add it back below */
                break;
            case VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR:
            {
                VkWin32KeyedMutexAcquireReleaseInfoKHR *mutex_info = (VkWin32KeyedMutexAcquireReleaseInfoKHR *)*next;
                if ((res = acquire_keyed_mutexes( mutex_info, &pool, &wait_infos, &wait_count ))) goto failed;
                if ((res = release_keyed_mutexes( mutex_info, &pool, &signal_infos, &signal_count ))) goto failed;
                *next = (*next)->pNext; next = &prev;
                break;
            }
            default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
            }
        }

        if (wait_count) /* extra wait semaphores, need to update arrays and counts */
        {
            if (!(wait_semaphores = mem_alloc( &pool, (submit->waitSemaphoreCount + wait_count) * sizeof(*wait_semaphores) ))) goto failed;
            memcpy( wait_semaphores, submit->pWaitSemaphores, submit->waitSemaphoreCount * sizeof(*wait_semaphores) );
            submit->pWaitSemaphores = wait_semaphores;

            if (!(wait_stages = mem_alloc( &pool, (submit->waitSemaphoreCount + wait_count) * sizeof(*wait_stages) ))) goto failed;
            memcpy( wait_stages, submit->pWaitDstStageMask, submit->waitSemaphoreCount * sizeof(*wait_stages) );
            submit->pWaitDstStageMask = wait_stages;

            for (uint32_t j = 0; j < wait_count; j++)
            {
                wait_semaphores[submit->waitSemaphoreCount + j] = wait_infos[j].semaphore;
                wait_stages[submit->waitSemaphoreCount + j] = wait_infos[j].stageMask;
            }
            submit->waitSemaphoreCount += wait_count;

            if (!(values = mem_alloc( &pool, (timeline->waitSemaphoreValueCount + wait_count) * sizeof(*values) ))) goto failed;
            memcpy( values, timeline->pWaitSemaphoreValues, timeline->waitSemaphoreValueCount * sizeof(*values) );
            for (uint32_t j = 0; j < wait_count; j++) values[submit->waitSemaphoreCount + j] = wait_infos[j].value;
            timeline->waitSemaphoreValueCount = submit->waitSemaphoreCount;
            timeline->pWaitSemaphoreValues = values;

            if (device_group)
            {
                if (!(indexes = mem_alloc( &pool, submit->waitSemaphoreCount * sizeof(*indexes) ))) goto failed;
                memcpy( indexes, device_group->pWaitSemaphoreDeviceIndices, device_group->waitSemaphoreCount * sizeof(*indexes) );
                for (uint32_t j = 0; j < wait_count; j++) indexes[device_group->waitSemaphoreCount + j] = wait_infos[j].deviceIndex;
                device_group->waitSemaphoreCount = submit->waitSemaphoreCount;
                device_group->pWaitSemaphoreDeviceIndices = indexes;
            }
        }

        if (signal_count) /* extra signal semaphores, need to update arrays and counts */
        {
            if (!(signal_semaphores = mem_alloc( &pool, (submit->signalSemaphoreCount + signal_count) * sizeof(*signal_semaphores) ))) goto failed;
            memcpy( signal_semaphores, submit->pSignalSemaphores, submit->signalSemaphoreCount * sizeof(*signal_semaphores) );
            for (uint32_t j = 0; j < signal_count; j++) signal_semaphores[submit->signalSemaphoreCount + j] = signal_infos[j].semaphore;
            submit->signalSemaphoreCount += signal_count;
            submit->pSignalSemaphores = signal_semaphores;

            if (!(values = mem_alloc( &pool, submit->signalSemaphoreCount * sizeof(*values) ))) goto failed;
            memcpy( values, timeline->pSignalSemaphoreValues, timeline->signalSemaphoreValueCount * sizeof(*values) );
            for (uint32_t j = 0; j < signal_count; j++) values[submit->signalSemaphoreCount + j] = signal_infos[j].value;
            timeline->signalSemaphoreValueCount = submit->signalSemaphoreCount;
            timeline->pSignalSemaphoreValues = values;

            if (device_group)
            {
                if (!(indexes = mem_alloc( &pool, submit->signalSemaphoreCount * sizeof(*indexes) ))) goto failed;
                memcpy( indexes, device_group->pSignalSemaphoreDeviceIndices, device_group->signalSemaphoreCount * sizeof(*indexes) );
                for (uint32_t j = 0; j < signal_count; j++) indexes[device_group->signalSemaphoreCount + j] = signal_infos[j].deviceIndex;
                device_group->signalSemaphoreCount = submit->signalSemaphoreCount;
                device_group->pSignalSemaphoreDeviceIndices = indexes;
            }
        }

        /* insert the timeline semaphore values in the chain if it was there or has been created */
        if (timeline->sType || wait_count || signal_count)
        {
            timeline->sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            timeline->pNext = submit->pNext;
            submit->pNext = timeline;
        }
    }

    res = device->p_vkQueueSubmit( queue->host.queue, count, submits, fence ? fence->host.fence : 0 );

failed:
    mem_free( &pool );
    return res;
}

static VkResult queue_submit( struct vulkan_queue *queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence client_fence, PFN_vkQueueSubmit2 p_vkQueueSubmit2 )
{
    struct vulkan_fence *fence = client_fence ? vulkan_fence_from_handle( client_fence ) : NULL;
    struct mempool pool = {0};
    VkResult res;

    for (uint32_t i = 0; i < count; i++)
    {
        VkSubmitInfo2 *submit = (VkSubmitInfo2 *)submits + i; /* cast away const, chain has been copied in the thunks */
        VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)submit;

        for (uint32_t j = 0; j < submit->commandBufferInfoCount; j++)
        {
            VkCommandBufferSubmitInfoKHR *command_buffer_infos = (VkCommandBufferSubmitInfoKHR *)submit->pCommandBufferInfos; /* cast away const, chain has been copied in the thunks */
            struct vulkan_command_buffer *command_buffer = vulkan_command_buffer_from_handle( command_buffer_infos[j].commandBuffer );
            command_buffer_infos[j].commandBuffer = command_buffer->host.command_buffer;
            if (command_buffer_infos->pNext) FIXME( "Unhandled struct chain\n" );
        }

        for (uint32_t j = 0; j < submit->waitSemaphoreInfoCount; j++)
        {
            VkSemaphoreSubmitInfo *semaphore_infos = (VkSemaphoreSubmitInfo *)submit->pWaitSemaphoreInfos; /* cast away const, it has been copied in the thunks */
            struct vulkan_semaphore *semaphore = vulkan_semaphore_from_handle( semaphore_infos[j].semaphore );
            semaphore_infos[j].semaphore = semaphore->host.semaphore;
            if (semaphore_infos->pNext) FIXME( "Unhandled struct chain\n" );
        }

        for (uint32_t j = 0; j < submit->signalSemaphoreInfoCount; j++)
        {
            VkSemaphoreSubmitInfo *semaphore_infos = (VkSemaphoreSubmitInfo *)submit->pSignalSemaphoreInfos; /* cast away const, it has been copied in the thunks */
            struct vulkan_semaphore *semaphore = vulkan_semaphore_from_handle( semaphore_infos[j].semaphore );
            semaphore_infos[j].semaphore = semaphore->host.semaphore;
            if (semaphore_infos->pNext) FIXME( "Unhandled struct chain\n" );
        }

        for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
        {
            switch ((*next)->sType)
            {
            case VK_STRUCTURE_TYPE_FRAME_BOUNDARY_EXT: break;
            case VK_STRUCTURE_TYPE_FRAME_BOUNDARY_TENSORS_ARM: break;
            case VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV: break;
            case VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR: break;
            case VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR:
            {
                VkWin32KeyedMutexAcquireReleaseInfoKHR *mutex_info = (VkWin32KeyedMutexAcquireReleaseInfoKHR *)*next;
                if ((res = acquire_keyed_mutexes( mutex_info, &pool, &submit->pWaitSemaphoreInfos, &submit->waitSemaphoreInfoCount ))) goto failed;
                if ((res = release_keyed_mutexes( mutex_info, &pool, &submit->pSignalSemaphoreInfos, &submit->signalSemaphoreInfoCount ))) goto failed;
                *next = (*next)->pNext; next = &prev;
                break;
            }
            default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
            }
        }
    }

    res = p_vkQueueSubmit2( queue->host.queue, count, submits, fence ? fence->host.fence : 0 );

failed:
    mem_free( &pool );
    return res;
}

static VkResult win32u_vkQueueSubmit2( VkQueue client_queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence client_fence )
{
    struct vulkan_fence *fence = client_fence ? vulkan_fence_from_handle( client_fence ) : NULL;
    struct vulkan_queue *queue = vulkan_queue_from_handle( client_queue );
    struct vulkan_device *device = queue->device;

    TRACE( "queue %p, count %u, submits %p, fence %p\n", queue, count, submits, fence );

    return queue_submit( queue, count, submits, client_fence, device->p_vkQueueSubmit2 );
}

static VkResult win32u_vkQueueSubmit2KHR( VkQueue client_queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence client_fence )
{
    struct vulkan_fence *fence = client_fence ? vulkan_fence_from_handle( client_fence ) : NULL;
    struct vulkan_queue *queue = vulkan_queue_from_handle( client_queue );
    struct vulkan_device *device = queue->device;

    TRACE( "queue %p, count %u, submits %p, fence %p\n", queue, count, submits, fence );

    return queue_submit( queue, count, submits, client_fence, device->p_vkQueueSubmit2KHR );
}

static HANDLE create_shared_semaphore_handle( D3DKMT_HANDLE local, const VkExportSemaphoreWin32HandleInfoKHR *info )
{
    SECURITY_DESCRIPTOR *security = info->pAttributes ? info->pAttributes->lpSecurityDescriptor : NULL;
    WCHAR bufferW[MAX_PATH * 2];
    UNICODE_STRING name = {.Buffer = bufferW};
    OBJECT_ATTRIBUTES attr;
    NTSTATUS status;
    HANDLE shared;

    if (info->name) init_shared_resource_path( info->name, &name );
    InitializeObjectAttributes( &attr, info->name ? &name : NULL, OBJ_CASE_INSENSITIVE, NULL, security );

    if (!(status = NtGdiDdDDIShareObjects( 1, &local, &attr, info->dwAccess, &shared ))) return shared;
    WARN( "Failed to share resource %#x, status %#x\n", local, status );
    return NULL;
}

HANDLE open_shared_semaphore_from_name( const WCHAR *name )
{
    D3DKMT_OPENSYNCOBJECTNTHANDLEFROMNAME open_name = {0};
    WCHAR bufferW[MAX_PATH * 2];
    UNICODE_STRING name_str = {.Buffer = bufferW};
    OBJECT_ATTRIBUTES attr;
    NTSTATUS status;

    init_shared_resource_path( name, &name_str );
    InitializeObjectAttributes( &attr, &name_str, OBJ_OPENIF, NULL, NULL );

    open_name.dwDesiredAccess = GENERIC_ALL;
    open_name.pObjAttrib = &attr;
    status = NtGdiDdDDIOpenSyncObjectNtHandleFromName( &open_name );
    if (status) WARN( "Failed to open %s, status %#x\n", debugstr_w( name ), status );
    return open_name.hNtHandle;
}

static VkResult win32u_vkCreateSemaphore( VkDevice client_device, const VkSemaphoreCreateInfo *client_create_info,
                                          const VkAllocationCallbacks *allocator, VkSemaphore *ret )
{
    VkSemaphoreCreateInfo *create_info = (VkSemaphoreCreateInfo *)client_create_info; /* cast away const, chain has been copied in the thunks */
    VkExportSemaphoreWin32HandleInfoKHR export_win32 = {.dwAccess = GENERIC_ALL};
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)create_info;
    struct vulkan_instance *instance = device->physical_device->instance;
    VkExportSemaphoreCreateInfoKHR *export_info = NULL;
    struct semaphore *semaphore;
    VkSemaphore host_semaphore;
    BOOL nt_shared = FALSE;
    VkResult res;

    TRACE( "device %p, create_info %p, allocator %p, ret %p\n", device, create_info, allocator, ret );

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO:
            export_info = (VkExportSemaphoreCreateInfoKHR *)*next;
            if (!(export_info->handleTypes & EXTERNAL_SEMAPHORE_WIN32_BITS))
                FIXME( "Unsupported handle types %#x\n", export_info->handleTypes );
            else
            {
                nt_shared = !(export_info->handleTypes & VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT);
                export_info->handleTypes = get_host_external_semaphore_type();
            }
            break;
        case VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR:
            export_win32 = *(VkExportSemaphoreWin32HandleInfoKHR *)*next;
            *next = (*next)->pNext; next = &prev;
            break;
        case VK_STRUCTURE_TYPE_QUERY_LOW_LATENCY_SUPPORT_NV: break;
        case VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    if (!(semaphore = calloc( 1, sizeof(*semaphore) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;

    if ((res = device->p_vkCreateSemaphore( device->host.device, create_info, NULL /* allocator */, &host_semaphore )))
    {
        free( semaphore );
        return res;
    }

    if (export_info)
    {
        VkSemaphoreGetFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = host_semaphore};
        int fd = -1;

        switch ((fd_info.handleType = get_host_external_semaphore_type()))
        {
        case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT:
            if ((res = device->p_vkGetSemaphoreFdKHR( device->host.device, &fd_info, &fd ))) goto failed;
            break;
        default:
            FIXME( "Unsupported handle type %#x\n", fd_info.handleType );
            break;
        }

        semaphore->local = d3dkmt_create_sync( fd, nt_shared ? NULL : &semaphore->global );
        close( fd );

        if (!semaphore->local) goto failed;
        if (nt_shared && !(semaphore->shared = create_shared_semaphore_handle( semaphore->local, &export_win32 ))) goto failed;
    }

    vulkan_object_init( &semaphore->obj.obj, host_semaphore );
    instance->p_insert_object( instance, &semaphore->obj.obj );

    *ret = semaphore->obj.client.semaphore;
    return res;

failed:
    WARN( "Failed to create semaphore, res %d\n", res );
    device->p_vkDestroySemaphore( device->host.device, host_semaphore, NULL );
    d3dkmt_destroy_sync( semaphore->local );
    free( semaphore );
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void win32u_vkDestroySemaphore( VkDevice client_device, VkSemaphore client_semaphore, const VkAllocationCallbacks *allocator )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct semaphore *semaphore = semaphore_from_handle( client_semaphore );
    struct vulkan_instance *instance = device->physical_device->instance;

    TRACE( "device %p, semaphore %p, allocator %p\n", device, semaphore, allocator );

    if (!client_semaphore) return;

    device->p_vkDestroySemaphore( device->host.device, semaphore->obj.host.semaphore, NULL /* allocator */ );
    instance->p_remove_object( instance, &semaphore->obj.obj );

    if (semaphore->shared) NtClose( semaphore->shared );
    d3dkmt_destroy_sync( semaphore->local );
    free( semaphore );
}

static VkResult win32u_vkGetSemaphoreWin32HandleKHR( VkDevice client_device, const VkSemaphoreGetWin32HandleInfoKHR *handle_info, HANDLE *handle )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct semaphore *semaphore = semaphore_from_handle( handle_info->semaphore );

    TRACE( "device %p, handle_info %p, handle %p\n", device, handle_info, handle );

    switch (handle_info->handleType)
    {
    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
        TRACE( "Returning global D3DKMT handle %#x\n", semaphore->global );
        *handle = UlongToPtr( semaphore->global );
        return VK_SUCCESS;

    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT:
    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT:
        NtDuplicateObject( NtCurrentProcess(), semaphore->shared, NtCurrentProcess(), handle, 0, 0, DUPLICATE_SAME_ATTRIBUTES | DUPLICATE_SAME_ACCESS );
        TRACE( "Returning NT shared handle %p -> %p\n", semaphore->shared, *handle );
        return VK_SUCCESS;

    default:
        FIXME( "Unsupported handle type %#x\n", handle_info->handleType );
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
}

static VkResult win32u_vkImportSemaphoreWin32HandleKHR( VkDevice client_device, const VkImportSemaphoreWin32HandleInfoKHR *handle_info )
{
    VkImportSemaphoreFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct semaphore *semaphore = semaphore_from_handle( handle_info->semaphore );
    D3DKMT_HANDLE local, global = 0;
    VkResult res = VK_SUCCESS;
    HANDLE shared = NULL;

    TRACE( "device %p, handle_info %p\n", device, handle_info );

    switch (handle_info->handleType)
    {
    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
        global = PtrToUlong( handle_info->handle );
        if (!(local = d3dkmt_open_sync( global, NULL ))) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        break;
    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT:
    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT:
        if (handle_info->name && !(shared = open_shared_semaphore_from_name( handle_info->name )))
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        else if (!(shared = handle_info->handle) || NtDuplicateObject( NtCurrentProcess(), shared, NtCurrentProcess(), &shared,
                                                                       0, 0, DUPLICATE_SAME_ATTRIBUTES | DUPLICATE_SAME_ACCESS ))
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;

        if (!(local = d3dkmt_open_sync( 0, shared )))
        {
            NtClose( shared );
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        break;
    default:
        FIXME( "Unsupported handle type %#x\n", handle_info->handleType );
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    if ((fd_info.fd = d3dkmt_object_get_fd( local )) < 0) res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    else
    {
        fd_info.handleType = get_host_external_semaphore_type();
        fd_info.semaphore = semaphore->obj.host.semaphore;
        fd_info.flags = handle_info->flags;
        res = device->p_vkImportSemaphoreFdKHR( device->host.device, &fd_info );
    }

    if (res || handle_info->flags & VK_SEMAPHORE_IMPORT_TEMPORARY_BIT)
    {
        /* FIXME: Should we still keep the temporary handles for vkGetSemaphoreWin32HandleKHR? */
        if (shared) NtClose( shared );
        d3dkmt_destroy_sync( local );
    }
    else
    {
        if (semaphore->shared) NtClose( semaphore->shared );
        d3dkmt_destroy_sync( semaphore->local );
        semaphore->shared = shared;
        semaphore->global = global;
        semaphore->local = local;
    }
    return res;
}

static void get_physical_device_external_semaphore_properties( struct vulkan_physical_device *physical_device, const VkPhysicalDeviceExternalSemaphoreInfo *client_semaphore_info,
                                                               VkExternalSemaphoreProperties *semaphore_properties, PFN_vkGetPhysicalDeviceExternalSemaphoreProperties p_vkGetPhysicalDeviceExternalSemaphoreProperties )
{
    VkPhysicalDeviceExternalSemaphoreInfo *semaphore_info = (VkPhysicalDeviceExternalSemaphoreInfo *)client_semaphore_info; /* cast away const, it has been copied in the thunks */
    VkExternalSemaphoreHandleTypeFlagBits handle_type;

    handle_type = semaphore_info->handleType;
    if (semaphore_info->handleType & EXTERNAL_SEMAPHORE_WIN32_BITS) semaphore_info->handleType = get_host_external_semaphore_type();

    p_vkGetPhysicalDeviceExternalSemaphoreProperties( physical_device->host.physical_device, semaphore_info, semaphore_properties );
    semaphore_properties->compatibleHandleTypes = handle_type;
    semaphore_properties->exportFromImportedHandleTypes = handle_type;
}

static void win32u_vkGetPhysicalDeviceExternalSemaphoreProperties( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalSemaphoreInfo *semaphore_info,
                                                                   VkExternalSemaphoreProperties *semaphore_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, semaphore_info %p, semaphore_properties %p\n", physical_device, semaphore_info, semaphore_properties );

    get_physical_device_external_semaphore_properties( physical_device, semaphore_info, semaphore_properties, instance->p_vkGetPhysicalDeviceExternalSemaphoreProperties );
}

static void win32u_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalSemaphoreInfo *semaphore_info,
                                                                      VkExternalSemaphoreProperties *semaphore_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, semaphore_info %p, semaphore_properties %p\n", physical_device, semaphore_info, semaphore_properties );

    get_physical_device_external_semaphore_properties( physical_device, semaphore_info, semaphore_properties, instance->p_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR );
}

static VkResult win32u_vkCreateFence( VkDevice client_device, const VkFenceCreateInfo *client_create_info, const VkAllocationCallbacks *allocator, VkFence *ret )
{
    VkFenceCreateInfo *create_info = (VkFenceCreateInfo *)client_create_info; /* cast away const, chain has been copied in the thunks */
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    VkExportSemaphoreWin32HandleInfoKHR export_win32 = {.dwAccess = GENERIC_ALL};
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)create_info;
    struct vulkan_instance *instance = device->physical_device->instance;
    VkExportFenceCreateInfoKHR *export_info = NULL;
    BOOL nt_shared = FALSE;
    struct fence *fence;
    VkFence host_fence;
    VkResult res;

    TRACE( "device %p, create_info %p, allocator %p, ret %p\n", device, create_info, allocator, ret );

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO:
            export_info = (VkExportFenceCreateInfoKHR *)*next;
            if (!(export_info->handleTypes & EXTERNAL_FENCE_WIN32_BITS))
                FIXME( "Unsupported handle types %#x\n", export_info->handleTypes );
            else
            {
                nt_shared = !(export_info->handleTypes & VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT);
                export_info->handleTypes = get_host_external_fence_type();
            }
            break;
        case VK_STRUCTURE_TYPE_EXPORT_FENCE_WIN32_HANDLE_INFO_KHR:
        {
            VkExportFenceWin32HandleInfoKHR *fence_win32 = (VkExportFenceWin32HandleInfoKHR *)*next;
            export_win32.pAttributes = fence_win32->pAttributes;
            export_win32.dwAccess = fence_win32->dwAccess;
            export_win32.name = fence_win32->name;
            *next = (*next)->pNext; next = &prev;
            break;
        }
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    if (!(fence = calloc( 1, sizeof(*fence) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;

    if ((res = device->p_vkCreateFence( device->host.device, create_info, NULL /* allocator */, &host_fence )))
    {
        free( fence );
        return res;
    }

    if (export_info)
    {
        VkFenceGetFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, .fence = host_fence};
        int fd = -1;

        switch ((fd_info.handleType = get_host_external_fence_type()))
        {
        case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT:
            if ((res = device->p_vkGetFenceFdKHR( device->host.device, &fd_info, &fd ))) goto failed;
            break;
        default:
            FIXME( "Unsupported handle type %#x\n", fd_info.handleType );
            break;
        }

        fence->local = d3dkmt_create_sync( fd, nt_shared ? NULL : &fence->global );
        close( fd );

        if (!fence->local) goto failed;
        if (nt_shared && !(fence->shared = create_shared_semaphore_handle( fence->local, &export_win32 ))) goto failed;
    }

    vulkan_object_init( &fence->obj.obj, host_fence );
    instance->p_insert_object( instance, &fence->obj.obj );

    *ret = fence->obj.client.fence;
    return res;

failed:
    WARN( "Failed to create fence, res %d\n", res );
    device->p_vkDestroyFence( device->host.device, host_fence, NULL );
    d3dkmt_destroy_sync( fence->local );
    free( fence );
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void win32u_vkDestroyFence( VkDevice client_device, VkFence client_fence, const VkAllocationCallbacks *allocator )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct fence *fence = fence_from_handle( client_fence );
    struct vulkan_instance *instance = device->physical_device->instance;

    TRACE( "device %p, fence %p, allocator %p\n", device, fence, allocator );

    if (!client_fence) return;

    device->p_vkDestroyFence( device->host.device, fence->obj.host.fence, NULL /* allocator */ );
    instance->p_remove_object( instance, &fence->obj.obj );

    if (fence->shared) NtClose( fence->shared );
    d3dkmt_destroy_sync( fence->local );
    free( fence );
}

static VkResult win32u_vkGetFenceWin32HandleKHR( VkDevice client_device, const VkFenceGetWin32HandleInfoKHR *handle_info, HANDLE *handle )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct fence *fence = fence_from_handle( handle_info->fence );

    TRACE( "device %p, handle_info %p, handle %p\n", device, handle_info, handle );

    switch (handle_info->handleType)
    {
    case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
        TRACE( "Returning global D3DKMT handle %#x\n", fence->global );
        *handle = UlongToPtr( fence->global );
        return VK_SUCCESS;

    case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT:
        NtDuplicateObject( NtCurrentProcess(), fence->shared, NtCurrentProcess(), handle, 0, 0, DUPLICATE_SAME_ATTRIBUTES | DUPLICATE_SAME_ACCESS );
        TRACE( "Returning NT shared handle %p -> %p\n", fence->shared, *handle );
        return VK_SUCCESS;

    default:
        FIXME( "Unsupported handle type %#x\n", handle_info->handleType );
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
}

static VkResult win32u_vkImportFenceWin32HandleKHR( VkDevice client_device, const VkImportFenceWin32HandleInfoKHR *handle_info )
{
    VkImportFenceFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR};
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct fence *fence = fence_from_handle( handle_info->fence );
    D3DKMT_HANDLE local, global = 0;
    HANDLE shared = NULL;
    VkResult res;

    TRACE( "device %p, handle_info %p\n", device, handle_info );

    switch (handle_info->handleType)
    {
    case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
        global = PtrToUlong( handle_info->handle );
        if (!(local = d3dkmt_open_sync( global, NULL ))) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        break;
    case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT:
        if (handle_info->name && !(shared = open_shared_semaphore_from_name( handle_info->name )))
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        else if (!(shared = handle_info->handle) || NtDuplicateObject( NtCurrentProcess(), shared, NtCurrentProcess(), &shared,
                                                                       0, 0, DUPLICATE_SAME_ATTRIBUTES | DUPLICATE_SAME_ACCESS ))
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;

        if (!(local = d3dkmt_open_sync( 0, shared )))
        {
            NtClose( shared );
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        break;
    default:
        FIXME( "Unsupported handle type %#x\n", handle_info->handleType );
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    if ((fd_info.fd = d3dkmt_object_get_fd( local )) < 0) res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    else
    {
        fd_info.handleType = get_host_external_fence_type();
        fd_info.fence = fence->obj.host.fence;
        fd_info.flags = handle_info->flags;
        res = device->p_vkImportFenceFdKHR( device->host.device, &fd_info );
    }

    if (res || handle_info->flags & VK_FENCE_IMPORT_TEMPORARY_BIT)
    {
        /* FIXME: Should we still keep the temporary handles for vkGetFenceWin32HandleKHR? */
        if (shared) NtClose( shared );
        d3dkmt_destroy_sync( local );
    }
    else
    {
        if (fence->shared) NtClose( fence->shared );
        if (fence->local) d3dkmt_destroy_sync( fence->local );
        fence->shared = shared;
        fence->global = global;
        fence->local = local;
    }
    return VK_SUCCESS;
}

static void get_physical_device_external_fence_properties( struct vulkan_physical_device *physical_device, const VkPhysicalDeviceExternalFenceInfo *client_fence_info,
                                                           VkExternalFenceProperties *fence_properties, PFN_vkGetPhysicalDeviceExternalFenceProperties p_vkGetPhysicalDeviceExternalFenceProperties )
{
    VkPhysicalDeviceExternalFenceInfo *fence_info = (VkPhysicalDeviceExternalFenceInfo *)client_fence_info; /* cast away const, it has been copied in the thunks */
    VkExternalFenceHandleTypeFlagBits handle_type;

    handle_type = fence_info->handleType;
    if (fence_info->handleType & EXTERNAL_FENCE_WIN32_BITS) fence_info->handleType = get_host_external_fence_type();

    p_vkGetPhysicalDeviceExternalFenceProperties( physical_device->host.physical_device, fence_info, fence_properties );
    fence_properties->compatibleHandleTypes = handle_type;
    fence_properties->exportFromImportedHandleTypes = handle_type;
}

static void win32u_vkGetPhysicalDeviceExternalFenceProperties( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalFenceInfo *fence_info,
                                                               VkExternalFenceProperties *fence_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, fence_info %p, fence_properties %p\n", physical_device, fence_info, fence_properties );

    get_physical_device_external_fence_properties( physical_device, fence_info, fence_properties, instance->p_vkGetPhysicalDeviceExternalFenceProperties );
}

static void win32u_vkGetPhysicalDeviceExternalFencePropertiesKHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalFenceInfo *fence_info,
                                                                  VkExternalFenceProperties *fence_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, fence_info %p, fence_properties %p\n", physical_device, fence_info, fence_properties );

    get_physical_device_external_fence_properties( physical_device, fence_info, fence_properties, instance->p_vkGetPhysicalDeviceExternalFencePropertiesKHR );
}

static struct vulkan_funcs vulkan_funcs =
{
    .p_vkAcquireNextImage2KHR = win32u_vkAcquireNextImage2KHR,
    .p_vkAcquireNextImageKHR = win32u_vkAcquireNextImageKHR,
    .p_vkAllocateMemory = win32u_vkAllocateMemory,
    .p_vkCreateBuffer = win32u_vkCreateBuffer,
    .p_vkCreateDevice = win32u_vkCreateDevice,
    .p_vkCreateFence = win32u_vkCreateFence,
    .p_vkCreateImage = win32u_vkCreateImage,
    .p_vkCreateInstance = win32u_vkCreateInstance,
    .p_vkCreateSemaphore = win32u_vkCreateSemaphore,
    .p_vkCreateSwapchainKHR = win32u_vkCreateSwapchainKHR,
    .p_vkCreateWin32SurfaceKHR = win32u_vkCreateWin32SurfaceKHR,
    .p_vkDestroyDevice = win32u_vkDestroyDevice,
    .p_vkDestroyFence = win32u_vkDestroyFence,
    .p_vkDestroyInstance = win32u_vkDestroyInstance,
    .p_vkDestroySemaphore = win32u_vkDestroySemaphore,
    .p_vkDestroySurfaceKHR = win32u_vkDestroySurfaceKHR,
    .p_vkDestroySwapchainKHR = win32u_vkDestroySwapchainKHR,
    .p_vkFreeMemory = win32u_vkFreeMemory,
    .p_vkGetDeviceBufferMemoryRequirements = win32u_vkGetDeviceBufferMemoryRequirements,
    .p_vkGetDeviceBufferMemoryRequirementsKHR = win32u_vkGetDeviceBufferMemoryRequirements,
    .p_vkGetDeviceImageMemoryRequirements = win32u_vkGetDeviceImageMemoryRequirements,
    .p_vkGetDeviceQueue = win32u_vkGetDeviceQueue,
    .p_vkGetDeviceQueue2 = win32u_vkGetDeviceQueue2,
    .p_vkGetFenceWin32HandleKHR = win32u_vkGetFenceWin32HandleKHR,
    .p_vkGetMemoryWin32HandleKHR = win32u_vkGetMemoryWin32HandleKHR,
    .p_vkGetMemoryWin32HandlePropertiesKHR = win32u_vkGetMemoryWin32HandlePropertiesKHR,
    .p_vkGetPhysicalDeviceExternalBufferProperties = win32u_vkGetPhysicalDeviceExternalBufferProperties,
    .p_vkGetPhysicalDeviceExternalBufferPropertiesKHR = win32u_vkGetPhysicalDeviceExternalBufferPropertiesKHR,
    .p_vkGetPhysicalDeviceExternalFenceProperties = win32u_vkGetPhysicalDeviceExternalFenceProperties,
    .p_vkGetPhysicalDeviceExternalFencePropertiesKHR = win32u_vkGetPhysicalDeviceExternalFencePropertiesKHR,
    .p_vkGetPhysicalDeviceExternalSemaphoreProperties = win32u_vkGetPhysicalDeviceExternalSemaphoreProperties,
    .p_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR = win32u_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR,
    .p_vkGetPhysicalDeviceImageFormatProperties2 = win32u_vkGetPhysicalDeviceImageFormatProperties2,
    .p_vkGetPhysicalDeviceImageFormatProperties2KHR = win32u_vkGetPhysicalDeviceImageFormatProperties2KHR,
    .p_vkGetPhysicalDevicePresentRectanglesKHR = win32u_vkGetPhysicalDevicePresentRectanglesKHR,
    .p_vkGetPhysicalDeviceProperties = win32u_vkGetPhysicalDeviceProperties,
    .p_vkGetPhysicalDeviceProperties2 = win32u_vkGetPhysicalDeviceProperties2,
    .p_vkGetPhysicalDeviceProperties2KHR = win32u_vkGetPhysicalDeviceProperties2KHR,
    .p_vkGetPhysicalDeviceSurfaceCapabilities2KHR = win32u_vkGetPhysicalDeviceSurfaceCapabilities2KHR,
    .p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = win32u_vkGetPhysicalDeviceSurfaceCapabilitiesKHR,
    .p_vkGetPhysicalDeviceSurfaceFormats2KHR = win32u_vkGetPhysicalDeviceSurfaceFormats2KHR,
    .p_vkGetPhysicalDeviceSurfaceFormatsKHR = win32u_vkGetPhysicalDeviceSurfaceFormatsKHR,
    .p_vkGetPhysicalDeviceWin32PresentationSupportKHR = win32u_vkGetPhysicalDeviceWin32PresentationSupportKHR,
    .p_vkGetSemaphoreWin32HandleKHR = win32u_vkGetSemaphoreWin32HandleKHR,
    .p_vkGetSwapchainImagesKHR = win32u_vkGetSwapchainImagesKHR,
    .p_vkImportFenceWin32HandleKHR = win32u_vkImportFenceWin32HandleKHR,
    .p_vkImportSemaphoreWin32HandleKHR = win32u_vkImportSemaphoreWin32HandleKHR,
    .p_vkMapMemory = win32u_vkMapMemory,
    .p_vkMapMemory2KHR = win32u_vkMapMemory2KHR,
    .p_vkQueuePresentKHR = win32u_vkQueuePresentKHR,
    .p_vkQueueSubmit = win32u_vkQueueSubmit,
    .p_vkQueueSubmit2 = win32u_vkQueueSubmit2,
    .p_vkQueueSubmit2KHR = win32u_vkQueueSubmit2KHR,
    .p_vkUnmapMemory = win32u_vkUnmapMemory,
    .p_vkUnmapMemory2KHR = win32u_vkUnmapMemory2KHR,
};

static VkResult nulldrv_vulkan_surface_create( struct client_surface *client, const struct vulkan_instance *instance,
                                               VkSurfaceKHR *surface )
{
    VkHeadlessSurfaceCreateInfoEXT create_info = {.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    return instance->p_vkCreateHeadlessSurfaceEXT( instance->host.instance, &create_info, NULL, surface );
}

static VkBool32 nulldrv_get_physical_device_presentation_support( struct vulkan_physical_device *physical_device, uint32_t queue )
{
    return VK_TRUE;
}

static void nulldrv_map_instance_extensions( struct vulkan_instance_extensions *extensions )
{
    if (extensions->has_VK_KHR_win32_surface) extensions->has_VK_EXT_headless_surface = 1;
    if (extensions->has_VK_EXT_headless_surface) extensions->has_VK_KHR_win32_surface = 1;
}

static void nulldrv_map_device_extensions( struct vulkan_device_extensions *extensions )
{
    if (extensions->has_VK_KHR_external_memory_win32) extensions->has_VK_KHR_external_memory_fd = 1;
    if (extensions->has_VK_KHR_external_memory_fd) extensions->has_VK_KHR_external_memory_win32 = 1;
    if (extensions->has_VK_KHR_external_semaphore_win32) extensions->has_VK_KHR_external_semaphore_fd = 1;
    if (extensions->has_VK_KHR_external_semaphore_fd) extensions->has_VK_KHR_external_semaphore_win32 = 1;
    if (extensions->has_VK_KHR_external_fence_win32) extensions->has_VK_KHR_external_fence_fd = 1;
    if (extensions->has_VK_KHR_external_fence_fd) extensions->has_VK_KHR_external_fence_win32 = 1;
}

static const struct vulkan_driver_funcs nulldrv_funcs =
{
    .p_vulkan_surface_create = nulldrv_vulkan_surface_create,
    .p_get_physical_device_presentation_support = nulldrv_get_physical_device_presentation_support,
    .p_map_instance_extensions = nulldrv_map_instance_extensions,
    .p_map_device_extensions = nulldrv_map_device_extensions,
};

static void vulkan_driver_init(void)
{
    UINT status;

    if ((status = user_driver->pVulkanInit( WINE_VULKAN_DRIVER_VERSION, vulkan_handle, &driver_funcs )) &&
        status != STATUS_NOT_IMPLEMENTED)
    {
        ERR( "Failed to initialize the driver vulkan functions, status %#x\n", status );
        return;
    }

    if (status == STATUS_NOT_IMPLEMENTED) driver_funcs = &nulldrv_funcs;
}

static void vulkan_driver_load(void)
{
    static pthread_once_t init_once = PTHREAD_ONCE_INIT;
    pthread_once( &init_once, vulkan_driver_init );
}

static VkResult lazydrv_vulkan_surface_create( struct client_surface *client, const struct vulkan_instance *instance,
                                               VkSurfaceKHR *surface )
{
    vulkan_driver_load();
    return driver_funcs->p_vulkan_surface_create( client, instance, surface );
}

static VkBool32 lazydrv_get_physical_device_presentation_support( struct vulkan_physical_device *physical_device, uint32_t queue )
{
    vulkan_driver_load();
    return driver_funcs->p_get_physical_device_presentation_support( physical_device, queue );
}

static void lazydrv_map_instance_extensions( struct vulkan_instance_extensions *extensions )
{
    vulkan_driver_load();
    return driver_funcs->p_map_instance_extensions( extensions );
}

static void lazydrv_map_device_extensions( struct vulkan_device_extensions *extensions )
{
    vulkan_driver_load();
    return driver_funcs->p_map_device_extensions( extensions );
}

static const struct vulkan_driver_funcs lazydrv_funcs =
{
    .p_vulkan_surface_create = lazydrv_vulkan_surface_create,
    .p_get_physical_device_presentation_support = lazydrv_get_physical_device_presentation_support,
    .p_map_instance_extensions = lazydrv_map_instance_extensions,
    .p_map_device_extensions = lazydrv_map_device_extensions,
};

static void vulkan_init_once(void)
{
    struct vulkan_instance_extensions extensions = {0};
    VkExtensionProperties *properties = NULL;
    uint32_t count = 0;
    VkResult res;

    const char *env = getenv( "WINE_DISABLE_FULLSCREEN_HACK" );
    fshack_enabled = !env || !atoi( env );

#if defined(__SWITCH__) && defined(WINE_NX_MESA_SWITCH)
    {
        static int static_vulkan;

        vulkan_handle = &static_vulkan;
        p_vkGetDeviceProcAddr = wine_nx_vkGetDeviceProcAddr;
        p_vkGetInstanceProcAddr = wine_nx_vkGetInstanceProcAddr;
    }
#else
#ifdef SONAME_LIBVULKAN
    vulkan_handle = dlopen( SONAME_LIBVULKAN, RTLD_NOW );
    if (!vulkan_handle) ERR( "Failed to load %s\n", SONAME_LIBVULKAN );
#else
    ERR( "Wine was built without Vulkan support.\n" );
#endif
    if (!vulkan_handle) return;

#define LOAD_FUNCPTR( f )                                                                          \
    if (!(p_##f = dlsym( vulkan_handle, #f )))                                                     \
    {                                                                                              \
        ERR( "Failed to find " #f "\n" );                                                          \
        dlclose( vulkan_handle );                                                                  \
        vulkan_handle = NULL;                                                                      \
        return;                                                                                    \
    }

    LOAD_FUNCPTR( vkGetDeviceProcAddr );
    LOAD_FUNCPTR( vkGetInstanceProcAddr );
#undef LOAD_FUNCPTR
#endif

    driver_funcs = &lazydrv_funcs;
    vulkan_funcs.p_vkGetInstanceProcAddr = p_vkGetInstanceProcAddr;
    vulkan_funcs.p_vkGetDeviceProcAddr = p_vkGetDeviceProcAddr;

#define LOAD_FUNCPTR( f ) p_##f = (PFN_##f)p_vkGetInstanceProcAddr( NULL, #f );
    LOAD_FUNCPTR( vkCreateInstance );
    LOAD_FUNCPTR( vkEnumerateInstanceExtensionProperties );
#undef LOAD_FUNCPTR

    do
    {
        free( properties );
        properties = NULL;
        if ((res = p_vkEnumerateInstanceExtensionProperties( NULL, &count, NULL ))) goto failed;
        if (!count || !(properties = malloc( count * sizeof(*properties) ))) goto failed;
    } while ((res = p_vkEnumerateInstanceExtensionProperties( NULL, &count, properties ) == VK_INCOMPLETE));
    if (res) goto failed;

    TRACE( "Host instance extensions:\n" );
    for (uint32_t i = 0; i < count; i++)
    {
        const char *extension = properties[i].extensionName;
#define USE_VK_EXT(x)                           \
        if (!strcmp( extension, #x ))           \
        {                                       \
            extensions.has_ ## x = 1;           \
            TRACE( "  - %s\n", extension );     \
        } else
        ALL_VK_INSTANCE_EXTS
#undef USE_VK_EXT
        WARN( "Extension %s is not supported.\n", debugstr_a(extension) );
    }
    vulkan_funcs.host_extensions = extensions;

    /* map host instance extensions for VK_KHR_win32_surface */
    driver_funcs->p_map_instance_extensions( &extensions );

    /* filter out unsupported client instance extensions */
#define USE_VK_EXT(x) vulkan_funcs.client_extensions.has_ ## x = extensions.has_ ## x;
    ALL_VK_CLIENT_INSTANCE_EXTS
#undef USE_VK_EXT

failed:
    if (res) ERR( "Failed to initialize instance extensions, res %d\n", res );
    free( properties );
}

/***********************************************************************
 *      __wine_get_vulkan_driver  (win32u.so)
 */
const struct vulkan_funcs *__wine_get_vulkan_driver( UINT version )
{
    static pthread_once_t init_once = PTHREAD_ONCE_INIT;

    if (version != WINE_VULKAN_DRIVER_VERSION)
    {
        ERR( "version mismatch, vulkan wants %u but win32u has %u\n", version, WINE_VULKAN_DRIVER_VERSION );
        return NULL;
    }

    pthread_once( &init_once, vulkan_init_once );
    if (!vulkan_handle) return NULL;
    return &vulkan_funcs;
}

/* unix side client-like instance wrapper to fit with the vulkan wrapping infrastructure */
struct instance_wrapper
{
    struct VkInstance_T client;
};

struct vulkan_instance *vulkan_instance_create( const struct vulkan_instance_extensions *extensions )
{
    const struct vulkan_funcs *funcs = __wine_get_vulkan_driver( WINE_VULKAN_DRIVER_VERSION );
    VkInstanceCreateInfo create_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    const char *extension_names[sizeof(*extensions) * 8];
    struct instance_wrapper *wrapper;
    UINT device_count = 8;
    VkResult res;

    if (!funcs) return NULL;

    create_info.ppEnabledExtensionNames = extension_names;
#define USE_VK_EXT(x) if (extensions->has_ ## x) extension_names[create_info.enabledExtensionCount++] = #x;
    ALL_VK_INSTANCE_EXTS
#undef USE_VK_EXT

    for (;;)
    {
        VkInstance instance;

        if (!(wrapper = calloc( 1, offsetof(struct instance_wrapper, client.physical_device[device_count]) ))) return NULL;
        wrapper->client.physical_device_count = device_count;
        wrapper->client.extensions = *extensions;
        instance = &wrapper->client;

        if ((res = funcs->p_vkCreateInstance( &create_info, NULL, &instance ))) break;
        if ((wrapper->client.physical_device_count <= device_count)) break;
        device_count = wrapper->client.physical_device_count;
        free( wrapper );
    }

    if (!res) return vulkan_instance_from_handle( &wrapper->client );
    WARN( "Failed to create instance, res %d\n", res );
    free( wrapper );
    return NULL;
}
