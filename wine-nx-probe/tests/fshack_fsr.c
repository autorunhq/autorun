/* Runs the Upscaling setting's FSR 1.0 passes, as dlls/win32u/vulkan.c ships
 * them (fshack_fsr_spv.h), on a real Vulkan driver: Mesa's lavapipe in
 * tests/check-fshack-fsr.sh. The images, views, descriptors, barriers and
 * push constants are set up the way vulkan.c sets them for a scaled swapchain:
 * an sRGB game image read through a UNORM view, EASU into an RGBA8 image the
 * size of the screen, RCAS from there into a BGRA8 screen buffer.
 *
 *   fshack_fsr [OUT_DIR]   also writes the inputs and results as PNGs
 */
#include <assert.h>
#include <math.h>
#include <png.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "../../dlls/win32u/fshack_fsr_spv.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_) { fprintf( stderr, "%s: %d\n", #x, r_ ); exit( 1 ); } } while (0)

static VkDevice dev;
static VkPhysicalDevice phys;
static VkQueue queue;
static uint32_t family;

struct constants { float offset[2], extents[2], src_extents[2], sharpness, padding; };

static uint32_t memory_type( uint32_t bits, VkMemoryPropertyFlags flags )
{
    VkPhysicalDeviceMemoryProperties props;
    uint32_t i;

    vkGetPhysicalDeviceMemoryProperties( phys, &props );
    for (i = 0; i < props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
    abort();
}

static VkDeviceMemory bind_image( VkImage image )
{
    VkMemoryRequirements req;
    VkMemoryAllocateInfo info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkDeviceMemory memory;

    vkGetImageMemoryRequirements( dev, image, &req );
    info.allocationSize = req.size;
    info.memoryTypeIndex = memory_type( req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
    CHECK( vkAllocateMemory( dev, &info, NULL, &memory ) );
    CHECK( vkBindImageMemory( dev, image, memory, 0 ) );
    return memory;
}

static VkImage make_image( VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageCreateFlags flags )
{
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImage image;

    info.flags = flags;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = w;
    info.extent.height = h;
    info.extent.depth = 1;
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    CHECK( vkCreateImage( dev, &info, NULL, &image ) );
    bind_image( image );
    return image;
}

static VkImageView make_view( VkImage image, VkFormat format )
{
    VkImageViewCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkImageView view;

    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    info.subresourceRange.levelCount = info.subresourceRange.layerCount = 1;
    CHECK( vkCreateImageView( dev, &info, NULL, &view ) );
    return view;
}

static VkBuffer make_buffer( VkDeviceSize size, VkBufferUsageFlags usage, void **map )
{
    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;
    VkDeviceMemory memory;
    VkBuffer buffer;

    info.size = size;
    info.usage = usage;
    CHECK( vkCreateBuffer( dev, &info, NULL, &buffer ) );
    vkGetBufferMemoryRequirements( dev, buffer, &req );
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type( req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );
    CHECK( vkAllocateMemory( dev, &alloc, NULL, &memory ) );
    CHECK( vkBindBufferMemory( dev, buffer, memory, 0 ) );
    CHECK( vkMapMemory( dev, memory, 0, size, 0, map ) );
    return buffer;
}

static VkPipeline make_pipeline( VkPipelineLayout layout, const uint32_t *code, size_t size )
{
    VkShaderModuleCreateInfo module_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkComputePipelineCreateInfo info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    VkShaderModule module;
    VkPipeline pipeline;

    module_info.codeSize = size;
    module_info.pCode = code;
    CHECK( vkCreateShaderModule( dev, &module_info, NULL, &module ) );
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = layout;
    CHECK( vkCreateComputePipelines( dev, VK_NULL_HANDLE, 1, &info, NULL, &pipeline ) );
    vkDestroyShaderModule( dev, module, NULL );
    return pipeline;
}

static void barrier( VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                     VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage )
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };

    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier( cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b );
}

/* src is BGRA, sRGB-encoded, w x h; out gets the screen's BGRA, W x H. */
static void upscale( const uint8_t *src, uint32_t w, uint32_t h, uint8_t *out, uint32_t W, uint32_t H, float sharpness,
                     struct constants *c )
{
    VkImage game, mid, screen;
    VkImageView gamma_view, mid_view, screen_view;
    VkSampler sampler;
    VkSamplerCreateInfo sampler_info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkDescriptorSetLayoutBinding bindings[3] = {{0}};
    VkDescriptorSetLayoutCreateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkDescriptorSetLayout set_layout;
    VkPushConstantRange range = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(struct constants) };
    VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout layout;
    VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 } };
    VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorPool pool;
    VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorSet set;
    VkDescriptorImageInfo images[3] = {{0}};
    VkWriteDescriptorSet writes[3] = {{0}};
    VkPipeline easu, rcas;
    VkCommandPoolCreateInfo cmd_pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandPool cmd_pool;
    VkCommandBufferAllocateInfo cmd_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkCommandBuffer cmd;
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkBufferImageCopy copy = {0};
    VkBuffer upload, download;
    void *up, *down;
    float scale, width, height;
    int i;

    /* The game's image: sRGB with a UNORM view, as vulkan.c makes it for FSR. */
    game = make_image( VK_FORMAT_B8G8R8A8_SRGB, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT );
    gamma_view = make_view( game, VK_FORMAT_B8G8R8A8_UNORM );
    mid = make_image( VK_FORMAT_R8G8B8A8_UNORM, W, H, VK_IMAGE_USAGE_STORAGE_BIT, 0 );
    mid_view = make_view( mid, VK_FORMAT_R8G8B8A8_UNORM );
    screen = make_image( VK_FORMAT_B8G8R8A8_UNORM, W, H, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0 );
    screen_view = make_view( screen, VK_FORMAT_B8G8R8A8_UNORM );

    sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxAnisotropy = 1;
    CHECK( vkCreateSampler( dev, &sampler_info, NULL, &sampler ) );

    for (i = 0; i < 3; i++)
    {
        bindings[i].binding = i;
        bindings[i].descriptorCount = 1;
        bindings[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    set_info.bindingCount = 3;
    set_info.pBindings = bindings;
    CHECK( vkCreateDescriptorSetLayout( dev, &set_info, NULL, &set_layout ) );
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &range;
    CHECK( vkCreatePipelineLayout( dev, &layout_info, NULL, &layout ) );
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = sizes;
    CHECK( vkCreateDescriptorPool( dev, &pool_info, NULL, &pool ) );
    alloc.descriptorPool = pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &set_layout;
    CHECK( vkAllocateDescriptorSets( dev, &alloc, &set ) );
    images[0].sampler = sampler;
    images[0].imageView = gamma_view;
    images[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    images[1].imageView = screen_view;
    images[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    images[2].imageView = mid_view;
    images[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    for (i = 0; i < 3; i++)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = bindings[i].descriptorType;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets( dev, 3, writes, 0, NULL );
    easu = make_pipeline( layout, easu_comp_spv, sizeof(easu_comp_spv) );
    rcas = make_pipeline( layout, rcas_comp_spv, sizeof(rcas_comp_spv) );

    /* vulkan.c's rectangle for FSR on the Switch: aspect kept, whole pixels. */
    scale = fminf( (float)W / w, (float)H / h );
    width = fminf( floorf( w * scale + 0.5f ), W );
    height = fminf( floorf( h * scale + 0.5f ), H );
    memset( c, 0, sizeof(*c) );
    c->offset[0] = floorf( (W - width) / 2 );
    c->offset[1] = floorf( (H - height) / 2 );
    c->extents[0] = width;
    c->extents[1] = height;
    c->src_extents[0] = w;
    c->src_extents[1] = h;
    c->sharpness = sharpness > 0 ? exp2f( -2.0f * (1.0f - sharpness) ) : 0;

    upload = make_buffer( (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &up );
    download = make_buffer( (VkDeviceSize)W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &down );
    memcpy( up, src, (size_t)w * h * 4 );

    cmd_pool_info.queueFamilyIndex = family;
    CHECK( vkCreateCommandPool( dev, &cmd_pool_info, NULL, &cmd_pool ) );
    cmd_info.commandPool = cmd_pool;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    CHECK( vkAllocateCommandBuffers( dev, &cmd_info, &cmd ) );
    CHECK( vkBeginCommandBuffer( cmd, &begin ) );
    barrier( cmd, game, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT );
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = w;
    copy.imageExtent.height = h;
    copy.imageExtent.depth = 1;
    vkCmdCopyBufferToImage( cmd, upload, game, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy );
    barrier( cmd, game, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );
    /* From here, record_compute_cmd's order. */
    barrier( cmd, screen, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );
    barrier( cmd, mid, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );
    vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, easu );
    vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL );
    vkCmdPushConstants( cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(*c), c );
    vkCmdDispatch( cmd, (W + 7) / 8, (H + 7) / 8, 1 );
    barrier( cmd, mid, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
             VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );
    vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rcas );
    vkCmdDispatch( cmd, (W + 7) / 8, (H + 7) / 8, 1 );
    barrier( cmd, screen, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
             VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT );
    copy.imageExtent.width = W;
    copy.imageExtent.height = H;
    vkCmdCopyImageToBuffer( cmd, screen, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, download, 1, &copy );
    CHECK( vkEndCommandBuffer( cmd ) );
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    CHECK( vkQueueSubmit( queue, 1, &submit, VK_NULL_HANDLE ) );
    CHECK( vkQueueWaitIdle( queue ) );
    memcpy( out, down, (size_t)W * H * 4 );
    CHECK( vkDeviceWaitIdle( dev ) );
}

static void save( const char *dir, const char *name, const uint8_t *bgra, uint32_t w, uint32_t h )
{
    png_image image = {0};
    char path[512];

    if (!dir) return;
    snprintf( path, sizeof(path), "%s/%s", dir, name );
    image.version = PNG_IMAGE_VERSION;
    image.width = w;
    image.height = h;
    image.format = PNG_FORMAT_BGRA;
    assert( png_image_write_to_file( &image, path, 0, bgra, 0, NULL ) );
}

static uint8_t *pixel( uint8_t *image, uint32_t stride, uint32_t x, uint32_t y )
{
    return image + ((size_t)y * stride + x) * 4;
}

/* What the plain blit would show, for the comparison PNG: bilinear on the
 * same rectangle, filtered in the same (perceptual) values. */
static void bilinear( const uint8_t *src, uint32_t w, uint32_t h, uint8_t *out, uint32_t W, uint32_t H,
                      const struct constants *c )
{
    uint32_t x, y, k;

    memset( out, 0, (size_t)W * H * 4 );
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
        {
            float u = (x + 0.5f - c->offset[0]) / c->extents[0] * w - 0.5f;
            float v = (y + 0.5f - c->offset[1]) / c->extents[1] * h - 0.5f;
            int x0, y0, x1, y1;
            float fx, fy;

            out[((size_t)y * W + x) * 4 + 3] = 255;
            if (x < c->offset[0] || y < c->offset[1] || x >= c->offset[0] + c->extents[0] ||
                y >= c->offset[1] + c->extents[1]) continue;
            x0 = (int)floorf( u ); y0 = (int)floorf( v );
            fx = u - x0; fy = v - y0;
            x1 = x0 + 1; y1 = y0 + 1;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > (int)w - 1) x1 = w - 1;
            if (y1 > (int)h - 1) y1 = h - 1;
            if (x0 > (int)w - 1) x0 = w - 1;
            if (y0 > (int)h - 1) y0 = h - 1;
            for (k = 0; k < 3; k++)
            {
                float a = src[((size_t)y0 * w + x0) * 4 + k], b = src[((size_t)y0 * w + x1) * 4 + k];
                float d = src[((size_t)y1 * w + x0) * 4 + k], e = src[((size_t)y1 * w + x1) * 4 + k];
                out[((size_t)y * W + x) * 4 + k] =
                    (uint8_t)((a * (1 - fx) + b * fx) * (1 - fy) + (d * (1 - fx) + e * fx) * fy + 0.5f);
            }
        }
}

/* The steepest step between neighbours along a row: how sharp an edge is. */
static int steepest( const uint8_t *image, uint32_t W, uint32_t y, uint32_t from, uint32_t to )
{
    int step = 0;
    uint32_t x;

    for (x = from + 1; x < to; x++)
    {
        int d = abs( image[((size_t)y * W + x) * 4 + 1] - image[((size_t)y * W + x - 1) * 4 + 1] );
        if (d > step) step = d;
    }
    return step;
}

int main( int argc, char **argv )
{
    const char *dir = argc > 1 ? argv[1] : NULL;
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo instance_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkDeviceQueueCreateInfo queue_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo device_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkPhysicalDeviceFeatures features = {0};
    VkPhysicalDeviceProperties props;
    VkInstance instance;
    uint32_t count = 1, w = 320, h = 240, W = 1280, H = 720, x, y;
    float priority = 1.0f;
    uint8_t *src, *out, *ref;
    struct constants c;
    int fsr_edge, bilinear_edge, sharp_edge;

    setvbuf( stdout, NULL, _IONBF, 0 );
    app.apiVersion = VK_API_VERSION_1_0;
    instance_info.pApplicationInfo = &app;
    CHECK( vkCreateInstance( &instance_info, NULL, &instance ) );
    CHECK( vkEnumeratePhysicalDevices( instance, &count, &phys ) );
    vkGetPhysicalDeviceProperties( phys, &props );
    printf( "device: %s\n", props.deviceName );
    family = 0;
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.pEnabledFeatures = &features;
    CHECK( vkCreateDevice( phys, &device_info, NULL, &dev ) );
    vkGetDeviceQueue( dev, family, 0, &queue );

    src = malloc( (size_t)w * h * 4 );
    out = malloc( (size_t)W * H * 4 );
    ref = malloc( (size_t)W * H * 4 );

    /* One colour everywhere: the picture keeps it, to the last bit, and the
     * bars beside a 4:3 picture are black. */
    for (x = 0; x < w * h; x++) memcpy( src + x * 4, (uint8_t[]){ 0x30, 0x80, 0xc0, 0xff }, 4 );
    upscale( src, w, h, out, W, H, 0.4f, &c );
    assert( c.offset[0] == 160 && c.offset[1] == 0 && c.extents[0] == 960 && c.extents[1] == 720 );
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
        {
            uint8_t *p = pixel( out, W, x, y );
            if (x < 160 || x >= 1120) assert( !p[0] && !p[1] && !p[2] );
            else assert( abs( p[0] - 0x30 ) <= 1 && abs( p[1] - 0x80 ) <= 1 && abs( p[2] - 0xc0 ) <= 1 );
        }
    printf( "flat colour: kept inside, black bars outside\n" );

    /* A hard vertical edge and diagonal ones. Across the vertical edge the row
     * only ever climbs (EASU does not ring), and it is steeper than bilinear's. */
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
        {
            int on = x >= w / 2 || (x + y) % 64 < 32 * (y < h / 2);
            uint8_t v = on ? 0xe0 : 0x20;
            memcpy( src + ((size_t)y * w + x) * 4, (uint8_t[]){ v, v, v, 0xff }, 4 );
        }
    save( dir, "edges-source.png", src, w, h );
    upscale( src, w, h, out, W, H, 0.0f, &c );
    save( dir, "edges-fsr-0.png", out, W, H );
    for (x = 161; x < 1119; x++)
        assert( pixel( out, W, x, 600 )[1] + 1 >= pixel( out, W, x - 1, 600 )[1] );
    for (x = 160; x < 1120; x++)
        assert( pixel( out, W, x, 600 )[1] >= 0x20 - 1 && pixel( out, W, x, 600 )[1] <= 0xe0 + 1 );
    fsr_edge = steepest( out, W, 600, 560, 720 );
    upscale( src, w, h, out, W, H, 1.0f, &c );
    save( dir, "edges-fsr-100.png", out, W, H );
    sharp_edge = steepest( out, W, 600, 560, 720 );
    bilinear( src, w, h, ref, W, H, &c );
    save( dir, "edges-bilinear.png", ref, W, H );
    bilinear_edge = steepest( ref, W, 600, 560, 720 );
    printf( "steepest step across the edge at x3: bilinear %d, EASU %d, EASU+RCAS 100%% %d\n",
            bilinear_edge, fsr_edge, sharp_edge );
    assert( fsr_edge > bilinear_edge && sharp_edge >= fsr_edge );

    /* A picture wider than the screen's shape: bars above and below. */
    free( src );
    w = 400; h = 100;
    src = malloc( (size_t)w * h * 4 );
    for (x = 0; x < w * h; x++) memcpy( src + x * 4, (uint8_t[]){ 0x80, 0x80, 0x80, 0xff }, 4 );
    upscale( src, w, h, out, W, H, 0.4f, &c );
    assert( c.offset[0] == 0 && c.extents[0] == 1280 && c.extents[1] == 320 && c.offset[1] == 200 );
    assert( !pixel( out, W, 640, 199 )[1] && abs( pixel( out, W, 640, 200 )[1] - 0x80 ) <= 1 &&
            abs( pixel( out, W, 640, 519 )[1] - 0x80 ) <= 1 && !pixel( out, W, 640, 520 )[1] );
    printf( "letterbox: bars above and below, edges of the picture not darkened\n" );

    free( src ); free( out ); free( ref );
    vkDestroyDevice( dev, NULL );
    vkDestroyInstance( instance, NULL );
    printf( "fshack FSR: EASU and RCAS as shipped run and upscale correctly\n" );
    return 0;
}
