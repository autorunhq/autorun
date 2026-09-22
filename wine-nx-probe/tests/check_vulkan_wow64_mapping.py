#!/usr/bin/env python3
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/win32u/vulkan.c').read_text()


def function(marker):
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <windef.h>
#include <winbase.h>
#include <wine/vulkan.h>
#define TRACE(...) ((void)0)
#define FIXME(...) ((void)0)
#define ERR(...) ((void)0)
#undef GetCurrentProcess
#define GetCurrentProcess() ((HANDLE)-1)
typedef LONG NTSTATUS;
static struct { int WowTebOffset; } teb;
#define NtCurrentTeb() (&teb)
static ULONG_PTR zero_bits;
static uintptr_t host_limit;
static unsigned allocations, frees, maps, unmaps;
static NTSTATUS allocation_result;
static VkResult properties_result, map_result;
static uint32_t host_type_bits = 1;
static void *allocation_pointer = (void *)0x30000000, *driver_pointer;
static LONG nx_host_import_logs;
struct vulkan_physical_device {
    uint32_t external_memory_align, map_placed_align;
    VkPhysicalDeviceMemoryProperties memory_properties;
} physical;
struct vulkan_device {
    struct vulkan_physical_device *physical_device;
    struct { VkDevice device; } host;
    PFN_vkGetMemoryHostPointerPropertiesEXT p_vkGetMemoryHostPointerPropertiesEXT;
    PFN_vkMapMemory p_vkMapMemory;
    PFN_vkMapMemory2KHR p_vkMapMemory2KHR;
    PFN_vkUnmapMemory p_vkUnmapMemory;
} device;
struct device_memory {
    struct { struct { VkDeviceMemory device_memory; } host; } obj;
    VkDeviceSize size;
    void *vm_map;
} memory;
static struct vulkan_device *vulkan_device_from_handle(VkDevice d)
{ assert(d == (VkDevice)1); return &device; }
static struct device_memory *device_memory_from_handle(VkDeviceMemory m)
{ assert(m == (VkDeviceMemory)2); return &memory; }
static void horizon_get_address_space_limits(void **start, void **limit)
{ *start = (void *)0x200000; *limit = (void *)host_limit; }
static void nx_vk_trace(const char *format, ...) { (void)format; }
static BOOL nx_memory_log_failure(void) { return TRUE; }
static NTSTATUS NtAllocateVirtualMemory(HANDLE process, void **p, ULONG_PTR bits,
                                        SIZE_T *size, ULONG type, ULONG protect)
{
    assert(process == GetCurrentProcess() && !*p && bits == zero_bits);
    assert(*size && type == MEM_COMMIT && protect == PAGE_READWRITE);
    allocations++;
    if (!allocation_result) *p = allocation_pointer;
    return allocation_result;
}
static NTSTATUS NtFreeVirtualMemory(HANDLE process, void **p, SIZE_T *size, ULONG type)
{
    assert(process == GetCurrentProcess() && *p == allocation_pointer);
    assert(!*size && type == MEM_RELEASE);
    frees++;
    *p = NULL;
    return 0;
}
static VkResult host_properties(VkDevice d, VkExternalMemoryHandleTypeFlagBits type,
                                const void *p, VkMemoryHostPointerPropertiesEXT *props)
{
    assert(d == (VkDevice)3 && type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT);
    assert(p == allocation_pointer);
    props->memoryTypeBits = host_type_bits;
    return properties_result;
}
static VkResult host_map(VkDevice d, VkDeviceMemory m, VkDeviceSize offset,
                         VkDeviceSize size, VkMemoryMapFlags flags, void **p)
{
    assert(d == (VkDevice)3 && m == (VkDeviceMemory)4);
    (void)offset; (void)size; (void)flags;
    maps++;
    if (!map_result) *p = driver_pointer;
    return map_result;
}
static VkResult host_map2(VkDevice d, const VkMemoryMapInfoKHR *info, void **p)
{
    VkResult res = host_map(d, info->memory, info->offset, info->size, info->flags, p);
    if (!res && info->pNext)
    {
        const VkMemoryMapPlacedInfoEXT *placed = info->pNext;
        assert(placed->sType == VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT);
        assert(info->flags & VK_MEMORY_MAP_PLACED_BIT_EXT);
        *p = placed->pPlacedAddress;
    }
    return res;
}
static void host_unmap(VkDevice d, VkDeviceMemory m)
{ assert(d == (VkDevice)3 && m == (VkDeviceMemory)4); unmaps++; }
'''

for marker in ('static BOOL nx_uses_32bit_address_space(',
               'static BOOL nx_driver_maps_preferred(',
               'static VkResult import_external_host_memory(',
               'static VkResult allocate_external_host_memory(',
               'static VkResult win32u_vkMapMemory2KHR(',
               'static VkResult win32u_vkMapMemory('):
    fixture += function(marker)

fixture += r'''
int main(int argc, char **argv)
{
    assert(argc == 2);
    host_limit = strtoull(argv[1], NULL, 0);
    physical.external_memory_align = 4096;
    physical.memory_properties.memoryTypeCount = 1;
    physical.memory_properties.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    device.physical_device = &physical;
    device.host.device = (VkDevice)3;
    device.p_vkGetMemoryHostPointerPropertiesEXT = host_properties;
    device.p_vkMapMemory = host_map;
    device.p_vkUnmapMemory = host_unmap;
    memory.obj.host.device_memory = (VkDeviceMemory)4;
    memory.size = 65536;

    for (unsigned wow64 = 0; wow64 < 2; wow64++)
    {
        teb.WowTebOffset = wow64 ? 0x2000 : 0;
        zero_bits = wow64 ? UINT32_MAX : 0;
        assert(nx_driver_maps_preferred() == (!wow64 || host_limit <= 0x100000000ULL));
        for (unsigned modern = 0; modern < 2; modern++)
        {
            void *p = NULL;
            device.p_vkMapMemory2KHR = modern ? host_map2 : NULL;
            driver_pointer = (void *)0x1a2210000ULL;
            unmaps = 0;
            VkResult res = win32u_vkMapMemory((VkDevice)1, (VkDeviceMemory)2, 0, VK_WHOLE_SIZE, 0, &p);
            assert(res == (wow64 ? VK_ERROR_MEMORY_MAP_FAILED : VK_SUCCESS));
            assert(p == (wow64 ? NULL : driver_pointer) && unmaps == wow64);
            driver_pointer = (void *)0x20000000;
            assert(win32u_vkMapMemory((VkDevice)1, (VkDeviceMemory)2, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS);
            assert(p == driver_pointer);
            driver_pointer = (void *)0xfffff000;
            assert(win32u_vkMapMemory((VkDevice)1, (VkDeviceMemory)2, 0, 4096, 0, &p) == VK_SUCCESS);
            res = win32u_vkMapMemory((VkDevice)1, (VkDeviceMemory)2, 0, 4097, 0, &p);
            assert(res == (wow64 ? VK_ERROR_MEMORY_MAP_FAILED : VK_SUCCESS));
            map_result = VK_ERROR_DEVICE_LOST;
            unmaps = 0;
            assert(win32u_vkMapMemory((VkDevice)1, (VkDeviceMemory)2, 0, 4096, 0, &p) == map_result);
            assert(!unmaps);
            map_result = VK_SUCCESS;
        }
    }

    teb.WowTebOffset = 0x2000;
    zero_bits = UINT32_MAX;
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 8192};
    VkImportMemoryHostPointerInfoEXT imported = {0};
    assert(allocate_external_host_memory(&device, &alloc, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &imported) == VK_SUCCESS);
    assert(imported.pHostPointer == allocation_pointer && alloc.pNext == &imported && !frees);
    memory.vm_map = imported.pHostPointer;
    void *p = NULL;
    maps = unmaps = 0;
    assert(win32u_vkMapMemory((VkDevice)1, (VkDeviceMemory)2, 4096, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS);
    assert(p == (char *)allocation_pointer + 4096 && !maps && !unmaps);
    memory.vm_map = (void *)0x1a2210000ULL;
    assert(win32u_vkMapMemory((VkDevice)1, (VkDeviceMemory)2, 0, VK_WHOLE_SIZE, 0, &p) == VK_ERROR_MEMORY_MAP_FAILED);
    assert(!p && !unmaps && memory.vm_map);
    memory.vm_map = NULL;

    alloc.pNext = NULL;
    imported = (VkImportMemoryHostPointerInfoEXT){0};
    properties_result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    assert(allocate_external_host_memory(&device, &alloc, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &imported) == properties_result);
    assert(frees == 1 && !imported.pHostPointer && !alloc.pNext);
    properties_result = VK_SUCCESS;
    host_type_bits = 0;
    assert(allocate_external_host_memory(&device, &alloc, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &imported) == VK_SUCCESS);
    assert(frees == 2 && !imported.pHostPointer && !alloc.pNext);
    allocation_result = 1;
    assert(allocate_external_host_memory(&device, &alloc, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &imported) == VK_ERROR_OUT_OF_HOST_MEMORY);
    assert(frees == 2 && !imported.pHostPointer);
    allocation_result = 0;

    physical.map_placed_align = 4096;
    device.p_vkMapMemory2KHR = host_map2;
    allocation_pointer = (void *)0xffff0000;
    assert(win32u_vkMapMemory((VkDevice)1, (VkDeviceMemory)2, 4096, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS);
    assert(p == (void *)0xffff1000 && memory.vm_map == allocation_pointer);
    memory.vm_map = NULL;
    map_result = VK_ERROR_MEMORY_MAP_FAILED;
    assert(win32u_vkMapMemory((VkDevice)1, (VkDeviceMemory)2, 0, VK_WHOLE_SIZE, 0, &p) == map_result);
    assert(frees == 3 && !memory.vm_map);
    puts("Vulkan: guest-width policy, low imports, mapping APIs, range boundaries and failure cleanup passed");
}
'''

allocation = function('static VkResult win32u_vkAllocateMemory(')
assert '!nx_driver_maps_preferred() &&' in allocation
assert 'allocate_external_host_memory( device, alloc_info, mem_flags, &host_pointer_info )' in allocation
with tempfile.TemporaryDirectory(prefix='vulkan-wow64-mapping-') as directory:
    directory = Path(directory)
    (directory / 'test.c').write_text(fixture)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-D__SWITCH__', '-D_WIN64', '-DWINE_UNIX_LIB',
                    '-D__WINESRC__', '-I', str(root / 'include'), str(directory / 'test.c'),
                    '-o', str(directory / 'test')], check=True)
    for limit in ('0x100000000', '0x1000000000', '0x8000000000'):
        subprocess.run([str(directory / 'test'), limit], check=True)
