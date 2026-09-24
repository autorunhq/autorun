#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "horizon_mman.h"
#include "unix_private.h"

static TEB smoke_teb;
static PEB smoke_peb;
static struct thread_data smoke_thread = { .teb = &smoke_teb, .tid = 1 };
static pthread_once_t smoke_once = PTHREAD_ONCE_INIT;
pthread_key_t thread_data_key;

static void smoke_thread_init(void)
{
    if (pthread_key_create( &thread_data_key, NULL )) abort();
    if (pthread_setspecific( thread_data_key, &smoke_thread )) abort();
}

struct _TEB * WINAPI NtCurrentTeb(void)
{
    pthread_once( &smoke_once, smoke_thread_init );
    smoke_teb.Tib.Self = &smoke_teb.Tib;
    smoke_teb.ClientId.UniqueProcess = (HANDLE)(ULONG_PTR)1;
    smoke_teb.ClientId.UniqueThread = (HANDLE)(ULONG_PTR)1;
    smoke_teb.Peb = &smoke_peb;
    return &smoke_teb;
}

void abort_thread( int status )
{
    fprintf( stderr, "wine-nx-ntdll-smoke: abort_thread(%d)\n", status );
    exit( status ? status : 1 );
}

void *anon_mmap_alloc( size_t size, int prot )
{
    return horizon_mmap( NULL, size, prot, MAP_PRIVATE | MAP_ANON, -1, 0 );
}
