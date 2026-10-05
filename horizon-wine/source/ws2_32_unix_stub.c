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
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
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

struct ws_hostent
{
    char *name;
    char **aliases;
    short family, length;
    char **addresses;
};

struct ws_hostent32
{
    ULONG name, aliases;
    short family, length;
    ULONG addresses;
};

static const char local_hostname[] = "wine-nx";

#define WS_AF_INET 2
#define WS_ERROR_INSUFFICIENT_BUFFER 122
#define WSAEFAULT           10014
#define WSAEINVAL           10022
#define WSAEAFNOSUPPORT     10047
#define WSAENOBUFS          10055
#define WSAHOST_NOT_FOUND   11001
#define WSATRY_AGAIN        11002
#define WSANO_RECOVERY      11003
#define WSANO_DATA          11004
#define WSA_NOT_ENOUGH_MEMORY 8

static NTSTATUS gai_error_from_unix( int err )
{
    switch (err)
    {
    case EAI_AGAIN:    return WSATRY_AGAIN;
    case EAI_MEMORY:   return WSA_NOT_ENOUGH_MEMORY;
    case EAI_FAMILY:   return WSAEAFNOSUPPORT;
    case EAI_NONAME:   return WSAHOST_NOT_FOUND;
#ifdef EAI_NODATA
    case EAI_NODATA:   return WSANO_DATA;
#endif
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

    if (params->size < sizeof(local_hostname)) return WS_ERROR_INSUFFICIENT_BUFFER;
    memcpy( params->name, local_hostname, sizeof(local_hostname) );
    return 0;
}

static NTSTATUS get_host_by_name( const char *name, void *buffer, unsigned int *size, BOOL wow64 )
{
    struct sockaddr_in local_address = { .sin_family = AF_INET };
    struct addrinfo local = { .ai_family = AF_INET, .ai_addr = (void *)&local_address };
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM, .ai_flags = AI_CANONNAME };
    struct addrinfo *info, *src;
    const char *canonical = name;
    unsigned int count = 0, i = 0, pointer_size = wow64 ? 4 : sizeof(void *), needed;
    char *aliases, *addresses, *data;
    int ret;

    if (!name || !*name || !strcasecmp( name, local_hostname ))
    {
        canonical = local_hostname;
        local_address.sin_addr.s_addr = gethostid();
        /* libnx returns host-order INADDR_LOOPBACK when no network is available. */
        if (local_address.sin_addr.s_addr == INADDR_LOOPBACK || !local_address.sin_addr.s_addr)
            local_address.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
        info = &local;
    }
    else if ((ret = getaddrinfo( name, NULL, &hints, &info ))) return gai_error_from_unix( ret );

    for (src = info; src; src = src->ai_next)
    {
        if (src->ai_family != AF_INET || !src->ai_addr) continue;
        if (src->ai_canonname) canonical = src->ai_canonname;
        count++;
    }
    if (!count) ret = WSAHOST_NOT_FOUND;
    else
    {
        needed = (wow64 ? sizeof(struct ws_hostent32) : sizeof(struct ws_hostent))
                 + (count + 2) * pointer_size + count * 4 + strlen(canonical) + 1;
        if (*size < needed)
        {
            *size = needed;
            ret = WS_ERROR_INSUFFICIENT_BUFFER;
        }
        else
        {
            memset( buffer, 0, needed );
            aliases = (char *)buffer + (wow64 ? sizeof(struct ws_hostent32) : sizeof(struct ws_hostent));
            addresses = aliases + pointer_size;
            data = addresses + (count + 1) * pointer_size;
            for (src = info; src; src = src->ai_next)
            {
                if (src->ai_family != AF_INET || !src->ai_addr) continue;
                if (wow64) ((ULONG *)addresses)[i++] = PtrToUlong( data );
                else ((char **)addresses)[i++] = data;
                memcpy( data, &((struct sockaddr_in *)src->ai_addr)->sin_addr, 4 );
                data += 4;
            }
            strcpy( data, canonical );
            if (wow64)
            {
                struct ws_hostent32 *host = buffer;
                host->name = PtrToUlong( data );
                host->aliases = PtrToUlong( aliases );
                host->addresses = PtrToUlong( addresses );
                host->family = WS_AF_INET;
                host->length = 4;
            }
            else
            {
                struct ws_hostent *host = buffer;
                host->name = data;
                host->aliases = (char **)aliases;
                host->addresses = (char **)addresses;
                host->family = WS_AF_INET;
                host->length = 4;
            }
            ret = 0;
        }
    }
    if (info != &local) freeaddrinfo( info );
    return ret;
}

static NTSTATUS wine_nx_gethostbyname( void *args )
{
    const struct { const char *name; void *host; unsigned int *size; } *params = args;
    return get_host_by_name( params->name, params->host, params->size, FALSE );
}

static NTSTATUS pack_hostent( const struct hostent *src, void *buffer, unsigned int *size, BOOL wow64 )
{
    unsigned int aliases_count = 0, address_count = 0, pointer_size = wow64 ? 4 : sizeof(void *);
    size_t needed = wow64 ? sizeof(struct ws_hostent32) : sizeof(struct ws_hostent);
    char *aliases, *addresses, *data;

    if (src->h_addrtype != AF_INET || src->h_length != 4) return WSAEAFNOSUPPORT;
    for (; src->h_aliases[aliases_count]; aliases_count++)
        needed += strlen( src->h_aliases[aliases_count] ) + 1;
    while (src->h_addr_list[address_count]) address_count++;
    needed += (aliases_count + address_count + 2) * pointer_size + address_count * 4 + strlen( src->h_name ) + 1;
    if (needed > UINT_MAX) return WSAENOBUFS;
    if (*size < needed)
    {
        *size = needed;
        return WS_ERROR_INSUFFICIENT_BUFFER;
    }

    memset( buffer, 0, needed );
    aliases = (char *)buffer + (wow64 ? sizeof(struct ws_hostent32) : sizeof(struct ws_hostent));
    addresses = aliases + (aliases_count + 1) * pointer_size;
    data = addresses + (address_count + 1) * pointer_size;
    for (unsigned int i = 0; i < address_count; i++)
    {
        if (wow64) ((ULONG *)addresses)[i] = PtrToUlong( data );
        else ((char **)addresses)[i] = data;
        memcpy( data, src->h_addr_list[i], 4 );
        data += 4;
    }
    for (unsigned int i = 0; i < aliases_count; i++)
    {
        size_t length = strlen( src->h_aliases[i] ) + 1;
        if (wow64) ((ULONG *)aliases)[i] = PtrToUlong( data );
        else ((char **)aliases)[i] = data;
        memcpy( data, src->h_aliases[i], length );
        data += length;
    }
    strcpy( data, src->h_name );
    if (wow64)
    {
        struct ws_hostent32 *host = buffer;
        host->name = PtrToUlong( data );
        host->aliases = PtrToUlong( aliases );
        host->addresses = PtrToUlong( addresses );
        host->family = WS_AF_INET;
        host->length = 4;
    }
    else
    {
        struct ws_hostent *host = buffer;
        host->name = data;
        host->aliases = (char **)aliases;
        host->addresses = (char **)addresses;
        host->family = WS_AF_INET;
        host->length = 4;
    }
    return 0;
}

static NTSTATUS get_host_by_addr( const void *addr, int length, int family,
                                 void *buffer, unsigned int *size, BOOL wow64 )
{
    struct in_addr ip, local_ip;
    char *aliases[] = { NULL }, *addresses[] = { (char *)&ip, NULL };
    struct hostent local = { (char *)local_hostname, aliases, AF_INET, 4, addresses }, *host;
    NTSTATUS status;

    if (family != WS_AF_INET) return WSAEAFNOSUPPORT;
    if (!addr || length < (int)sizeof(ip)) return WSAEFAULT;
    memcpy( &ip, addr, sizeof(ip) );
    /* Wine uses 127.12.34.56 for the local host when it has no network address. */
    if (ip.s_addr == htonl( 0x7f0c2238 )) ip.s_addr = htonl( INADDR_LOOPBACK );
    local_ip.s_addr = gethostid();
    if (local_ip.s_addr == INADDR_LOOPBACK) local_ip.s_addr = htonl( INADDR_LOOPBACK );
    if (ip.s_addr == htonl( INADDR_LOOPBACK ) || (local_ip.s_addr && ip.s_addr == local_ip.s_addr))
        return pack_hostent( &local, buffer, size, wow64 );

    if (!(host = gethostbyaddr( &ip, sizeof(ip), AF_INET )))
    {
        switch (h_errno)
        {
        case HOST_NOT_FOUND: return WSAHOST_NOT_FOUND;
        case TRY_AGAIN: return WSATRY_AGAIN;
        case NO_DATA: return WSANO_DATA;
        case NETDB_INTERNAL:
            switch (errno)
            {
            case ENOMEM: case ENOBUFS: case ENOSPC: return WSAENOBUFS;
            case EAGAIN: return WSATRY_AGAIN;
            case EINVAL: return WSAEINVAL;
            case EFAULT: return WSAEFAULT;
            }
        }
        return WSANO_RECOVERY;
    }
    status = pack_hostent( host, buffer, size, wow64 );
    freehostent( host );
    return status;
}

static NTSTATUS wine_nx_gethostbyaddr( void *args )
{
    const struct { const void *addr; int len, family; void *host; unsigned int *size; } *params = args;
    return get_host_by_addr( params->addr, params->len, params->family, params->host, params->size, FALSE );
}

static NTSTATUS wine_nx_wow64_gethostbyaddr( void *args )
{
    const struct { ULONG addr; int len, family; ULONG host, size; } *params = args;
    return get_host_by_addr( ULongToPtr(params->addr), params->len, params->family,
                             ULongToPtr(params->host), ULongToPtr(params->size), TRUE );
}

static NTSTATUS wine_nx_wow64_gethostbyname( void *args )
{
    const struct { ULONG name, host, size; } *params = args;
    return get_host_by_name( ULongToPtr(params->name), ULongToPtr(params->host), ULongToPtr(params->size), TRUE );
}

static NTSTATUS wine_nx_wow64_gethostname( void *args )
{
    const struct { ULONG name; unsigned int size; } *params32 = args;
    struct gethostname_params params = { ULongToPtr(params32->name), params32->size };
    return wine_nx_gethostname( &params );
}

/* ws2_32: 5 DNS / hostname helpers. Layout: ws_unix_funcs in
 * dlls/ws2_32/ws2_32_private.h. */
const unixlib_entry_t wine_nx_ws2_32_unix_funcs[] =
{
    wine_nx_getaddrinfo,   /* unix_getaddrinfo */
    wine_nx_gethostbyaddr,  /* unix_gethostbyaddr */
    wine_nx_gethostbyname,  /* unix_gethostbyname */
    wine_nx_gethostname,   /* unix_gethostname */
    stub_not_implemented,  /* unix_getnameinfo */
};

const unixlib_entry_t wine_nx_ws2_32_wow64_unix_funcs[5] =
{
    stub_not_implemented,  /* unix_getaddrinfo */
    wine_nx_wow64_gethostbyaddr, /* unix_gethostbyaddr */
    wine_nx_wow64_gethostbyname, /* unix_gethostbyname */
    wine_nx_wow64_gethostname,  /* unix_gethostname */
    stub_not_implemented,  /* unix_getnameinfo */
};

/* The x86 unix call gate calls these tables only below their sizes. */
const unsigned int wine_nx_ws2_32_wow64_unix_count = ARRAY_SIZE(wine_nx_ws2_32_wow64_unix_funcs);
