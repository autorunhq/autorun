/*
 * Vulkan for the Switch display driver
 *
 * Copyright 2026 Wine-NX contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#if defined(__SWITCH__) && defined(WINE_NX_MESA_SWITCH)

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "win32u_private.h"
#include "wine/vulkan_driver.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

/* The runtime hands the screen's NWindow to one surface at a time
 * (wine-nx-probe/source/runtime.c); OpenGL and Vulkan take turns with it. */
extern void *wine_nx_gl_acquire_window( void );
extern void wine_nx_gl_release_window( void );
extern void wine_nx_runtime_trace( const char *msg ) __attribute__((weak));

/* VK_NN_vi_surface, which Wine's Vulkan headers do not describe: mesa-switch's
 * loaderless NVK presents to an NWindow through it, and is linked in. */
struct nx_vi_surface_create_info
{
    VkStructureType sType;
    const void     *pNext;
    VkFlags         flags;
    void           *window;
};
#define NX_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN ((VkStructureType)1000062000)

extern VkResult wine_nx_vkCreateViSurfaceNN( VkInstance instance, const struct nx_vi_surface_create_info *info,
                                             const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface )
    __asm__("vkCreateViSurfaceNN");

extern VkResult nvk_switch_allocate_shared_memory(
    VkDevice device, const VkMemoryAllocateInfo *allocate_info,
    const VkAllocationCallbacks *allocator, uint32_t nvmap_id,
    VkDeviceMemory *memory_out );
extern bool nvk_switch_export_memory( VkDeviceMemory memory,
                                      uint32_t *nvmap_id_out,
                                      void **reference_out );
extern void nvk_switch_release_memory_reference( void *reference );

VkResult wine_nx_vk_allocate_shared_memory(
    VkDevice device, const VkMemoryAllocateInfo *allocate_info,
    uint32_t nvmap_id, VkDeviceMemory *memory_out )
{
    return nvk_switch_allocate_shared_memory( device, allocate_info, NULL,
                                               nvmap_id, memory_out );
}

BOOL wine_nx_vk_export_memory( VkDeviceMemory memory, uint32_t *nvmap_id,
                               void **reference )
{
    return nvk_switch_export_memory( memory, nvmap_id, reference );
}

void wine_nx_vk_release_memory_reference( void *reference )
{
    nvk_switch_release_memory_reference( reference );
}

static void nx_log( const char *format, ... )
{
    char buffer[256];
    va_list args;

    if (!&wine_nx_runtime_trace) return;
    va_start( args, format );
    vsnprintf( buffer, sizeof(buffer), format, args );
    va_end( args );
    wine_nx_runtime_trace( buffer );
}

/* Like an OpenGL window surface, a Vulkan surface covers the whole screen: the
 * Switch has one NWindow, and other windows are not shown meanwhile. */
static VkResult nx_vulkan_surface_create( struct client_surface *client, const struct vulkan_instance *instance,
                                          VkSurfaceKHR *handle )
{
    struct nx_vi_surface_create_info info = { .sType = NX_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN };
    HWND hwnd = client->hwnd;
    VkResult res;

    TRACE( "hwnd %p, raw %u, instance %p\n", hwnd, client->raw, instance );

    if (!(info.window = wine_nx_gl_acquire_window()))
    {
        ERR( "hwnd %p: the screen already has an OpenGL or Vulkan surface\n", hwnd );
        return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
    }
    if ((res = wine_nx_vkCreateViSurfaceNN( instance->host.instance, &info, NULL, handle )))
    {
        ERR( "hwnd %p: vkCreateViSurfaceNN failed, res %d\n", hwnd, res );
        nx_log( "[NXVK] hwnd %p: vkCreateViSurfaceNN failed, res %d", hwnd, res );
        wine_nx_gl_release_window();
        return res;
    }

    nx_log( "[NXVK] hwnd %p: a Vulkan surface has the screen", hwnd );
    return VK_SUCCESS;
}

static VkBool32 nx_get_physical_device_presentation_support( struct vulkan_physical_device *physical_device,
                                                             uint32_t index )
{
    TRACE( "%p %u\n", physical_device, index );
    return VK_TRUE;
}

static void nx_map_instance_extensions( struct vulkan_instance_extensions *extensions )
{
    if (extensions->has_VK_KHR_win32_surface) extensions->has_VK_NN_vi_surface = 1;
    if (extensions->has_VK_NN_vi_surface) extensions->has_VK_KHR_win32_surface = 1;
}

static void nx_map_device_extensions( struct vulkan_device_extensions *extensions )
{
    if (extensions->has_VK_EXT_external_memory_host)
        extensions->has_VK_KHR_external_memory_win32 = 1;
    if (extensions->has_VK_KHR_external_memory_win32)
        extensions->has_VK_EXT_external_memory_host = 1;
}

static const struct vulkan_driver_funcs nx_vulkan_driver_funcs =
{
    .p_vulkan_surface_create = nx_vulkan_surface_create,
    .p_get_physical_device_presentation_support = nx_get_physical_device_presentation_support,
    .p_map_instance_extensions = nx_map_instance_extensions,
    .p_map_device_extensions = nx_map_device_extensions,
};

UINT wine_nx_drv_VulkanInit( UINT version, void *vulkan_handle, const struct vulkan_driver_funcs **driver_funcs )
{
    TRACE( "version %u, vulkan_handle %p\n", version, vulkan_handle );

    if (version != WINE_VULKAN_DRIVER_VERSION)
    {
        ERR( "version mismatch, win32u wants %u but driver has %u\n", version, WINE_VULKAN_DRIVER_VERSION );
        return STATUS_INVALID_PARAMETER;
    }
    *driver_funcs = &nx_vulkan_driver_funcs;
    nx_log( "[NXVK] the Switch display driver serves Vulkan (handle %p)", vulkan_handle );
    return STATUS_SUCCESS;
}

#endif /* __SWITCH__ && WINE_NX_MESA_SWITCH */
