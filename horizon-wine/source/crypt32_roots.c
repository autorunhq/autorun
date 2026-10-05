/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <switch.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wincrypt.h"
#include "../../dlls/crypt32/crypt32_private.h"

extern void wine_nx_runtime_trace(const char *message);

static pthread_mutex_t roots_lock = PTHREAD_MUTEX_INITIALIZER;
static SslBuiltInCertificateInfo *roots;
static unsigned int root_count, root_index;
static BOOL roots_loaded;

static NTSTATUS load_roots(void)
{
    u32 id = SslCaCertificateId_All, size, count;
    Result rc;
    void *buffer = NULL;
    unsigned int i;
    const char *stage = "initialize";
    char message[112];

    if (R_FAILED(rc = sslInitialize(1))) goto failed;
    stage = "buffer size";
    rc = sslGetCertificateBufSize(&id, 1, &size);
    if (R_SUCCEEDED(rc))
    {
        if (!(buffer = malloc(size)))
        {
            sslExit();
            return STATUS_NO_MEMORY;
        }
        stage = "certificate fetch";
        rc = sslGetCertificates(buffer, size, &id, 1, &count);
    }
    sslExit();
    if (R_FAILED(rc)) goto failed;
    if (count > size / sizeof(*roots)) goto invalid;
    for (i = 0; i < count; ++i)
    {
        const SslBuiltInCertificateInfo *cert = (SslBuiltInCertificateInfo *)buffer + i;
        uintptr_t offset = (uintptr_t)cert->cert_data - (uintptr_t)buffer;
        if (offset < count * sizeof(*roots) || offset > size || cert->cert_size > size - offset)
            goto invalid;
    }
    roots = buffer;
    root_count = count;
    roots_loaded = TRUE;
    return STATUS_SUCCESS;

invalid:
    free(buffer);
    return STATUS_INVALID_PARAMETER;
failed:
    snprintf(message, sizeof(message), "[TLS] Cannot read Horizon root certificates (%s): %#x", stage, rc);
    wine_nx_runtime_trace(message);
    free(buffer);
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS enum_root_certs(void *args)
{
    const struct enum_root_certs_params *params = args;
    NTSTATUS status = STATUS_SUCCESS;

    if (!params->needed) return STATUS_INVALID_PARAMETER;
    pthread_mutex_lock(&roots_lock);
    if (!roots_loaded && (status = load_roots())) goto done;
    while (root_index < root_count && roots[root_index].status != SslTrustedCertStatus_EnabledTrusted)
        ++root_index;
    if (root_index == root_count)
    {
        free(roots);
        roots = NULL;
        status = STATUS_NO_MORE_ENTRIES;
        goto done;
    }
    *params->needed = roots[root_index].cert_size;
    if (params->size >= *params->needed && params->buffer)
    {
        memcpy(params->buffer, roots[root_index].cert_data, *params->needed);
        ++root_index;
    }
done:
    pthread_mutex_unlock(&roots_lock);
    return status;
}

static NTSTATUS process_attach(void *args) { (void)args; return STATUS_SUCCESS; }

static NTSTATUS process_detach(void *args)
{
    (void)args;
    pthread_mutex_lock(&roots_lock);
    free(roots);
    roots = NULL;
    root_count = root_index = 0;
    roots_loaded = FALSE;
    pthread_mutex_unlock(&roots_lock);
    return STATUS_SUCCESS;
}

static NTSTATUS unsupported(void *args) { (void)args; return STATUS_NOT_IMPLEMENTED; }

static NTSTATUS wow64_enum_root_certs(void *args)
{
    const struct { ULONG buffer, size, needed; } *params32 = args;
    struct enum_root_certs_params params =
        { ULongToPtr(params32->buffer), params32->size, ULongToPtr(params32->needed) };
    return enum_root_certs(&params);
}

const unixlib_entry_t wine_nx_crypt32_unix_funcs[] =
{
    process_attach, process_detach, unsupported, unsupported, unsupported, unsupported,
    enum_root_certs, unsupported,
};

const unixlib_entry_t wine_nx_crypt32_wow64_unix_funcs[] =
{
    process_attach, process_detach, unsupported, unsupported, unsupported, unsupported,
    wow64_enum_root_certs, unsupported,
};

C_ASSERT(ARRAY_SIZE(wine_nx_crypt32_unix_funcs) == unix_funcs_count);
C_ASSERT(ARRAY_SIZE(wine_nx_crypt32_wow64_unix_funcs) == unix_funcs_count);
const unsigned int wine_nx_crypt32_wow64_unix_count = ARRAY_SIZE(wine_nx_crypt32_wow64_unix_funcs);
