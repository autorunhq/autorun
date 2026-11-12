/*
 * The Vulkan entry points the runtime calls by name, for a runtime linked with
 * nxvk (CMakeLists.txt, WINE_NX_NXVK_DIR).
 *
 * mesa-switch's loaderless NVK defines vkGetInstanceProcAddr, the global
 * functions and vkCreateViSurfaceNN as symbols, and win32u, winevulkan and the
 * Vulkan probe call them so. nxvk defines one entry point,
 * vk_icdGetInstanceProcAddr, as a driver does for a loader; everything else is
 * found through it, which is what these do. They are weak, so a build of nxvk
 * that defines them itself keeps its own.
 */
#include <stddef.h>
/* vkCreateViSurfaceNN below takes its create info as it comes. */
#undef VK_USE_PLATFORM_VI_NN
#include <vulkan/vulkan.h>

/* What tools/package-autorun.py --nxvk looks for in the NRO it ships. */
__attribute__((used)) static const char nxvk_marker[] = "autorun-graphics-runtime: nxvk";

extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr( VkInstance instance, const char *name );

__attribute__((weak)) PFN_vkVoidFunction vkGetInstanceProcAddr( VkInstance instance, const char *name )
{
    return vk_icdGetInstanceProcAddr( instance, name );
}

__attribute__((weak)) VkResult vkCreateInstance( const VkInstanceCreateInfo *info,
                                                 const VkAllocationCallbacks *allocator, VkInstance *instance )
{
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr( NULL, "vkCreateInstance" );

    return create ? create( info, allocator, instance ) : VK_ERROR_INITIALIZATION_FAILED;
}

__attribute__((weak)) VkResult vkEnumerateInstanceExtensionProperties( const char *layer, uint32_t *count,
                                                                       VkExtensionProperties *properties )
{
    PFN_vkEnumerateInstanceExtensionProperties enumerate = (PFN_vkEnumerateInstanceExtensionProperties)
        vk_icdGetInstanceProcAddr( NULL, "vkEnumerateInstanceExtensionProperties" );

    if (!enumerate)
    {
        *count = 0;
        return VK_SUCCESS;
    }
    return enumerate( layer, count, properties );
}

__attribute__((weak)) VkResult vkEnumerateInstanceLayerProperties( uint32_t *count, VkLayerProperties *properties )
{
    (void)properties;
    *count = 0;  /* a driver has no layers; there is no loader to add any */
    return VK_SUCCESS;
}

__attribute__((weak)) VkResult vkEnumerateInstanceVersion( uint32_t *version )
{
    PFN_vkEnumerateInstanceVersion get = (PFN_vkEnumerateInstanceVersion)
        vk_icdGetInstanceProcAddr( NULL, "vkEnumerateInstanceVersion" );

    if (!get)
    {
        *version = VK_API_VERSION_1_0;
        return VK_SUCCESS;
    }
    return get( version );
}

/* VK_NN_vi_surface: the create info is NVK's own, passed through untouched
 * (dlls/win32u/winnx_vulkan.c). */
__attribute__((weak)) VkResult vkCreateViSurfaceNN( VkInstance instance, const void *info,
                                                    const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface )
{
    typedef VkResult (*create_vi_surface)( VkInstance, const void *, const VkAllocationCallbacks *, VkSurfaceKHR * );
    create_vi_surface create = (create_vi_surface)vk_icdGetInstanceProcAddr( instance, "vkCreateViSurfaceNN" );

    return create ? create( instance, info, allocator, surface ) : VK_ERROR_EXTENSION_NOT_PRESENT;
}
