/*
 * D3DKMT resource management for Horizon
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

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "ntgdi_private.h"
#include "win32u_private.h"
#include "ntuser_private.h"

#define D3DKMT_HANDLE_BIT 0x40000000
#define D3DKMT_INITIAL_OBJECTS 1024
#define D3DKMT_SHARED_PAGE_SIZE 0x1000
#define D3DKMT_SHARED_RUNTIME_SIZE 0x400
#define D3DKMT_SHARED_MAGIC UINT64_C(0x584e544d4b443344)

struct d3dkmt_shared_header
{
    uint64_t magic;
    uint64_t data_size;
    uint32_t nvmap_id;
    uint32_t runtime_size;
    unsigned char runtime[D3DKMT_SHARED_RUNTIME_SIZE];
};

struct d3dkmt_shared_resource
{
    struct d3dkmt_shared_resource *next;
    HANDLE section;
    void *mapping;
    SIZE_T view_size;
    SIZE_T data_size;
    void *reference;
    D3DKMT_HANDLE global;
    unsigned int refs;
};

struct d3dkmt_object
{
    enum d3dkmt_type type;
    struct d3dkmt_shared_resource *resource;
    D3DKMT_HANDLE allocation;
};

static pthread_mutex_t d3dkmt_lock = PTHREAD_MUTEX_INITIALIZER;
static struct d3dkmt_object **d3dkmt_objects;
static size_t d3dkmt_object_count;
static size_t d3dkmt_object_next = 1;
static struct d3dkmt_shared_resource *d3dkmt_resources;
static unsigned int d3dkmt_global_next = 1;

static D3DKMT_HANDLE object_handle( size_t index )
{
    return (index << 6) | D3DKMT_HANDLE_BIT;
}

static size_t object_index( D3DKMT_HANDLE handle )
{
    return (handle & ~0xc0000000u) >> 6;
}

static BOOL is_global_handle( D3DKMT_HANDLE handle )
{
    return (handle & 0xc0000000u) && (handle & 0x3f) == 2;
}

static BOOL grow_object_table(void)
{
    size_t count = d3dkmt_object_count ? d3dkmt_object_count * 2 : D3DKMT_INITIAL_OBJECTS;
    struct d3dkmt_object **objects;

    if (!(objects = realloc( d3dkmt_objects, count * sizeof(*objects) ))) return FALSE;
    memset( objects + d3dkmt_object_count, 0, (count - d3dkmt_object_count) * sizeof(*objects) );
    d3dkmt_objects = objects;
    d3dkmt_object_count = count;
    return TRUE;
}

static D3DKMT_HANDLE insert_object_locked( struct d3dkmt_object *object )
{
    size_t index, start;

    if (!d3dkmt_object_count && !grow_object_table()) return 0;
    start = d3dkmt_object_next;
    for (;;)
    {
        for (index = start; index < d3dkmt_object_count; index++)
        {
            if (d3dkmt_objects[index]) continue;
            d3dkmt_objects[index] = object;
            d3dkmt_object_next = index + 1;
            return object_handle( index );
        }
        if (start != 1)
        {
            start = 1;
            continue;
        }
        if (!grow_object_table()) return 0;
        start = d3dkmt_object_next;
    }
}

static D3DKMT_HANDLE insert_object( struct d3dkmt_object *object )
{
    D3DKMT_HANDLE handle;

    pthread_mutex_lock( &d3dkmt_lock );
    handle = insert_object_locked( object );
    pthread_mutex_unlock( &d3dkmt_lock );
    return handle;
}

static struct d3dkmt_object *get_object_locked( D3DKMT_HANDLE handle, enum d3dkmt_type type )
{
    size_t index = object_index( handle );
    struct d3dkmt_object *object;

    if (!handle || index >= d3dkmt_object_count || !(object = d3dkmt_objects[index])) return NULL;
    if (object_handle( index ) != handle || object->type != type) return NULL;
    return object;
}

static struct d3dkmt_object *get_object( D3DKMT_HANDLE handle, enum d3dkmt_type type )
{
    struct d3dkmt_object *object;

    pthread_mutex_lock( &d3dkmt_lock );
    object = get_object_locked( handle, type );
    pthread_mutex_unlock( &d3dkmt_lock );
    return object;
}

static struct d3dkmt_object *remove_object_locked( D3DKMT_HANDLE handle, enum d3dkmt_type type )
{
    size_t index = object_index( handle );
    struct d3dkmt_object *object = get_object_locked( handle, type );

    if (!object) return NULL;
    d3dkmt_objects[index] = NULL;
    if (index < d3dkmt_object_next) d3dkmt_object_next = index;
    return object;
}

static BOOL destroy_plain_object( D3DKMT_HANDLE handle, enum d3dkmt_type type )
{
    struct d3dkmt_object *object;

    pthread_mutex_lock( &d3dkmt_lock );
    object = remove_object_locked( handle, type );
    pthread_mutex_unlock( &d3dkmt_lock );
    free( object );
    return object != NULL;
}

static D3DKMT_HANDLE create_plain_object( enum d3dkmt_type type )
{
    struct d3dkmt_object *object;
    D3DKMT_HANDLE handle;

    if (!(object = calloc( 1, sizeof(*object) ))) return 0;
    object->type = type;
    if (!(handle = insert_object( object ))) free( object );
    return handle;
}

static struct d3dkmt_shared_header *resource_header( struct d3dkmt_shared_resource *resource )
{
    return (struct d3dkmt_shared_header *)((char *)resource->mapping + resource->view_size - D3DKMT_SHARED_PAGE_SIZE);
}

static D3DKMT_HANDLE allocate_global_locked(void)
{
    unsigned int first = d3dkmt_global_next;

    do
    {
        D3DKMT_HANDLE handle = (d3dkmt_global_next++ << 6) | D3DKMT_HANDLE_BIT | 2;
        struct d3dkmt_shared_resource *resource;

        if (!d3dkmt_global_next) d3dkmt_global_next = 1;
        for (resource = d3dkmt_resources; resource; resource = resource->next)
            if (resource->global == handle) break;
        if (!resource) return handle;
    } while (d3dkmt_global_next != first);
    return 0;
}

static struct d3dkmt_shared_resource *find_global_locked( D3DKMT_HANDLE global )
{
    struct d3dkmt_shared_resource *resource;

    for (resource = d3dkmt_resources; resource; resource = resource->next)
        if (resource->global == global) return resource;
    return NULL;
}

static struct d3dkmt_shared_resource *find_section_locked( HANDLE section )
{
    struct d3dkmt_shared_resource *resource;

    for (resource = d3dkmt_resources; resource; resource = resource->next)
        if (!NtCompareObjects( resource->section, section )) return resource;
    return NULL;
}

static void release_memory_reference( void *reference )
{
#ifdef WINE_NX_MESA_SWITCH
    if (reference) wine_nx_vk_release_memory_reference( reference );
#else
    (void)reference;
#endif
}

static void free_shared_resource( struct d3dkmt_shared_resource *resource )
{
    release_memory_reference( resource->reference );
    NtUnmapViewOfSection( NtCurrentProcess(), resource->mapping );
    NtClose( resource->section );
    free( resource );
}

static BOOL shared_resource_has_external_handle( struct d3dkmt_shared_resource *resource )
{
    OBJECT_BASIC_INFORMATION info;

    return NtQueryObject( resource->section, ObjectBasicInformation, &info, sizeof(info), NULL ) ||
           info.HandleCount > 1;
}

static void reap_shared_resources(void)
{
    struct d3dkmt_shared_resource **entry, *resource;

    for (;;)
    {
        resource = NULL;
        pthread_mutex_lock( &d3dkmt_lock );
        for (entry = &d3dkmt_resources; *entry; entry = &(*entry)->next)
        {
            if ((*entry)->refs || shared_resource_has_external_handle( *entry )) continue;
            resource = *entry;
            *entry = resource->next;
            break;
        }
        pthread_mutex_unlock( &d3dkmt_lock );
        if (!resource) return;
        free_shared_resource( resource );
    }
}

static void release_shared_resource( struct d3dkmt_shared_resource *resource )
{
    struct d3dkmt_shared_resource **entry;
    BOOL destroy = FALSE;

    pthread_mutex_lock( &d3dkmt_lock );
    if (!--resource->refs)
    {
        if (!shared_resource_has_external_handle( resource ))
            for (entry = &d3dkmt_resources; *entry; entry = &(*entry)->next)
                if (*entry == resource)
                {
                    *entry = resource->next;
                    destroy = TRUE;
                    break;
                }
    }
    pthread_mutex_unlock( &d3dkmt_lock );
    if (destroy) free_shared_resource( resource );
}

static struct d3dkmt_shared_resource *map_shared_section( HANDLE section )
{
    struct d3dkmt_shared_resource *resource, *existing;
    struct d3dkmt_shared_header *header;
    SECTION_BASIC_INFORMATION info;
    SIZE_T view_size;
    HANDLE duplicate;
    void *mapping = NULL;

    if (NtQuerySection( section, SectionBasicInformation, &info, sizeof(info), NULL )) return NULL;
    if (info.Size.QuadPart < D3DKMT_SHARED_PAGE_SIZE || (uint64_t)info.Size.QuadPart > SIZE_MAX) return NULL;
    view_size = info.Size.QuadPart;
    if (NtMapViewOfSection( section, NtCurrentProcess(), &mapping, 0, 0, NULL, &view_size,
                            ViewUnmap, 0, PAGE_READWRITE )) return NULL;
    header = (struct d3dkmt_shared_header *)((char *)mapping + view_size - D3DKMT_SHARED_PAGE_SIZE);
    if (header->magic != D3DKMT_SHARED_MAGIC || !header->data_size || !header->nvmap_id ||
        header->runtime_size > sizeof(header->runtime))
    {
        NtUnmapViewOfSection( NtCurrentProcess(), mapping );
        return NULL;
    }
    if (NtDuplicateObject( NtCurrentProcess(), section, NtCurrentProcess(), &duplicate, 0, 0,
                           DUPLICATE_SAME_ATTRIBUTES | DUPLICATE_SAME_ACCESS ))
    {
        NtUnmapViewOfSection( NtCurrentProcess(), mapping );
        return NULL;
    }
    if (!(resource = calloc( 1, sizeof(*resource) )))
    {
        NtClose( duplicate );
        NtUnmapViewOfSection( NtCurrentProcess(), mapping );
        return NULL;
    }
    resource->section = duplicate;
    resource->mapping = mapping;
    resource->view_size = view_size;
    resource->data_size = header->data_size;

    pthread_mutex_lock( &d3dkmt_lock );
    if ((existing = find_section_locked( section )))
    {
        existing->refs++;
        pthread_mutex_unlock( &d3dkmt_lock );
        free_shared_resource( resource );
        return existing;
    }
    resource->global = allocate_global_locked();
    if (!resource->global)
    {
        pthread_mutex_unlock( &d3dkmt_lock );
        free_shared_resource( resource );
        return NULL;
    }
    resource->refs = 1;
    resource->next = d3dkmt_resources;
    d3dkmt_resources = resource;
    pthread_mutex_unlock( &d3dkmt_lock );
    return resource;
}

static struct d3dkmt_shared_resource *get_shared_resource( D3DKMT_HANDLE global, HANDLE section )
{
    struct d3dkmt_shared_resource *resource;

    reap_shared_resources();
    pthread_mutex_lock( &d3dkmt_lock );
    resource = global ? find_global_locked( global ) : find_section_locked( section );
    if (resource) resource->refs++;
    pthread_mutex_unlock( &d3dkmt_lock );
    if (!resource && section) resource = map_shared_section( section );
    return resource;
}

static D3DKMT_HANDLE open_local_resource( struct d3dkmt_shared_resource *shared )
{
    struct d3dkmt_object *resource, *allocation = NULL;
    D3DKMT_HANDLE resource_handle = 0, allocation_handle = 0;

    if (!(resource = calloc( 1, sizeof(*resource) )) ||
        !(allocation = calloc( 1, sizeof(*allocation) ))) goto failed;
    resource->type = D3DKMT_RESOURCE;
    resource->resource = shared;
    allocation->type = D3DKMT_ALLOCATION;
    pthread_mutex_lock( &d3dkmt_lock );
    if (!(allocation_handle = insert_object_locked( allocation ))) goto failed_locked;
    resource->allocation = allocation_handle;
    if (!(resource_handle = insert_object_locked( resource ))) goto failed_locked;
    shared->refs++;
    pthread_mutex_unlock( &d3dkmt_lock );
    return resource_handle;

failed_locked:
    if (allocation_handle) remove_object_locked( allocation_handle, D3DKMT_ALLOCATION );
    pthread_mutex_unlock( &d3dkmt_lock );
failed:
    free( allocation );
    free( resource );
    return 0;
}

static NTSTATUS copy_runtime_data( struct d3dkmt_shared_resource *resource, void *data, UINT *size )
{
    struct d3dkmt_shared_header *header = resource_header( resource );
    UINT capacity = *size;

    pthread_mutex_lock( &d3dkmt_lock );
    *size = header->runtime_size;
    if (data && capacity) memcpy( data, header->runtime, min( capacity, header->runtime_size ) );
    pthread_mutex_unlock( &d3dkmt_lock );
    return capacity && capacity < *size ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
}

static NTSTATUS destroy_local_resource( D3DKMT_HANDLE handle )
{
    struct d3dkmt_shared_resource *shared;
    struct d3dkmt_object *resource, *allocation = NULL;

    pthread_mutex_lock( &d3dkmt_lock );
    if (!(resource = remove_object_locked( handle, D3DKMT_RESOURCE )))
    {
        pthread_mutex_unlock( &d3dkmt_lock );
        return STATUS_INVALID_PARAMETER;
    }
    if (resource->allocation)
        allocation = remove_object_locked( resource->allocation, D3DKMT_ALLOCATION );
    shared = resource->resource;
    pthread_mutex_unlock( &d3dkmt_lock );
    free( allocation );
    free( resource );
    release_shared_resource( shared );
    return STATUS_SUCCESS;
}

static NTSTATUS fill_open_resource( struct d3dkmt_shared_resource *shared, D3DKMT_HANDLE *resource,
                                    D3DKMT_HANDLE *allocation, void *runtime, UINT *runtime_size )
{
    struct d3dkmt_object *object;
    NTSTATUS status;

    if (!(*resource = open_local_resource( shared ))) return STATUS_NO_MEMORY;
    object = get_object( *resource, D3DKMT_RESOURCE );
    *allocation = object->allocation;
    status = copy_runtime_data( shared, runtime, runtime_size );
    if (status)
    {
        destroy_local_resource( *resource );
        *resource = 0;
    }
    return status;
}

NTSTATUS WINAPI NtGdiDdDDIAcquireKeyedMutex( D3DKMT_ACQUIREKEYEDMUTEX *params ) { return STATUS_PROCEDURE_NOT_FOUND; }
NTSTATUS WINAPI NtGdiDdDDIAcquireKeyedMutex2( D3DKMT_ACQUIREKEYEDMUTEX2 *params ) { return STATUS_PROCEDURE_NOT_FOUND; }
NTSTATUS WINAPI NtGdiDdDDICheckOcclusion( const D3DKMT_CHECKOCCLUSION *desc ) { return STATUS_PROCEDURE_NOT_FOUND; }
NTSTATUS WINAPI NtGdiDdDDICheckVidPnExclusiveOwnership( const D3DKMT_CHECKVIDPNEXCLUSIVEOWNERSHIP *desc ) { return STATUS_PROCEDURE_NOT_FOUND; }

NTSTATUS WINAPI NtGdiDdDDICloseAdapter( const D3DKMT_CLOSEADAPTER *desc )
{
    if (!desc || !destroy_plain_object( desc->hAdapter, D3DKMT_ADAPTER )) return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtGdiDdDDICreateAllocation( D3DKMT_CREATEALLOCATION *params ) { return STATUS_PROCEDURE_NOT_FOUND; }
NTSTATUS WINAPI NtGdiDdDDICreateAllocation2( D3DKMT_CREATEALLOCATION *params ) { return STATUS_PROCEDURE_NOT_FOUND; }

NTSTATUS WINAPI NtGdiDdDDICreateDevice( D3DKMT_CREATEDEVICE *desc )
{
    if (!desc || !get_object( desc->hAdapter, D3DKMT_ADAPTER )) return STATUS_INVALID_PARAMETER;
    if (!(desc->hDevice = create_plain_object( D3DKMT_DEVICE ))) return STATUS_NO_MEMORY;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtGdiDdDDICreateKeyedMutex( D3DKMT_CREATEKEYEDMUTEX *params ) { return STATUS_PROCEDURE_NOT_FOUND; }
NTSTATUS WINAPI NtGdiDdDDICreateKeyedMutex2( D3DKMT_CREATEKEYEDMUTEX2 *params ) { return STATUS_PROCEDURE_NOT_FOUND; }
NTSTATUS WINAPI NtGdiDdDDICreateSynchronizationObject( D3DKMT_CREATESYNCHRONIZATIONOBJECT *params ) { return STATUS_PROCEDURE_NOT_FOUND; }
NTSTATUS WINAPI NtGdiDdDDICreateSynchronizationObject2( D3DKMT_CREATESYNCHRONIZATIONOBJECT2 *params ) { return STATUS_PROCEDURE_NOT_FOUND; }

NTSTATUS WINAPI NtGdiDdDDIDestroyAllocation2( const D3DKMT_DESTROYALLOCATION2 *params )
{
    UINT i;

    if (!params || !get_object( params->hDevice, D3DKMT_DEVICE )) return STATUS_INVALID_PARAMETER;
    if (params->hResource) return destroy_local_resource( params->hResource );
    if (params->AllocationCount && !params->phAllocationList) return STATUS_INVALID_PARAMETER;
    for (i = 0; i < params->AllocationCount; i++)
        if (!destroy_plain_object( params->phAllocationList[i], D3DKMT_ALLOCATION )) return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtGdiDdDDIDestroyAllocation( const D3DKMT_DESTROYALLOCATION *params )
{
    D3DKMT_DESTROYALLOCATION2 params2 = {0};

    if (!params) return STATUS_INVALID_PARAMETER;
    params2.hDevice = params->hDevice;
    params2.hResource = params->hResource;
    params2.phAllocationList = params->phAllocationList;
    params2.AllocationCount = params->AllocationCount;
    return NtGdiDdDDIDestroyAllocation2( &params2 );
}

NTSTATUS WINAPI NtGdiDdDDIDestroyDevice( const D3DKMT_DESTROYDEVICE *desc )
{
    if (!desc || !destroy_plain_object( desc->hDevice, D3DKMT_DEVICE )) return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtGdiDdDDIDestroyKeyedMutex( const D3DKMT_DESTROYKEYEDMUTEX *params ) { return STATUS_PROCEDURE_NOT_FOUND; }
NTSTATUS WINAPI NtGdiDdDDIDestroySynchronizationObject( const D3DKMT_DESTROYSYNCHRONIZATIONOBJECT *params ) { return STATUS_PROCEDURE_NOT_FOUND; }

NTSTATUS WINAPI NtGdiDdDDIEscape( const D3DKMT_ESCAPE *desc )
{
    struct d3dkmt_object *object;
    struct d3dkmt_shared_header *header;

    if (!desc || desc->Type != D3DKMT_ESCAPE_UPDATE_RESOURCE_WINE ||
        desc->PrivateDriverDataSize > D3DKMT_SHARED_RUNTIME_SIZE ||
        (desc->PrivateDriverDataSize && !desc->pPrivateDriverData)) return STATUS_INVALID_PARAMETER;
    pthread_mutex_lock( &d3dkmt_lock );
    if (!(object = get_object_locked( desc->hContext, D3DKMT_RESOURCE )))
    {
        pthread_mutex_unlock( &d3dkmt_lock );
        return STATUS_INVALID_PARAMETER;
    }
    header = resource_header( object->resource );
    memcpy( header->runtime, desc->pPrivateDriverData, desc->PrivateDriverDataSize );
    header->runtime_size = desc->PrivateDriverDataSize;
    pthread_mutex_unlock( &d3dkmt_lock );
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtGdiDdDDIOpenAdapterFromHdc( D3DKMT_OPENADAPTERFROMHDC *desc ) { return STATUS_PROCEDURE_NOT_FOUND; }

NTSTATUS WINAPI NtGdiDdDDIOpenAdapterFromLuid( D3DKMT_OPENADAPTERFROMLUID *desc )
{
    if (!desc) return STATUS_INVALID_PARAMETER;
    if (!(desc->hAdapter = create_plain_object( D3DKMT_ADAPTER ))) return STATUS_NO_MEMORY;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtGdiDdDDIOpenKeyedMutex( D3DKMT_OPENKEYEDMUTEX *params ) { return STATUS_PROCEDURE_NOT_FOUND; }
NTSTATUS WINAPI NtGdiDdDDIOpenKeyedMutex2( D3DKMT_OPENKEYEDMUTEX2 *params ) { return STATUS_PROCEDURE_NOT_FOUND; }
NTSTATUS WINAPI NtGdiDdDDIOpenKeyedMutexFromNtHandle( D3DKMT_OPENKEYEDMUTEXFROMNTHANDLE *params ) { return STATUS_PROCEDURE_NOT_FOUND; }
NTSTATUS WINAPI NtGdiDdDDIOpenNtHandleFromName( D3DKMT_OPENNTHANDLEFROMNAME *params ) { return STATUS_PROCEDURE_NOT_FOUND; }

static NTSTATUS open_resource( D3DKMT_OPENRESOURCE *params, BOOL version2 )
{
    struct d3dkmt_shared_resource *shared;
    D3DKMT_HANDLE allocation;
    NTSTATUS status;

    if (!params || !get_object( params->hDevice, D3DKMT_DEVICE ) ||
        !is_global_handle( params->hGlobalShare ) || params->NumAllocations != 1) return STATUS_INVALID_PARAMETER;
    if (version2 && !params->pOpenAllocationInfo2) return STATUS_INVALID_PARAMETER;
    if (!version2 && !params->pOpenAllocationInfo) return STATUS_INVALID_PARAMETER;
    if (!(shared = get_shared_resource( params->hGlobalShare, NULL ))) return STATUS_INVALID_HANDLE;
    status = fill_open_resource( shared, &params->hResource, &allocation,
                                 params->pPrivateRuntimeData, &params->PrivateRuntimeDataSize );
    release_shared_resource( shared );
    if (status) return status;
    if (version2)
    {
        params->pOpenAllocationInfo2[0].hAllocation = allocation;
        params->pOpenAllocationInfo2[0].PrivateDriverDataSize = 0;
    }
    else
    {
        params->pOpenAllocationInfo[0].hAllocation = allocation;
        params->pOpenAllocationInfo[0].PrivateDriverDataSize = 0;
    }
    params->TotalPrivateDriverDataBufferSize = 0;
    params->ResourcePrivateDriverDataSize = 0;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtGdiDdDDIOpenResource( D3DKMT_OPENRESOURCE *params )
{
    return open_resource( params, FALSE );
}

NTSTATUS WINAPI NtGdiDdDDIOpenResource2( D3DKMT_OPENRESOURCE *params )
{
    return open_resource( params, TRUE );
}

NTSTATUS WINAPI NtGdiDdDDIOpenResourceFromNtHandle( D3DKMT_OPENRESOURCEFROMNTHANDLE *params )
{
    struct d3dkmt_shared_resource *shared;
    D3DKMT_HANDLE allocation;
    NTSTATUS status;

    if (!params || !get_object( params->hDevice, D3DKMT_DEVICE ) || !params->hNtHandle ||
        params->NumAllocations != 1 || !params->pOpenAllocationInfo2) return STATUS_INVALID_PARAMETER;
    if (!(shared = get_shared_resource( 0, params->hNtHandle ))) return STATUS_INVALID_HANDLE;
    status = fill_open_resource( shared, &params->hResource, &allocation,
                                 params->pPrivateRuntimeData, &params->PrivateRuntimeDataSize );
    release_shared_resource( shared );
    if (status) return status;
    params->pOpenAllocationInfo2[0].hAllocation = allocation;
    params->pOpenAllocationInfo2[0].PrivateDriverDataSize = 0;
    params->hKeyedMutex = 0;
    params->hSyncObject = 0;
    params->TotalPrivateDriverDataBufferSize = 0;
    params->ResourcePrivateDriverDataSize = 0;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtGdiDdDDIOpenSynchronizationObject( D3DKMT_OPENSYNCHRONIZATIONOBJECT *params ) { return STATUS_PROCEDURE_NOT_FOUND; }
NTSTATUS WINAPI NtGdiDdDDIOpenSyncObjectFromNtHandle( D3DKMT_OPENSYNCOBJECTFROMNTHANDLE *params ) { return STATUS_PROCEDURE_NOT_FOUND; }
NTSTATUS WINAPI NtGdiDdDDIOpenSyncObjectFromNtHandle2( D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 *params ) { return STATUS_PROCEDURE_NOT_FOUND; }
NTSTATUS WINAPI NtGdiDdDDIOpenSyncObjectNtHandleFromName( D3DKMT_OPENSYNCOBJECTNTHANDLEFROMNAME *params ) { return STATUS_PROCEDURE_NOT_FOUND; }
NTSTATUS WINAPI NtGdiDdDDIQueryAdapterInfo( D3DKMT_QUERYADAPTERINFO *desc ) { return STATUS_PROCEDURE_NOT_FOUND; }

NTSTATUS WINAPI NtGdiDdDDIQueryResourceInfo( D3DKMT_QUERYRESOURCEINFO *params )
{
    struct d3dkmt_shared_resource *shared;
    NTSTATUS status;

    if (!params || !get_object( params->hDevice, D3DKMT_DEVICE ) ||
        !is_global_handle( params->hGlobalShare )) return STATUS_INVALID_PARAMETER;
    if (!(shared = get_shared_resource( params->hGlobalShare, NULL ))) return STATUS_INVALID_HANDLE;
    status = copy_runtime_data( shared, params->pPrivateRuntimeData, &params->PrivateRuntimeDataSize );
    release_shared_resource( shared );
    params->TotalPrivateDriverDataSize = 0;
    params->ResourcePrivateDriverDataSize = 0;
    params->NumAllocations = 1;
    return status;
}

NTSTATUS WINAPI NtGdiDdDDIQueryResourceInfoFromNtHandle( D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE *params )
{
    struct d3dkmt_shared_resource *shared;
    NTSTATUS status;

    if (!params || !get_object( params->hDevice, D3DKMT_DEVICE ) || !params->hNtHandle)
        return STATUS_INVALID_PARAMETER;
    if (!(shared = get_shared_resource( 0, params->hNtHandle ))) return STATUS_INVALID_HANDLE;
    status = copy_runtime_data( shared, params->pPrivateRuntimeData, &params->PrivateRuntimeDataSize );
    release_shared_resource( shared );
    params->TotalPrivateDriverDataSize = 0;
    params->ResourcePrivateDriverDataSize = 0;
    params->NumAllocations = 1;
    return status;
}

NTSTATUS WINAPI NtGdiDdDDIQueryStatistics( D3DKMT_QUERYSTATISTICS *stats ) { return STATUS_PROCEDURE_NOT_FOUND; }
NTSTATUS WINAPI NtGdiDdDDIQueryVideoMemoryInfo( D3DKMT_QUERYVIDEOMEMORYINFO *desc ) { return STATUS_PROCEDURE_NOT_FOUND; }
NTSTATUS WINAPI NtGdiDdDDIReleaseKeyedMutex( D3DKMT_RELEASEKEYEDMUTEX *params ) { return STATUS_PROCEDURE_NOT_FOUND; }
NTSTATUS WINAPI NtGdiDdDDIReleaseKeyedMutex2( D3DKMT_RELEASEKEYEDMUTEX2 *params ) { return STATUS_PROCEDURE_NOT_FOUND; }
NTSTATUS WINAPI NtGdiDdDDISetQueuedLimit( D3DKMT_SETQUEUEDLIMIT *desc ) { return STATUS_PROCEDURE_NOT_FOUND; }
NTSTATUS WINAPI NtGdiDdDDISetVidPnSourceOwner( const D3DKMT_SETVIDPNSOURCEOWNER *desc ) { return STATUS_PROCEDURE_NOT_FOUND; }

NTSTATUS WINAPI NtGdiDdDDIShareObjects( UINT count, const D3DKMT_HANDLE *handles, OBJECT_ATTRIBUTES *attr,
                                        UINT access, HANDLE *handle )
{
    struct d3dkmt_object *object;
    NTSTATUS status;

    if (count != 1 || !handles || !handle || (attr && attr->ObjectName)) return STATUS_INVALID_PARAMETER;
    pthread_mutex_lock( &d3dkmt_lock );
    if (!(object = get_object_locked( handles[0], D3DKMT_RESOURCE ))) status = STATUS_INVALID_PARAMETER;
    else status = NtDuplicateObject( NtCurrentProcess(), object->resource->section, NtCurrentProcess(),
                                     handle, access, attr ? attr->Attributes : 0,
                                     access ? 0 : DUPLICATE_SAME_ACCESS );
    pthread_mutex_unlock( &d3dkmt_lock );
    return status;
}

NTSTATUS WINAPI NtGdiDdDDISignalSynchronizationObjectFromCpu( const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU *params ) { return STATUS_PROCEDURE_NOT_FOUND; }
NTSTATUS WINAPI NtGdiDdDDIWaitForSynchronizationObjectFromCpu( const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *params ) { return STATUS_PROCEDURE_NOT_FOUND; }

int d3dkmt_object_get_fd( D3DKMT_HANDLE local ) { return -1; }
NTSTATUS d3dkmt_destroy_mutex( D3DKMT_HANDLE local ) { return STATUS_PROCEDURE_NOT_FOUND; }
D3DKMT_HANDLE d3dkmt_create_resource( int fd, D3DKMT_HANDLE *global ) { if (global) *global = 0; return 0; }

D3DKMT_HANDLE d3dkmt_create_nvmap_resource( SIZE_T size, uint32_t nvmap_id,
                                            void *reference, D3DKMT_HANDLE *global )
{
    struct d3dkmt_shared_resource *resource;
    struct d3dkmt_shared_header *header;
    LARGE_INTEGER section_size;
    SIZE_T view_size;
    D3DKMT_HANDLE local;
    void *mapping = NULL;
    HANDLE section;

    reap_shared_resources();
    if (!size || !nvmap_id || !reference)
    {
        release_memory_reference( reference );
        return 0;
    }
    section_size.QuadPart = D3DKMT_SHARED_PAGE_SIZE;
    if (NtCreateSection( &section, SECTION_ALL_ACCESS, NULL, &section_size,
                         PAGE_READWRITE, SEC_COMMIT, NULL ))
    {
        release_memory_reference( reference );
        return 0;
    }
    view_size = section_size.QuadPart;
    if (NtMapViewOfSection( section, NtCurrentProcess(), &mapping, 0, 0, NULL,
                            &view_size, ViewUnmap, 0, PAGE_READWRITE ))
    {
        NtClose( section );
        release_memory_reference( reference );
        return 0;
    }
    if (!(resource = calloc( 1, sizeof(*resource) )))
    {
        NtUnmapViewOfSection( NtCurrentProcess(), mapping );
        NtClose( section );
        release_memory_reference( reference );
        return 0;
    }
    resource->section = section;
    resource->mapping = mapping;
    resource->view_size = view_size;
    resource->data_size = size;
    resource->reference = reference;
    resource->refs = 1;
    header = resource_header( resource );
    header->magic = D3DKMT_SHARED_MAGIC;
    header->data_size = size;
    header->nvmap_id = nvmap_id;

    pthread_mutex_lock( &d3dkmt_lock );
    resource->global = allocate_global_locked();
    if (!resource->global)
    {
        pthread_mutex_unlock( &d3dkmt_lock );
        free_shared_resource( resource );
        return 0;
    }
    resource->next = d3dkmt_resources;
    d3dkmt_resources = resource;
    pthread_mutex_unlock( &d3dkmt_lock );
    local = open_local_resource( resource );
    if (global) *global = local ? resource->global : 0;
    release_shared_resource( resource );
    return local;
}

D3DKMT_HANDLE d3dkmt_open_resource( D3DKMT_HANDLE global, HANDLE shared,
                                    D3DKMT_HANDLE *mutex_local, D3DKMT_HANDLE *sync_local )
{
    struct d3dkmt_shared_resource *resource;
    D3DKMT_HANDLE local;

    if (mutex_local) *mutex_local = 0;
    if (sync_local) *sync_local = 0;
    if (!(resource = get_shared_resource( global, shared ))) return 0;
    local = open_local_resource( resource );
    release_shared_resource( resource );
    return local;
}

BOOL d3dkmt_resource_get_nvmap( D3DKMT_HANDLE local, uint32_t *nvmap_id, SIZE_T *size )
{
    struct d3dkmt_object *object;
    struct d3dkmt_shared_header *header;
    BOOL ret = FALSE;

    if (!nvmap_id || !size) return FALSE;
    pthread_mutex_lock( &d3dkmt_lock );
    if ((object = get_object_locked( local, D3DKMT_RESOURCE )))
    {
        header = resource_header( object->resource );
        if ((*nvmap_id = header->nvmap_id))
        {
            *size = object->resource->data_size;
            ret = TRUE;
        }
    }
    pthread_mutex_unlock( &d3dkmt_lock );
    return ret;
}

NTSTATUS d3dkmt_destroy_resource( D3DKMT_HANDLE local )
{
    if (!local) return STATUS_SUCCESS;
    return destroy_local_resource( local );
}

D3DKMT_HANDLE d3dkmt_create_sync( int fd, D3DKMT_HANDLE *global ) { if (global) *global = 0; return 0; }
D3DKMT_HANDLE d3dkmt_open_sync( D3DKMT_HANDLE global, HANDLE shared ) { return 0; }
NTSTATUS d3dkmt_destroy_sync( D3DKMT_HANDLE local ) { return STATUS_PROCEDURE_NOT_FOUND; }

struct vk_physdev_info
{
    VkPhysicalDeviceProperties2 properties2;
    VkPhysicalDeviceIDProperties id;
    VkPhysicalDeviceMemoryProperties memory;
};

static int compare_vulkan_physical_devices( const void *left, const void *right )
{
    static const int ranks[] = {100, 1, 0, 2, 3, 200};
    const struct vk_physdev_info *a = left, *b = right;
    int rank_a = ranks[min( a->properties2.properties.deviceType, ARRAY_SIZE(ranks) - 1 )];
    int rank_b = ranks[min( b->properties2.properties.deviceType, ARRAY_SIZE(ranks) - 1 )];

    if (rank_a != rank_b) return rank_a - rank_b;
    return memcmp( a->id.deviceUUID, b->id.deviceUUID, sizeof(a->id.deviceUUID) );
}

static struct vulkan_instance *d3dkmt_vulkan_instance;

static void init_d3dkmt_vulkan(void)
{
    static const struct vulkan_instance_extensions extensions =
    {
        .has_VK_KHR_get_physical_device_properties2 = 1,
        .has_VK_KHR_external_memory_capabilities = 1,
    };

    d3dkmt_vulkan_instance = vulkan_instance_create( &extensions );
}

BOOL get_vulkan_gpus( struct list *gpus )
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    struct vk_physdev_info *devices;
    struct vulkan_instance *instance;
    UINT i, j;

    pthread_once( &once, init_d3dkmt_vulkan );
    if (!(instance = d3dkmt_vulkan_instance)) return FALSE;
    if (!(devices = calloc( instance->physical_device_count, sizeof(*devices) ))) return FALSE;

    for (i = 0; i < instance->physical_device_count; i++)
    {
        struct vulkan_physical_device *physical = instance->physical_devices + i;

        devices[i].id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        devices[i].properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        devices[i].properties2.pNext = &devices[i].id;
        instance->p_vkGetPhysicalDeviceProperties2KHR( physical->host.physical_device,
                                                        &devices[i].properties2 );
        instance->p_vkGetPhysicalDeviceMemoryProperties( physical->host.physical_device,
                                                          &devices[i].memory );
    }
    qsort( devices, instance->physical_device_count, sizeof(*devices),
           compare_vulkan_physical_devices );

    for (i = 0; i < instance->physical_device_count; i++)
    {
        struct gpu_info *gpu;

        if (devices[i].properties2.properties.vendorID >= 0x10000) continue;
        if (!(gpu = calloc( 1, sizeof(*gpu) ))) break;
        memcpy( &gpu->uuid, devices[i].id.deviceUUID, sizeof(gpu->uuid) );
        gpu->name = strdup( devices[i].properties2.properties.deviceName );
        gpu->pci_id.vendor = devices[i].properties2.properties.vendorID;
        gpu->pci_id.device = devices[i].properties2.properties.deviceID;
        for (j = 0; j < devices[i].memory.memoryHeapCount; j++)
            if (devices[i].memory.memoryHeaps[j].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                gpu->memory += devices[i].memory.memoryHeaps[j].size;
        list_add_tail( gpus, &gpu->entry );
    }

    free( devices );
    return !list_empty( gpus );
}
