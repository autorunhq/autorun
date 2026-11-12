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
#include <sys/types.h>  /* off_t, before <regex.h> */
#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <pwd.h>
#include <regex.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* vkCreateViSurfaceNN below takes its create info as it comes. */
#undef VK_USE_PLATFORM_VI_NN
#include <vulkan/vulkan.h>

/* What tools/package-autorun.py --nxvk looks for in the NRO it ships. */
__attribute__((used)) static const char nxvk_marker[] = "autorun-graphics-runtime: nxvk";

extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr( VkInstance instance, const char *name );

/* NVK leaves Maxwell (the Switch's GM20B) out unless told to take it, which
 * mesa-switch's does by itself. Before main, so before any Vulkan or EGL. */
__attribute__((constructor)) static void nxvk_take_maxwell( void )
{
    setenv( "NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 0 );
}

/* The newest instance the runtime asked for anything of: device functions are
 * found through an instance (vkGetDeviceProcAddr below). */
static VkInstance nxvk_instance;

__attribute__((weak)) PFN_vkVoidFunction vkGetInstanceProcAddr( VkInstance instance, const char *name )
{
    if (instance) __atomic_store_n( &nxvk_instance, instance, __ATOMIC_RELEASE );
    return vk_icdGetInstanceProcAddr( instance, name );
}

/* Mesa hands a device-level function asked of an instance as a trampoline
 * through the device's own table, so this is the device's vkGetDeviceProcAddr. */
__attribute__((weak)) PFN_vkVoidFunction vkGetDeviceProcAddr( VkDevice device, const char *name )
{
    static PFN_vkGetDeviceProcAddr get;
    VkInstance instance;

    if (!get && (instance = __atomic_load_n( &nxvk_instance, __ATOMIC_ACQUIRE )))
        get = (PFN_vkGetDeviceProcAddr)vk_icdGetInstanceProcAddr( instance, "vkGetDeviceProcAddr" );
    return get ? get( device, name ) : NULL;
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

/*
 * What Mesa's utility code calls that newlib, under libnx, does not have.
 * mesa-switch brings its own; nxvk leaves them to the program. Weak, so
 * whatever defines them already wins.
 */

/* newlib has memalign() but not posix_memalign(). */
__attribute__((weak)) int posix_memalign( void **memptr, size_t alignment, size_t size )
{
    void *p;

    if (alignment < sizeof(void *) || (alignment & (alignment - 1))) return EINVAL;
    if (!(p = memalign( alignment, size ))) return ENOMEM;
    *memptr = p;
    return 0;
}

/* One user: Mesa only compares them, to keep its caches to their owner. */
__attribute__((weak)) uid_t getuid( void ) { return 0; }
__attribute__((weak)) uid_t geteuid( void ) { return 0; }
__attribute__((weak)) gid_t getgid( void ) { return 0; }
__attribute__((weak)) gid_t getegid( void ) { return 0; }

/* driconf's application rules: no expression ever matches, so only the rules
 * by executable name apply. */
__attribute__((weak)) int regcomp( regex_t *preg, const char *regex, int cflags )
{
    (void)regex;
    (void)cflags;
    if (preg) preg->re_nsub = 0;
    return 0;
}

__attribute__((weak)) int regexec( const regex_t *preg, const char *string, size_t nmatch,
                                   regmatch_t *pmatch, int eflags )
{
    (void)preg;
    (void)string;
    (void)nmatch;
    (void)pmatch;
    (void)eflags;
    return REG_NOMATCH;
}

__attribute__((weak)) void regfree( regex_t *preg ) { (void)preg; }

/* Horizon has no signals: a new thread has no mask to inherit. */
__attribute__((weak)) int pthread_sigmask( int how, const sigset_t *set, sigset_t *old )
{
    (void)how;
    (void)set;
    if (old) memset( old, 0, sizeof(*old) );
    return 0;
}

/* One program has the card; the shader cache's lock is its own. */
__attribute__((weak)) int flock( int fd, int operation )
{
    (void)fd;
    (void)operation;
    return 0;
}

/* Only the disk cache's other backends use these; they fail rather than lie. */
__attribute__((weak)) int dirfd( DIR *dir )
{
    (void)dir;
    errno = ENOTSUP;
    return -1;
}

__attribute__((weak)) int getpwuid_r( uid_t uid, struct passwd *pwd, char *buffer, size_t size,
                                      struct passwd **result )
{
    (void)uid;
    (void)pwd;
    (void)buffer;
    (void)size;
    *result = NULL;
    return ENOTSUP;
}
