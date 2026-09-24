/*
 * Switch-side unixlibs for PE DLLs whose DllMain insists on resolving a
 * unixlib function table via NtQueryVirtualMemory(MemoryWineUnixFuncs).
 *
 * ws2_32's DNS entry points are backed by the libnx resolver (sfdnsres
 * service, IPv4 only); the rest remain STATUS_NOT_IMPLEMENTED until
 * they're wired to libnx or a Switch-portable crypto backend.
 *
 * Each table layout MUST match the corresponding enum unix_funcs in the
 * matching Wine source: ws2_32_private.h, crypt32_private.h, etc.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/unixlib.h"

static NTSTATUS stub_not_implemented( void *args )
{
    (void)args;
    return STATUS_NOT_IMPLEMENTED;
}

/* For DllMain process_attach hooks: returning SUCCESS without doing anything
 * is correct for "feature not available". The PE DllMain treats SUCCESS as
 * "subsystem initialized" and proceeds. Failures from later calls fail at
 * the API boundary instead of breaking DllMain. */
static NTSTATUS stub_success( void *args )
{
    (void)args;
    return STATUS_SUCCESS;
}

/* Windows-side types, mirrored from ws2_32 (64-bit PE layout).  The real
 * winsock headers can't be included next to the newlib socket headers. */
struct ws_addrinfo
{
    int   ai_flags;
    int   ai_family;
    int   ai_socktype;
    int   ai_protocol;
    ULONG_PTR ai_addrlen;
    char *ai_canonname;
    void *ai_addr;
    struct ws_addrinfo *ai_next;
};

struct getaddrinfo_params
{
    const char *node;
    const char *service;
    const struct ws_addrinfo *hints;
    struct ws_addrinfo *info;
    unsigned int *size;
};

struct gethostname_params
{
    char *name;
    unsigned int size;
};

#define WS_AF_INET 2
#define WS_ERROR_INSUFFICIENT_BUFFER 122
#define WSAEAFNOSUPPORT     10047
#define WSAHOST_NOT_FOUND   11001
#define WSATRY_AGAIN        11002
#define WSANO_RECOVERY      11003
#define WSA_NOT_ENOUGH_MEMORY 8

static NTSTATUS gai_error_from_unix( int err )
{
    switch (err)
    {
    case EAI_AGAIN:    return WSATRY_AGAIN;
    case EAI_MEMORY:   return WSA_NOT_ENOUGH_MEMORY;
    case EAI_FAMILY:   return WSAEAFNOSUPPORT;
    case EAI_NONAME:   return WSAHOST_NOT_FOUND;
    default:           return WSANO_RECOVERY;
    }
}

/* IPv4-only getaddrinfo through the libnx resolver.  Results are packed
 * into the caller's buffer as [ws_addrinfo][ws sockaddr_in][canonname],
 * mirroring what dlls/ws2_32/unixlib.c produces. */
extern void wine_nx_runtime_trace( const char *msg );

static NTSTATUS wine_nx_getaddrinfo( void *args )
{
    struct getaddrinfo_params *params = args;
    const char *service = params->service;
    struct addrinfo unix_hints = {0};
    struct addrinfo *unix_info, *src;
    char *out, *out_end;
    struct ws_addrinfo *dst, *prev = NULL;
    unsigned int needed_size = 0;
    int ret;
    char trace[256];

    snprintf( trace, sizeof(trace), "[GAI] getaddrinfo node='%s' service='%s' hints_family=%d",
              params->node ? params->node : "(null)", service ? service : "(null)",
              params->hints ? params->hints->ai_family : -1 );
    wine_nx_runtime_trace( trace );

    if (service && !service[0]) service = "0";

    unix_hints.ai_family = AF_INET; /* the resolver is IPv4 only */
    if (params->hints)
    {
        if (params->hints->ai_family && params->hints->ai_family != WS_AF_INET)
            return WSAEAFNOSUPPORT;
        unix_hints.ai_flags = params->hints->ai_flags & (AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST);
        unix_hints.ai_socktype = params->hints->ai_socktype; /* 1/2 match */
        unix_hints.ai_protocol = params->hints->ai_protocol > 0 ? params->hints->ai_protocol : 0;
    }

    ret = getaddrinfo( params->node, service, &unix_hints, &unix_info );
    if (ret)
    {
        snprintf( trace, sizeof(trace), "[GAI] libnx getaddrinfo failed ret=%d errno=%d -> wsa=%d",
                  ret, errno, (int)gai_error_from_unix( ret ) );
        wine_nx_runtime_trace( trace );
        return gai_error_from_unix( ret );
    }

    for (src = unix_info; src; src = src->ai_next)
    {
        if (src->ai_family != AF_INET) continue;
        needed_size += sizeof(struct ws_addrinfo) + 16;
        if (src->ai_canonname) needed_size += (strlen( src->ai_canonname ) + 1 + 7) & ~7u;
    }

    if (!needed_size)
    {
        wine_nx_runtime_trace( "[GAI] resolved but no IPv4 results" );
        freeaddrinfo( unix_info );
        return WSAHOST_NOT_FOUND;
    }

    if (*params->size < needed_size)
    {
        *params->size = needed_size;
        freeaddrinfo( unix_info );
        return WS_ERROR_INSUFFICIENT_BUFFER;
    }

    {
        const struct sockaddr_in *first = (const struct sockaddr_in *)unix_info->ai_addr;
        unsigned int ip = first ? ntohl( first->sin_addr.s_addr ) : 0;
        snprintf( trace, sizeof(trace), "[GAI] resolved: first=%u.%u.%u.%u size=%u",
                  (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff, needed_size );
        wine_nx_runtime_trace( trace );
    }

    out = (char *)params->info;
    out_end = out + needed_size;
    memset( out, 0, needed_size );

    for (src = unix_info; src; src = src->ai_next)
    {
        const struct sockaddr_in *sa = (const struct sockaddr_in *)src->ai_addr;
        unsigned short family = WS_AF_INET;
        char *addr;

        if (src->ai_family != AF_INET) continue;

        dst = (struct ws_addrinfo *)out;
        addr = out + sizeof(*dst);

        dst->ai_flags = src->ai_flags;
        dst->ai_family = WS_AF_INET;
        if (params->hints)
        {
            dst->ai_socktype = params->hints->ai_socktype;
            dst->ai_protocol = params->hints->ai_protocol;
        }
        else
        {
            dst->ai_socktype = src->ai_socktype;
            dst->ai_protocol = src->ai_protocol;
        }
        dst->ai_addrlen = 16;
        dst->ai_addr = addr;
        memcpy( addr, &family, sizeof(family) );
        memcpy( addr + 2, &sa->sin_port, 2 );
        memcpy( addr + 4, &sa->sin_addr, 4 );
        out = addr + 16;

        if (src->ai_canonname)
        {
            size_t len = strlen( src->ai_canonname ) + 1;
            dst->ai_canonname = out;
            memcpy( out, src->ai_canonname, len );
            out += (len + 7) & ~7u;
        }

        if (prev) prev->ai_next = dst;
        prev = dst;
    }
    (void)out_end;

    freeaddrinfo( unix_info );
    return 0;
}

static NTSTATUS wine_nx_gethostname( void *args )
{
    struct gethostname_params *params = args;
    static const char name[] = "wine-nx";

    if (params->size < sizeof(name)) return WS_ERROR_INSUFFICIENT_BUFFER;
    memcpy( params->name, name, sizeof(name) );
    return 0;
}

/* ws2_32: 5 DNS / hostname helpers. Layout: ws_unix_funcs in
 * dlls/ws2_32/ws2_32_private.h. */
const unixlib_entry_t wine_nx_ws2_32_unix_funcs[] =
{
    wine_nx_getaddrinfo,   /* unix_getaddrinfo */
    stub_not_implemented,  /* unix_gethostbyaddr */
    stub_not_implemented,  /* unix_gethostbyname */
    wine_nx_gethostname,   /* unix_gethostname */
    stub_not_implemented,  /* unix_getnameinfo */
};

/* 32-bit DLLs under WoW64 pass 32-bit argument structures, so the tables
 * above cannot serve them. These let the DLLs load; every call reports the
 * feature as unavailable. Sizes are checked against the Wine enums by
 * tests/check_wow64_unix_tables.py. */
const unixlib_entry_t wine_nx_ws2_32_wow64_unix_funcs[5] =
{
    stub_not_implemented,  /* unix_getaddrinfo */
    stub_not_implemented,  /* unix_gethostbyaddr */
    stub_not_implemented,  /* unix_gethostbyname */
    stub_not_implemented,  /* unix_gethostname */
    stub_not_implemented,  /* unix_getnameinfo */
};

/* The x86 unix call gate calls these tables only below their sizes. */
const unsigned int wine_nx_ws2_32_wow64_unix_count = ARRAY_SIZE(wine_nx_ws2_32_wow64_unix_funcs);

/* crypt32: enum unix_funcs in
 * dlls/crypt32/crypt32_private.h. process_attach must succeed for DllMain
 * to complete (it does global setup). The rest can fail until we have a
 * crypto backend. */
const unixlib_entry_t wine_nx_crypt32_unix_funcs[] =
{
    stub_success,          /* unix_process_attach */
    stub_success,          /* unix_process_detach */
    stub_not_implemented,  /* unix_open_cert_store */
    stub_not_implemented,  /* unix_import_store_key */
    stub_not_implemented,  /* unix_import_store_cert */
    stub_not_implemented,  /* unix_close_cert_store */
    stub_not_implemented,  /* unix_enum_root_certs */
    stub_not_implemented,  /* unix_export_cert_store */
};

/* The same for the 32-bit crypt32 a WoW64 program loads. Its DllMain returns
 * FALSE when the unix side is missing (dlls/crypt32/main.c), and a failed
 * DllMain of a statically imported DLL ends the process before it starts:
 * Quake III Arena imports crypt32 and stopped with c0000142. */
const unixlib_entry_t wine_nx_crypt32_wow64_unix_funcs[8] =
{
    stub_success,          /* unix_process_attach */
    stub_success,          /* unix_process_detach */
    stub_not_implemented,  /* unix_open_cert_store */
    stub_not_implemented,  /* unix_import_store_key */
    stub_not_implemented,  /* unix_import_store_cert */
    stub_not_implemented,  /* unix_close_cert_store */
    stub_not_implemented,  /* unix_enum_root_certs */
    stub_not_implemented,  /* unix_export_cert_store */
};

const unsigned int wine_nx_crypt32_wow64_unix_count = ARRAY_SIZE(wine_nx_crypt32_wow64_unix_funcs);
