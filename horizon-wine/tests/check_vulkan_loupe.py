#!/usr/bin/env python3
"""Run dlls/win32u/vulkan.c's touch magnifier (nx_loupe_present) against a fake
Vulkan device: while the runtime says the magnifier is up, each present
records, for the image it is given, an enlargement of the part around the
finger into an image of its own (nearest, a whole number of times), a copy of
that above the finger, and a border and crosshair from a buffer of two lines of
pixels, then hands the image to the present. A magnifier that is down, a busy
image, a swapchain that cannot be read, a failed submit and a screen too small
for it leave the present as it was."""
import os
from pathlib import Path
import re
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
#include "@ROOT@/horizon-wine/source/touch_loupe.h"
#define ERR(...) ((void)0)
#undef min
#undef max
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
static void nx_vk_trace(const char *format, ...) { (void)format; }

#define H(type, value) ((type)(uintptr_t)(value))
enum { IMAGES = 3, CMD_BASE = 0x500, MAX_OPS = 16, LINE_BYTES = 2 * TOUCH_LOUPE_DST_W * 4 };
enum { OP_BARRIER = 1, OP_BLIT, OP_COPY, OP_LINES };

struct mempool { char data[256]; size_t used; };
static void *mem_alloc(struct mempool *pool, size_t size)
{
    void *p = pool->data + pool->used;
    assert(pool->used + size <= sizeof(pool->data));
    pool->used += size;
    return p;
}

static int shown, finger_x, finger_y, finger_zoom = 3, screen_w = 1280, screen_h = 720;
static int wine_nx_loupe_state(int *x, int *y, int *width, int *height, int *zoom)
{ *x = finger_x; *y = finger_y; *width = screen_w; *height = screen_h; *zoom = finger_zoom; return shown; }
@DEFINES@

struct vulkan_instance { PFN_vkGetPhysicalDeviceMemoryProperties p_vkGetPhysicalDeviceMemoryProperties; };
struct vulkan_physical_device { struct vulkan_instance *instance; struct { VkPhysicalDevice physical_device; } host; };
struct vulkan_device {
    struct vulkan_physical_device *physical_device;
    struct { VkDevice device; } host;
#define F(name) PFN_##name p_##name;
    F(vkGetSwapchainImagesKHR) F(vkDeviceWaitIdle) F(vkCreateCommandPool) F(vkDestroyCommandPool)
    F(vkCreateImage) F(vkDestroyImage) F(vkGetImageMemoryRequirements) F(vkAllocateMemory) F(vkFreeMemory)
    F(vkBindImageMemory) F(vkCreateBuffer) F(vkDestroyBuffer) F(vkGetBufferMemoryRequirements)
    F(vkBindBufferMemory) F(vkMapMemory) F(vkUnmapMemory) F(vkAllocateCommandBuffers) F(vkCreateSemaphore)
    F(vkDestroySemaphore) F(vkCreateFence) F(vkDestroyFence) F(vkWaitForFences) F(vkResetFences)
    F(vkResetCommandBuffer) F(vkBeginCommandBuffer) F(vkEndCommandBuffer) F(vkCmdPipelineBarrier)
    F(vkCmdBlitImage) F(vkCmdCopyImage) F(vkCmdCopyBufferToImage) F(vkQueueSubmit)
#undef F
};
struct vulkan_queue { struct vulkan_device *device; struct { VkQueue queue; } host; struct { uint32_t queueFamilyIndex; } info; };
struct swapchain {
    struct { struct { VkSwapchainKHR swapchain; } host; } obj;
    VkFormat nx_format;
    VkExtent2D nx_extent;
    BOOL nx_loupe_usable;
    struct nx_loupe_overlay *nx_loupe;
};
static struct swapchain chain;
static struct swapchain *swapchain_from_handle(VkSwapchainKHR handle)
{ assert(handle == H(VkSwapchainKHR, 0x50)); return &chain; }

static uint32_t line_pixels[2 * TOUCH_LOUPE_DST_W];
static int ops[IMAGES][MAX_OPS], op_count[IMAGES];
static VkImageLayout final_layout[IMAGES];
static VkImageBlit blitted[IMAGES];
static VkImageCopy copied[IMAGES];
static VkBufferImageCopy lines[IMAGES][NX_LOUPE_MAX_REGIONS];
static uint32_t line_count[IMAGES];
static VkFilter blit_filter;
static VkImageCreateInfo image_made;
static VkBufferCreateInfo buffer_made;
static VkResult submit_result, wait_result;
static int submits, created, destroyed, unmaps, memory_types[2], memory_count;
static VkSubmitInfo last_submit;
static VkSemaphore last_wait;

static int slot(VkCommandBuffer c) { return (int)((uintptr_t)c - CMD_BASE); }
static void add(VkCommandBuffer c, int op) { int s = slot(c); assert(s >= 0 && s < IMAGES && op_count[s] < MAX_OPS); ops[s][op_count[s]++] = op; }

static void VKAPI_PTR memory_properties(VkPhysicalDevice d, VkPhysicalDeviceMemoryProperties *p)
{
    (void)d;
    memset(p, 0, sizeof(*p));
    p->memoryTypeCount = 2;
    p->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    p->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
}
static VkResult VKAPI_PTR swapchain_images(VkDevice d, VkSwapchainKHR s, uint32_t *count, VkImage *images)
{
    (void)d; (void)s;
    if (images) for (uint32_t i = 0; i < *count; i++) images[i] = H(VkImage, 0x100 + i);
    else *count = IMAGES;
    return VK_SUCCESS;
}
static VkResult VKAPI_PTR wait_idle(VkDevice d) { (void)d; return VK_SUCCESS; }
static VkResult VKAPI_PTR create_pool(VkDevice d, const VkCommandPoolCreateInfo *i, const VkAllocationCallbacks *a, VkCommandPool *pool)
{ (void)d; (void)i; (void)a; created++; *pool = H(VkCommandPool, 0x200); return VK_SUCCESS; }
static void VKAPI_PTR destroy_pool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a) { (void)d; (void)a; if (p) destroyed++; }
static VkResult VKAPI_PTR create_image(VkDevice d, const VkImageCreateInfo *info, const VkAllocationCallbacks *a, VkImage *image)
{ (void)d; (void)a; image_made = *info; created++; *image = H(VkImage, 0x800); return VK_SUCCESS; }
static void VKAPI_PTR destroy_image(VkDevice d, VkImage i, const VkAllocationCallbacks *a) { (void)d; (void)a; if (i) destroyed++; }
static void VKAPI_PTR image_requirements(VkDevice d, VkImage i, VkMemoryRequirements *r)
{ (void)d; (void)i; r->size = TOUCH_LOUPE_DST_W * TOUCH_LOUPE_DST_H * 4; r->alignment = 4096; r->memoryTypeBits = 3; }
static VkResult VKAPI_PTR create_buffer(VkDevice d, const VkBufferCreateInfo *info, const VkAllocationCallbacks *a, VkBuffer *b)
{ (void)d; (void)a; buffer_made = *info; created++; *b = H(VkBuffer, 0x900); return VK_SUCCESS; }
static void VKAPI_PTR destroy_buffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *a) { (void)d; (void)a; if (b) destroyed++; }
static void VKAPI_PTR buffer_requirements(VkDevice d, VkBuffer b, VkMemoryRequirements *r)
{ (void)d; (void)b; r->size = LINE_BYTES; r->alignment = 4; r->memoryTypeBits = 3; }
static VkResult VKAPI_PTR allocate_memory(VkDevice d, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *a, VkDeviceMemory *m)
{
    (void)d; (void)a;
    assert(memory_count < 2);
    memory_types[memory_count++] = (int)info->memoryTypeIndex;
    created++; *m = H(VkDeviceMemory, 0x400 + memory_count); return VK_SUCCESS;
}
static void VKAPI_PTR free_memory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) { (void)d; (void)a; if (m) destroyed++; }
static VkResult VKAPI_PTR bind_image_memory(VkDevice d, VkImage i, VkDeviceMemory m, VkDeviceSize o) { (void)d; (void)i; (void)m; (void)o; return VK_SUCCESS; }
static VkResult VKAPI_PTR bind_buffer_memory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize o) { (void)d; (void)b; (void)m; (void)o; return VK_SUCCESS; }
static VkResult VKAPI_PTR map_memory(VkDevice d, VkDeviceMemory m, VkDeviceSize o, VkDeviceSize s, VkMemoryMapFlags f, void **p)
{ (void)d; (void)m; (void)o; (void)f; assert(s == LINE_BYTES); *p = line_pixels; return VK_SUCCESS; }
static void VKAPI_PTR unmap_memory(VkDevice d, VkDeviceMemory m) { (void)d; (void)m; unmaps++; }
static VkResult VKAPI_PTR allocate_commands(VkDevice d, const VkCommandBufferAllocateInfo *info, VkCommandBuffer *c)
{
    static uintptr_t next = CMD_BASE;
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
{ (void)d; (void)f; (void)all; assert(n == 1 && timeout == NX_LOUPE_FENCE_WAIT_NS); return wait_result; }
static VkResult VKAPI_PTR reset_fences(VkDevice d, uint32_t n, const VkFence *f) { (void)d; (void)f; assert(n == 1); return VK_SUCCESS; }
static VkResult VKAPI_PTR reset_command(VkCommandBuffer c, VkCommandBufferResetFlags f) { (void)f; op_count[slot(c)] = 0; return VK_SUCCESS; }
static VkResult VKAPI_PTR begin_command(VkCommandBuffer c, const VkCommandBufferBeginInfo *b)
{ (void)c; (void)b; return VK_SUCCESS; }
static VkResult VKAPI_PTR end_command(VkCommandBuffer c) { (void)c; return VK_SUCCESS; }
static void VKAPI_PTR barrier(VkCommandBuffer c, VkPipelineStageFlags s, VkPipelineStageFlags t, VkDependencyFlags d,
                              uint32_t mc, const VkMemoryBarrier *m, uint32_t bc, const VkBufferMemoryBarrier *b,
                              uint32_t ic, const VkImageMemoryBarrier *i)
{
    (void)s; (void)t; (void)d; (void)mc; (void)m; (void)b;
    assert(bc == 0 && ic >= 1 && ic <= 2);
    add(c, OP_BARRIER);
    final_layout[slot(c)] = i[0].newLayout;
}
static void VKAPI_PTR blit(VkCommandBuffer c, VkImage si, VkImageLayout sl, VkImage di, VkImageLayout dl,
                           uint32_t n, const VkImageBlit *r, VkFilter f)
{
    assert(si == H(VkImage, 0x100 + slot(c)) && di == H(VkImage, 0x800) && n == 1);
    assert(sl == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && dl == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    add(c, OP_BLIT); blitted[slot(c)] = *r; blit_filter = f;
}
static void VKAPI_PTR copy(VkCommandBuffer c, VkImage si, VkImageLayout sl, VkImage di, VkImageLayout dl,
                           uint32_t n, const VkImageCopy *r)
{
    assert(si == H(VkImage, 0x800) && di == H(VkImage, 0x100 + slot(c)) && n == 1);
    assert(sl == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && dl == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    add(c, OP_COPY); copied[slot(c)] = *r;
}
static void VKAPI_PTR copy_buffer(VkCommandBuffer c, VkBuffer b, VkImage di, VkImageLayout dl, uint32_t n,
                                  const VkBufferImageCopy *r)
{
    assert(b == H(VkBuffer, 0x900) && di == H(VkImage, 0x100 + slot(c)) && dl == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    assert(n <= NX_LOUPE_MAX_REGIONS);
    add(c, OP_LINES); line_count[slot(c)] = n; memcpy(lines[slot(c)], r, n * sizeof(*r));
}
static VkResult VKAPI_PTR queue_submit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f)
{
    (void)q; assert(n == 1 && f);
    submits++; last_submit = *s;
    last_wait = s->waitSemaphoreCount ? s->pWaitSemaphores[0] : VK_NULL_HANDLE;
    return submit_result;
}

@CODE@

static struct vulkan_instance instance = { memory_properties };
static struct vulkan_physical_device physical = { &instance, { 0 } };
static struct vulkan_device device = {
    &physical, { H(VkDevice, 1) },
    .p_vkGetSwapchainImagesKHR = swapchain_images, .p_vkDeviceWaitIdle = wait_idle,
    .p_vkCreateCommandPool = create_pool, .p_vkDestroyCommandPool = destroy_pool,
    .p_vkCreateImage = create_image, .p_vkDestroyImage = destroy_image,
    .p_vkGetImageMemoryRequirements = image_requirements, .p_vkAllocateMemory = allocate_memory,
    .p_vkFreeMemory = free_memory, .p_vkBindImageMemory = bind_image_memory,
    .p_vkCreateBuffer = create_buffer, .p_vkDestroyBuffer = destroy_buffer,
    .p_vkGetBufferMemoryRequirements = buffer_requirements, .p_vkBindBufferMemory = bind_buffer_memory,
    .p_vkMapMemory = map_memory, .p_vkUnmapMemory = unmap_memory,
    .p_vkAllocateCommandBuffers = allocate_commands, .p_vkCreateSemaphore = create_semaphore,
    .p_vkDestroySemaphore = destroy_semaphore, .p_vkCreateFence = create_fence, .p_vkDestroyFence = destroy_fence,
    .p_vkWaitForFences = wait_fences, .p_vkResetFences = reset_fences, .p_vkResetCommandBuffer = reset_command,
    .p_vkBeginCommandBuffer = begin_command, .p_vkEndCommandBuffer = end_command, .p_vkCmdPipelineBarrier = barrier,
    .p_vkCmdBlitImage = blit, .p_vkCmdCopyImage = copy, .p_vkCmdCopyBufferToImage = copy_buffer,
    .p_vkQueueSubmit = queue_submit,
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
    nx_loupe_present(&queue, info, &client, &pool);
    return submits - before;
}

static void check_regions(int image, const struct touch_loupe_rect *r, uint32_t expected)
{
    uint32_t i;

    assert(line_count[image] == expected);
    for (i = 0; i < line_count[image]; i++)
    {
        const VkBufferImageCopy *l = &lines[image][i];

        assert(l->imageOffset.x >= r->dst_x && l->imageOffset.y >= r->dst_y);
        assert(l->imageOffset.x + (int)l->imageExtent.width <= r->dst_x + r->dst_w);
        assert(l->imageOffset.y + (int)l->imageExtent.height <= r->dst_y + r->dst_h);
        assert(l->bufferRowLength == l->imageExtent.width);
        assert(l->bufferOffset == (i < 4 ? 0 : TOUCH_LOUPE_DST_W * 4));  /* the border's line, then the crosshair's */
        assert(l->bufferRowLength * l->imageExtent.height <= TOUCH_LOUPE_DST_W);  /* it fits in the line */
    }
}

int main(void)
{
    const VkSemaphore app_wait = H(VkSemaphore, 0x99);
    const int expected_ops[] = { OP_BARRIER, OP_BLIT, OP_BARRIER, OP_COPY, OP_BARRIER, OP_LINES, OP_BARRIER };
    struct touch_loupe_rect r;
    VkPresentInfoKHR info;
    int j;

    chain.obj.host.swapchain = H(VkSwapchainKHR, 0x51);
    chain.nx_format = VK_FORMAT_B8G8R8A8_UNORM;
    chain.nx_extent.width = 1280;
    chain.nx_extent.height = 720;
    chain.nx_loupe_usable = TRUE;

    /* Down: nothing is set up and the present goes as it was. */
    assert(!present(&info, &app_wait) && info.pWaitSemaphores == &app_wait && !chain.nx_loupe);

    /* Up, above a finger in the lower middle of the screen. */
    shown = 1;
    finger_x = 640;
    finger_y = 500;
    assert(touch_loupe_layout(1280, 720, 640, 500, finger_zoom, &r));
    assert(present(&info, &app_wait) == 1);
    assert(image_made.extent.width == TOUCH_LOUPE_DST_W && image_made.extent.height == TOUCH_LOUPE_DST_H);
    assert(image_made.format == VK_FORMAT_B8G8R8A8_UNORM);
    assert(image_made.usage == (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    assert(buffer_made.size == LINE_BYTES && buffer_made.usage == VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    assert(memory_types[0] == 1 && memory_types[1] == 0);  /* the image on the device, the lines where the host writes */
    assert(unmaps == 1);
    for (j = 0; j < TOUCH_LOUPE_DST_W; j++)
        assert(line_pixels[j] == 0xffffffffu && line_pixels[TOUCH_LOUPE_DST_W + j] == 0xffff00ffu);
    assert(op_count[1] == (int)(sizeof(expected_ops) / sizeof(*expected_ops)));
    for (j = 0; j < op_count[1]; j++) assert(ops[1][j] == expected_ops[j]);
    assert(final_layout[1] == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    assert(blit_filter == VK_FILTER_NEAREST);  /* the program's pixels stay sharp */
    assert(blitted[1].srcOffsets[0].x == r.src_x && blitted[1].srcOffsets[0].y == r.src_y);
    assert(blitted[1].srcOffsets[1].x == r.src_x + r.src_w && blitted[1].srcOffsets[1].y == r.src_y + r.src_h);
    assert(blitted[1].dstOffsets[1].x == r.dst_w && blitted[1].dstOffsets[1].y == r.dst_h);
    assert(r.dst_w == 3 * r.src_w && r.dst_h == 3 * r.src_h && r.src_w == 160 && r.src_h == 100);
    assert(copied[1].dstOffset.x == r.dst_x && copied[1].dstOffset.y == r.dst_y);
    assert(copied[1].extent.width == (uint32_t)r.dst_w && copied[1].extent.height == (uint32_t)r.dst_h);
    assert(r.dst_y + r.dst_h <= finger_y - TOUCH_LOUPE_GAP + 1);  /* above the finger */
    check_regions(1, &r, 8);  /* the border's four sides and the crosshair's four arms */
    assert(last_wait == app_wait);
    assert(info.waitSemaphoreCount == 1 && info.pWaitSemaphores[0] == last_submit.pSignalSemaphores[0]);

    /* In the lowest corner, everything stays on the screen. */
    finger_x = 0;
    finger_y = 719;
    assert(touch_loupe_layout(1280, 720, 0, 719, finger_zoom, &r));
    assert(present(&info, &app_wait) == 1);
    assert(blitted[1].srcOffsets[0].x == 0 && blitted[1].srcOffsets[1].y == 720);
    check_regions(1, &r, 6);  /* the crosshair is at the magnifier's corner: two arms are cut off */

    /* Near the top it goes below the finger. */
    finger_x = 640;
    finger_y = 50;
    assert(touch_loupe_layout(1280, 720, 640, 50, finger_zoom, &r) && r.dst_y == 50 + TOUCH_LOUPE_GAP);
    assert(present(&info, &app_wait) == 1 && copied[1].dstOffset.y == r.dst_y);
    check_regions(1, &r, 8);

    /* Another zoom takes another part into the same magnifier: the size stays, the part changes. */
    finger_x = 640;
    finger_y = 500;
    for (finger_zoom = TOUCH_LOUPE_ZOOM_MIN; finger_zoom <= TOUCH_LOUPE_ZOOM_MAX; finger_zoom++)
    {
        assert(touch_loupe_layout(1280, 720, 640, 500, finger_zoom, &r));
        assert(present(&info, &app_wait) == 1);
        assert(blitted[1].srcOffsets[1].x - blitted[1].srcOffsets[0].x == TOUCH_LOUPE_DST_W / finger_zoom);
        assert(blitted[1].srcOffsets[1].y - blitted[1].srcOffsets[0].y == TOUCH_LOUPE_DST_H / finger_zoom);
        assert(blitted[1].dstOffsets[1].x == TOUCH_LOUPE_DST_W && blitted[1].dstOffsets[1].y == TOUCH_LOUPE_DST_H);
        assert(copied[1].extent.width == TOUCH_LOUPE_DST_W && copied[1].extent.height == TOUCH_LOUPE_DST_H);
        check_regions(1, &r, 8);
    }
    /* A zoom the magnifier does not have draws nothing. */
    finger_zoom = TOUCH_LOUPE_ZOOM_MAX + 1;
    assert(!present(&info, &app_wait) && info.pWaitSemaphores == &app_wait);
    finger_zoom = TOUCH_LOUPE_ZOOM_DEFAULT;

    /* An image the GPU is still using is left alone. */
    wait_result = VK_TIMEOUT;
    assert(!present(&info, &app_wait) && info.pWaitSemaphores == &app_wait);
    wait_result = VK_SUCCESS;

    /* A swapchain that cannot be read from is not touched. */
    chain.nx_loupe_usable = FALSE;
    assert(!present(&info, &app_wait) && info.pWaitSemaphores == &app_wait);
    chain.nx_loupe_usable = TRUE;

    /* The finger's place is taken to the swapchain's own pixels. */
    chain.nx_extent.width = 1920;
    chain.nx_extent.height = 1080;
    finger_x = 640;
    finger_y = 360;
    assert(touch_loupe_layout(1920, 1080, 960, 540, finger_zoom, &r));
    assert(present(&info, &app_wait) == 1 && blitted[1].srcOffsets[0].x == r.src_x);
    assert(blitted[1].srcOffsets[0].y == r.src_y);
    chain.nx_extent.width = 1280;
    chain.nx_extent.height = 720;

    /* A failed submit leaves the present alone and stops the magnifier. */
    submit_result = VK_ERROR_DEVICE_LOST;
    assert(present(&info, &app_wait) == 1 && info.pWaitSemaphores == &app_wait);
    submit_result = VK_SUCCESS;
    assert(!present(&info, &app_wait) && chain.nx_loupe->failed);

    /* Freeing destroys everything that was made. */
    nx_loupe_free(&device, chain.nx_loupe);
    assert(created == destroyed && unmaps == 1);

    /* A screen too small for the magnifier has none, and nothing is made for it. */
    chain.nx_loupe = NULL;
    chain.nx_extent.width = 400;
    chain.nx_extent.height = 720;
    created = destroyed = 0;
    assert(!present(&info, &app_wait) && info.pWaitSemaphores == &app_wait && !chain.nx_loupe && !created);
    puts("Vulkan touch magnifier: down, commands and layouts, zooms, corners, below the finger, busy image, "
         "unusable swapchain, scaled place, failed submit, cleanup and small screen passed");
    return 0;
}
'''

code = ''.join(block(marker) for marker in (
    'struct nx_loupe_image\n{', 'struct nx_loupe_overlay\n{',
    'static void nx_loupe_release(', 'static void nx_loupe_free(', 'static uint32_t nx_loupe_memory_type(',
    'static VkImageMemoryBarrier nx_loupe_barrier(', 'static BOOL nx_loupe_prepare(',
    'static int nx_loupe_region(', 'static int nx_loupe_regions(', 'static void nx_loupe_present('))
defines = ''.join(line + '\n' for line in source.splitlines() if line.startswith('#define NX_LOUPE_'))
with tempfile.TemporaryDirectory(prefix='vulkan-loupe-') as directory:
    directory = Path(directory)
    (directory / 'test.c').write_text(fixture.replace('@ROOT@', str(root)).replace('@DEFINES@', defines)
                                      .replace('@CODE@', code))
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-D__SWITCH__', '-D_WIN64', '-DWINE_UNIX_LIB',
                    '-D__WINESRC__', '-I', str(root / 'include'), str(directory / 'test.c'),
                    '-o', str(directory / 'test')], check=True)
    subprocess.run([str(directory / 'test')], check=True)
