#!/usr/bin/env python3
"""Run dlls/win32u/vulkan.c's mouse arrow copy (nx_cursor_present) against a
fake Vulkan device: the arrow's runs are copied into the image about to be
shown, the present waits for that copy, and a hidden arrow, an image still
being copied into, or a failed submit leave the present as it was."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/win32u/vulkan.c').read_text()


def block(marker):
    """The definition that starts with marker, not a declaration of it."""
    start = source.index(marker)
    while source.index(';', start) < source.index('{', start):
        start = source.index(marker, start + 1)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + (';\n' if marker.startswith('struct ') else '\n')


fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windef.h>
#include <winbase.h>
#include <wine/vulkan.h>
#include "@ROOT@/horizon-wine/source/pointer_cursor.h"
#define ERR(...) ((void)0)
static void nx_vk_trace(const char *format, ...) { (void)format; }

#define H(type, value) ((type)(uintptr_t)(value))
enum { IMAGES = 3 };

struct mempool { char data[256]; size_t used; };
static void *mem_alloc(struct mempool *pool, size_t size)
{
    void *p = pool->data + pool->used;
    assert(pool->used + size <= sizeof(pool->data));
    pool->used += size;
    return p;
}

static int cursor_x, cursor_y, cursor_visible;
static int wine_nx_cursor_state(int *x, int *y, int *width, int *height)
{
    *x = cursor_x; *y = cursor_y; *width = 1280; *height = 720;
    return cursor_visible;
}

struct vulkan_instance { PFN_vkGetPhysicalDeviceMemoryProperties p_vkGetPhysicalDeviceMemoryProperties; };
struct vulkan_physical_device { struct vulkan_instance *instance; struct { VkPhysicalDevice physical_device; } host; };
struct vulkan_device {
    struct vulkan_physical_device *physical_device;
    struct { VkDevice device; } host;
#define F(name) PFN_##name p_##name;
    F(vkGetSwapchainImagesKHR) F(vkDeviceWaitIdle) F(vkCreateCommandPool) F(vkDestroyCommandPool)
    F(vkCreateBuffer) F(vkDestroyBuffer) F(vkGetBufferMemoryRequirements) F(vkAllocateMemory)
    F(vkFreeMemory) F(vkBindBufferMemory) F(vkMapMemory) F(vkUnmapMemory) F(vkAllocateCommandBuffers)
    F(vkCreateSemaphore) F(vkDestroySemaphore) F(vkCreateFence) F(vkDestroyFence) F(vkWaitForFences)
    F(vkResetFences) F(vkResetCommandBuffer) F(vkBeginCommandBuffer) F(vkEndCommandBuffer)
    F(vkCmdPipelineBarrier) F(vkCmdCopyBufferToImage) F(vkQueueSubmit)
#undef F
};
struct vulkan_queue { struct vulkan_device *device; struct { VkQueue queue; } host; struct { uint32_t queueFamilyIndex; } info; };
struct swapchain {
    struct { struct { VkSwapchainKHR swapchain; } host; } obj;
    VkExtent2D nx_extent;
    BOOL nx_osk_usable;
    struct nx_cursor_overlay *nx_cursor;
};
static struct swapchain chain;
static struct swapchain *swapchain_from_handle(VkSwapchainKHR handle)
{ assert(handle == H(VkSwapchainKHR, 0x50)); return &chain; }

static uint32_t arrow[POINTER_CURSOR_W * POINTER_CURSOR_H];
static VkResult fence_status, submit_result;
static int submits, copies, created, destroyed, waits_seen;
static VkBufferImageCopy copied[POINTER_CURSOR_MAX_SPANS];
static VkSubmitInfo last_submit;
static VkSemaphore last_wait;
static VkFence last_fence;

static void VKAPI_PTR memory_properties(VkPhysicalDevice d, VkPhysicalDeviceMemoryProperties *p)
{
    (void)d;
    memset(p, 0, sizeof(*p));
    p->memoryTypeCount = 2;
    p->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}
static VkResult VKAPI_PTR swapchain_images(VkDevice d, VkSwapchainKHR s, uint32_t *count, VkImage *images)
{
    (void)d; (void)s;
    if (images) for (uint32_t i = 0; i < *count; i++) images[i] = H(VkImage, 0x100 + i);
    else *count = IMAGES;
    return VK_SUCCESS;
}
static VkResult VKAPI_PTR wait_idle(VkDevice d) { (void)d; return VK_SUCCESS; }
static VkResult VKAPI_PTR create_pool(VkDevice d, const VkCommandPoolCreateInfo *info, const VkAllocationCallbacks *a, VkCommandPool *pool)
{
    (void)d; (void)a;
    assert(info->flags & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    created++; *pool = H(VkCommandPool, 0x200); return VK_SUCCESS;
}
static void VKAPI_PTR destroy_pool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a) { (void)d; (void)a; if (p) destroyed++; }
static VkResult VKAPI_PTR create_buffer(VkDevice d, const VkBufferCreateInfo *info, const VkAllocationCallbacks *a, VkBuffer *b)
{
    (void)d; (void)a;
    assert(info->size == sizeof(arrow) && info->usage == VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    created++; *b = H(VkBuffer, 0x300); return VK_SUCCESS;
}
static void VKAPI_PTR destroy_buffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *a) { (void)d; (void)a; if (b) destroyed++; }
static void VKAPI_PTR buffer_requirements(VkDevice d, VkBuffer b, VkMemoryRequirements *r)
{ (void)d; (void)b; r->size = sizeof(arrow); r->alignment = 4; r->memoryTypeBits = 3; }
static VkResult VKAPI_PTR allocate_memory(VkDevice d, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *a, VkDeviceMemory *m)
{
    (void)d; (void)a;
    assert(info->memoryTypeIndex == 1);
    created++; *m = H(VkDeviceMemory, 0x400); return VK_SUCCESS;
}
static void VKAPI_PTR free_memory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) { (void)d; (void)a; if (m) destroyed++; }
static VkResult VKAPI_PTR bind_memory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize o) { (void)d; (void)b; (void)m; (void)o; return VK_SUCCESS; }
static VkResult VKAPI_PTR map_memory(VkDevice d, VkDeviceMemory m, VkDeviceSize o, VkDeviceSize s, VkMemoryMapFlags f, void **p)
{ (void)d; (void)m; (void)o; (void)f; assert(s == sizeof(arrow)); *p = arrow; return VK_SUCCESS; }
static void VKAPI_PTR unmap_memory(VkDevice d, VkDeviceMemory m) { (void)d; (void)m; }
static VkResult VKAPI_PTR allocate_commands(VkDevice d, const VkCommandBufferAllocateInfo *info, VkCommandBuffer *c)
{
    static uintptr_t next = 0x500;
    (void)d; assert(info->commandBufferCount == 1);
    *c = (VkCommandBuffer)next++; return VK_SUCCESS;
}
static VkResult VKAPI_PTR create_semaphore(VkDevice d, const VkSemaphoreCreateInfo *i, const VkAllocationCallbacks *a, VkSemaphore *s)
{
    static uintptr_t next = 0x600;
    (void)d; (void)i; (void)a; created++; *s = H(VkSemaphore, next++); return VK_SUCCESS;
}
static void VKAPI_PTR destroy_semaphore(VkDevice d, VkSemaphore s, const VkAllocationCallbacks *a) { (void)d; (void)a; if (s) destroyed++; }
static VkResult VKAPI_PTR create_fence(VkDevice d, const VkFenceCreateInfo *info, const VkAllocationCallbacks *a, VkFence *f)
{
    static uintptr_t next = 0x700;
    (void)d; (void)a;
    assert(info->flags & VK_FENCE_CREATE_SIGNALED_BIT);
    created++; *f = H(VkFence, next++); return VK_SUCCESS;
}
static void VKAPI_PTR destroy_fence(VkDevice d, VkFence f, const VkAllocationCallbacks *a) { (void)d; (void)a; if (f) destroyed++; }
static VkResult VKAPI_PTR wait_fences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout)
{ (void)d; (void)f; (void)all; assert(n == 1 && timeout == 0); return fence_status; }
static VkResult VKAPI_PTR reset_fences(VkDevice d, uint32_t n, const VkFence *f) { (void)d; (void)f; assert(n == 1); return VK_SUCCESS; }
static VkResult VKAPI_PTR reset_command(VkCommandBuffer c, VkCommandBufferResetFlags f) { (void)c; (void)f; return VK_SUCCESS; }
static VkResult VKAPI_PTR begin_command(VkCommandBuffer c, const VkCommandBufferBeginInfo *b) { (void)c; (void)b; return VK_SUCCESS; }
static VkResult VKAPI_PTR end_command(VkCommandBuffer c) { (void)c; return VK_SUCCESS; }
static void VKAPI_PTR barrier(VkCommandBuffer c, VkPipelineStageFlags s, VkPipelineStageFlags t, VkDependencyFlags d,
                              uint32_t mc, const VkMemoryBarrier *m, uint32_t bc, const VkBufferMemoryBarrier *b,
                              uint32_t ic, const VkImageMemoryBarrier *i)
{ (void)c; (void)s; (void)t; (void)d; (void)mc; (void)m; (void)bc; (void)b; assert(ic == 1 && i->image); }
static void VKAPI_PTR copy(VkCommandBuffer c, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r)
{
    (void)c;
    assert(b == H(VkBuffer, 0x300) && i == H(VkImage, 0x101) && l == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    assert(n <= POINTER_CURSOR_MAX_SPANS);
    copies = n; memcpy(copied, r, n * sizeof(*r));
}
static VkResult VKAPI_PTR queue_submit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f)
{
    (void)q; assert(n == 1);
    submits++; last_submit = *s; last_fence = f;
    last_wait = s->waitSemaphoreCount ? s->pWaitSemaphores[0] : VK_NULL_HANDLE;
    waits_seen = s->waitSemaphoreCount;
    return submit_result;
}

@CODE@

static struct vulkan_instance instance = { memory_properties };
static struct vulkan_physical_device physical = { &instance, { 0 } };
static struct vulkan_device device = {
    &physical, { H(VkDevice, 1) },
    swapchain_images, wait_idle, create_pool, destroy_pool, create_buffer, destroy_buffer,
    buffer_requirements, allocate_memory, free_memory, bind_memory, map_memory, unmap_memory,
    allocate_commands, create_semaphore, destroy_semaphore, create_fence, destroy_fence, wait_fences,
    reset_fences, reset_command, begin_command, end_command, barrier, copy, queue_submit,
};
static struct vulkan_queue queue = { &device, { H(VkQueue, 2) }, { 0 } };

static int present(VkPresentInfoKHR *info, const VkSemaphore *app_wait)
{
    static const uint32_t index = 1;
    static const VkSwapchainKHR client = H(VkSwapchainKHR, 0x50);
    struct mempool pool = { .used = 0 };
    int before = submits;

    memset(info, 0, sizeof(*info));
    info->swapchainCount = 1;
    info->pImageIndices = &index;
    info->waitSemaphoreCount = 1;
    info->pWaitSemaphores = app_wait;
    nx_cursor_present(&queue, info, &client, &pool);
    return submits - before;
}

int main(void)
{
    const VkSemaphore app_wait = H(VkSemaphore, 0x99);
    struct pointer_cursor_span spans[POINTER_CURSOR_MAX_SPANS];
    VkPresentInfoKHR info;
    int count, i, j;

    chain.obj.host.swapchain = H(VkSwapchainKHR, 0x51);
    chain.nx_extent.width = 1280;
    chain.nx_extent.height = 720;
    chain.nx_osk_usable = TRUE;

    /* A hidden arrow is not drawn and sets nothing up. */
    cursor_x = 640; cursor_y = 360; cursor_visible = 0;
    assert(!present(&info, &app_wait) && info.pWaitSemaphores == &app_wait && !chain.nx_cursor);

    /* A shown one: its runs are copied, after what the present waited for,
     * and the present then waits for the copy. */
    cursor_visible = 1;
    assert(present(&info, &app_wait) == 1);
    count = pointer_cursor_spans(640, 360, 1280, 720, spans);
    assert(copies == count && count > POINTER_CURSOR_H);
    for (i = 0; i < count; i++)
    {
        assert(copied[i].imageOffset.x == spans[i].x && copied[i].imageOffset.y == spans[i].y);
        assert(copied[i].imageExtent.width == (uint32_t)spans[i].width && copied[i].imageExtent.height == 1);
        assert(copied[i].bufferRowLength == POINTER_CURSOR_W);
        assert(copied[i].bufferOffset == (VkDeviceSize)(spans[i].row * POINTER_CURSOR_W + spans[i].column) * 4);
    }
    assert(waits_seen == 1 && last_wait == app_wait && last_fence == chain.nx_cursor->per[1].fence);
    assert(info.waitSemaphoreCount == 1 && info.pWaitSemaphores[0] == chain.nx_cursor->per[1].done);
    for (j = 0; j < POINTER_CURSOR_H; j++)
        for (i = 0; i < POINTER_CURSOR_W; i++)
            assert(arrow[j * POINTER_CURSOR_W + i] == pointer_cursor_color(pointer_cursor_shape[j][i]));

    /* The arrow follows the screen into a smaller swapchain. */
    chain.nx_extent.width = 640;
    chain.nx_extent.height = 360;
    assert(present(&info, &app_wait) == 1);
    assert(copied[0].imageOffset.x == 320 && copied[0].imageOffset.y == 180);
    chain.nx_extent.width = 1280;
    chain.nx_extent.height = 720;

    /* An image whose last copy is still running goes without the arrow. */
    fence_status = VK_TIMEOUT;
    assert(!present(&info, &app_wait) && info.pWaitSemaphores == &app_wait);
    fence_status = VK_SUCCESS;

    /* A failed submit leaves the present alone and stops the copies. */
    submit_result = VK_ERROR_DEVICE_LOST;
    assert(present(&info, &app_wait) == 1 && info.pWaitSemaphores == &app_wait);
    submit_result = VK_SUCCESS;
    assert(!present(&info, &app_wait) && chain.nx_cursor->failed);

    /* Freeing destroys what was made. */
    nx_cursor_free(&device, chain.nx_cursor);
    assert(created == destroyed);
    puts("Vulkan cursor: hidden arrow, runs and present wait, scaling, busy image and failed submit passed");
    return 0;
}
'''

code = ''.join(block(marker) for marker in (
    'struct nx_cursor_image\n{', 'struct nx_cursor_overlay\n{',
    'static void nx_cursor_release(', 'static void nx_cursor_free(',
    'static BOOL nx_cursor_prepare(', 'static void nx_cursor_present('))
assert 'nx_cursor_present( queue, present_info, client_swapchains, &pool );' in source
with tempfile.TemporaryDirectory(prefix='vulkan-cursor-') as directory:
    directory = Path(directory)
    (directory / 'test.c').write_text(fixture.replace('@ROOT@', str(root)).replace('@CODE@', code))
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-D__SWITCH__', '-D_WIN64', '-DWINE_UNIX_LIB',
                    '-D__WINESRC__', '-I', str(root / 'include'), str(directory / 'test.c'),
                    '-o', str(directory / 'test')], check=True)
    subprocess.run([str(directory / 'test')], check=True)
