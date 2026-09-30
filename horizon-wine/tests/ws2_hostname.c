#include <assert.h>
#include <sys/mman.h>

#define gethostid mock_gethostid
#define getaddrinfo mock_getaddrinfo
#define freeaddrinfo mock_freeaddrinfo
#define gethostbyaddr mock_gethostbyaddr
#define freehostent mock_freehostent
struct hostent;
void mock_freehostent( struct hostent *host );
#include "../source/ws2_32_unix_stub.c"

static long local_ip;
static int dns_error, dns_calls, dns_frees;
static struct sockaddr_in addresses[2];
static struct addrinfo records[2];
static int reverse_error, reverse_errno, reverse_calls, reverse_frees;
static struct hostent reverse_host;
static uint32_t reverse_ip;

long mock_gethostid(void) { return local_ip; }
void wine_nx_runtime_trace( const char *message ) { (void)message; }
int mock_getaddrinfo( const char *name, const char *service, const struct addrinfo *hints, struct addrinfo **out )
{
    assert( name && !strcmp(name, "example.test") );
    assert( !service && hints->ai_family == AF_INET && hints->ai_socktype == SOCK_STREAM );
    dns_calls++;
    *out = records;
    return dns_error;
}
void mock_freeaddrinfo( struct addrinfo *info ) { assert(info == records); dns_frees++; }
struct hostent *mock_gethostbyaddr( const void *addr, socklen_t length, int family )
{
    assert( family == AF_INET && length == 4 && !memcmp( addr, &reverse_ip, 4 ) );
    reverse_calls++;
    h_errno = reverse_error;
    errno = reverse_errno;
    return reverse_error ? NULL : &reverse_host;
}
void mock_freehostent( struct hostent *host ) { assert(host == &reverse_host); reverse_frees++; }

static void check_host( void *buffer, BOOL wow64, unsigned int count, const char *name, uint32_t ip )
{
    char *hostname;
    void *first;
    if (wow64)
    {
        struct ws_hostent32 *host = buffer;
        ULONG *list = ULongToPtr(host->addresses);
        hostname = ULongToPtr(host->name);
        assert(host->family == WS_AF_INET && host->length == 4);
        assert(!*(ULONG *)ULongToPtr(host->aliases));
        assert(!list[count]);
        first = ULongToPtr(list[0]);
    }
    else
    {
        struct ws_hostent *host = buffer;
        hostname = host->name;
        assert(host->family == WS_AF_INET && host->length == 4);
        assert(!host->aliases[0] && !host->addresses[count]);
        first = host->addresses[0];
    }
    assert(!strcmp(hostname, name));
    assert(!memcmp(first, &ip, 4));
}

static void test_reverse( char *memory )
{
    char *aliases[] = { "alias.test", "second.test", NULL };
    char *addresses[] = { (char *)&reverse_ip, NULL };
    uint32_t *ip = (void *)(memory + 0x3000);
    unsigned int *size = (void *)(memory + 0x1000), needed;
    void *buffer = memory + 0x2000;
    struct { const void *addr; int len, family; void *host; unsigned int *size; }
        params = { ip, 4, WS_AF_INET, buffer, size };
    struct { ULONG addr; int len, family; ULONG host, size; }
        params32 = { PtrToUlong(ip), 4, WS_AF_INET, PtrToUlong(buffer), PtrToUlong(size) };

    for (int wow64 = 0; wow64 < 2; wow64++)
    {
        unixlib_entry_t resolve = wow64 ? wine_nx_ws2_32_wow64_unix_funcs[1] : wine_nx_ws2_32_unix_funcs[1];
        void *args = wow64 ? (void *)&params32 : &params;
        local_ip = htonl(0xc0a8010a);
        *size = 1024;
        *ip = htonl(INADDR_LOOPBACK);
        assert(!resolve(args));
        check_host(buffer, wow64, 1, "wine-nx", *ip);
        *ip = htonl(0x7f0c2238);
        assert(!resolve(args));
        check_host(buffer, wow64, 1, "wine-nx", htonl(INADDR_LOOPBACK));
        *ip = local_ip;
        assert(!resolve(args));
        check_host(buffer, wow64, 1, "wine-nx", *ip);

        reverse_ip = *ip = htonl(0x0a000001);
        reverse_host = (struct hostent){ "canonical.test", aliases, AF_INET, 4, addresses };
        *size = 0;
        assert(resolve(args) == WS_ERROR_INSUFFICIENT_BUFFER);
        needed = *size;
        memset(buffer, 0x5a, needed + 1);
        *size = needed - 1;
        assert(resolve(args) == WS_ERROR_INSUFFICIENT_BUFFER);
        assert(*size == needed && *(unsigned char *)buffer == 0x5a);
        assert(!resolve(args));
        assert(((unsigned char *)buffer)[needed] == 0x5a);
        if (wow64)
        {
            struct ws_hostent32 *host = buffer;
            ULONG *list = ULongToPtr(host->aliases), *ips = ULongToPtr(host->addresses);
            assert(host->family == WS_AF_INET && host->length == 4);
            assert(!strcmp(ULongToPtr(host->name), "canonical.test"));
            assert(!strcmp(ULongToPtr(list[0]), aliases[0]));
            assert(!strcmp(ULongToPtr(list[1]), aliases[1]) && !list[2]);
            assert(!memcmp(ULongToPtr(ips[0]), &reverse_ip, 4) && !ips[1]);
        }
        else
        {
            struct ws_hostent *host = buffer;
            assert(host->family == WS_AF_INET && host->length == 4);
            assert(!strcmp(host->name, "canonical.test"));
            assert(!strcmp(host->aliases[0], aliases[0]));
            assert(!strcmp(host->aliases[1], aliases[1]) && !host->aliases[2]);
            assert(!memcmp(host->addresses[0], &reverse_ip, 4) && !host->addresses[1]);
        }
        assert(reverse_calls == reverse_frees);
    }
    assert(reverse_calls == 6);
    assert(get_host_by_addr(ip, -1, WS_AF_INET, buffer, size, TRUE) == WSAEFAULT);
    assert(get_host_by_addr(ip, 3, WS_AF_INET, buffer, size, TRUE) == WSAEFAULT);
    assert(get_host_by_addr(NULL, 4, WS_AF_INET, buffer, size, TRUE) == WSAEFAULT);
    assert(get_host_by_addr(ip, 4, 23, buffer, size, TRUE) == WSAEAFNOSUPPORT);
    assert(reverse_calls == 6);
    static const struct { int error, native_errno, result; } errors[] = {
        { HOST_NOT_FOUND, 0, WSAHOST_NOT_FOUND }, { TRY_AGAIN, 0, WSATRY_AGAIN },
        { NO_RECOVERY, 0, WSANO_RECOVERY }, { NO_DATA, 0, WSANO_DATA },
        { NETDB_INTERNAL, ENOMEM, WSAENOBUFS }, { NETDB_INTERNAL, EAGAIN, WSATRY_AGAIN },
        { NETDB_INTERNAL, EFAULT, WSAEFAULT }, { NETDB_INTERNAL, EINVAL, WSAEINVAL }
    };
    for (unsigned int i = 0; i < ARRAY_SIZE(errors); i++)
    {
        reverse_error = errors[i].error;
        reverse_errno = errors[i].native_errno;
        assert(wine_nx_gethostbyaddr(&params) == errors[i].result);
        assert(wine_nx_wow64_gethostbyaddr(&params32) == errors[i].result);
    }
    assert(reverse_frees == 6);
}

int main(void)
{
    char *memory = mmap((void *)0x18000000, 0x10000, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    unsigned int *size = (void *)(memory + 0x1000), needed;
    struct { ULONG name, host, size; } byname;
    struct { ULONG name; unsigned int size; } hostname;
    int wide;

    assert(memory != MAP_FAILED);
    hostname.name = PtrToUlong(memory);
    hostname.size = 1;
    assert(wine_nx_wow64_gethostname(&hostname) == WS_ERROR_INSUFFICIENT_BUFFER);
    hostname.size = 100;
    assert(!wine_nx_wow64_gethostname(&hostname));
    assert(!strcmp(memory, "wine-nx"));
    byname.name = PtrToUlong(memory);
    byname.host = PtrToUlong(memory + 0x2000);
    byname.size = PtrToUlong(size);
    local_ip = htonl(0xc0a8010a);
    *size = 0;
    assert(wine_nx_ws2_32_wow64_unix_funcs[2](&byname) == WS_ERROR_INSUFFICIENT_BUFFER);
    needed = *size;
    memset(memory + 0x2000, 0x5a, needed + 1);
    *size = needed - 1;
    assert(wine_nx_ws2_32_wow64_unix_funcs[2](&byname) == WS_ERROR_INSUFFICIENT_BUFFER);
    assert((unsigned char)memory[0x2000] == 0x5a);
    assert(*size == needed);
    assert(!wine_nx_ws2_32_wow64_unix_funcs[2](&byname));
    check_host(memory + 0x2000, TRUE, 1, "wine-nx", local_ip);
    assert((unsigned char)memory[0x2000 + needed] == 0x5a);
    assert(!dns_calls);

    records[0].ai_family = records[1].ai_family = AF_INET;
    records[0].ai_addr = (void *)&addresses[0];
    records[1].ai_addr = (void *)&addresses[1];
    records[0].ai_next = &records[1];
    records[0].ai_canonname = "canonical.test";
    addresses[0].sin_addr.s_addr = htonl(0x0a000001);
    addresses[1].sin_addr.s_addr = htonl(0x0a000002);
    for (wide = 0; wide < 2; wide++)
    {
        *size = 1024;
        local_ip = INADDR_LOOPBACK;
        assert(!get_host_by_name("WINE-NX", memory + 0x2000, size, !wide));
        check_host(memory + 0x2000, !wide, 1, "wine-nx", htonl(INADDR_LOOPBACK));
        assert(!get_host_by_name("example.test", memory + 0x2000, size, !wide));
        check_host(memory + 0x2000, !wide, 2, "canonical.test", addresses[0].sin_addr.s_addr);
    }
    assert(dns_calls == 2 && dns_frees == 2);
    dns_error = EAI_AGAIN;
    assert(get_host_by_name("example.test", memory + 0x2000, size, TRUE) == WSATRY_AGAIN);
#ifdef EAI_NODATA
    dns_error = EAI_NODATA;
    assert(get_host_by_name("example.test", memory + 0x2000, size, TRUE) == WSANO_DATA);
#endif
    assert(dns_frees == 2);
    test_reverse(memory);
    assert(!munmap(memory, 0x10000));
    puts("Winsock hostname tests passed");
}
