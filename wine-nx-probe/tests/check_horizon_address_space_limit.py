#!/usr/bin/env python3
"""Check that ARM64EC exposes and validates only Horizon's address space."""
from pathlib import Path
import os
import subprocess
import tempfile


root = Path(__file__).resolve().parents[2]
source = Path(os.environ.get('WINE_NX_VIRTUAL_SOURCE', root / 'dlls/ntdll/unix/virtual.c')).read_text()


def block(marker):
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int BOOL;
#define FALSE 0
#define TRUE 1
#define __SWITCH__ 1
#define min(a,b) ((a) < (b) ? (a) : (b))
#define max(a,b) ((a) > (b) ? (a) : (b))
static const uintptr_t page_size = 0x1000;
static const uintptr_t granularity_mask = 0xffff;
static unsigned int cpu_count = 4;
static void *address_space_limit;
static void *user_space_limit;
static void *working_set_limit;
static void *host_addr_space_limit;
static int arm64ec;
static BOOL is_arm64ec(void) { return arm64ec; }
'''
fixture += block('static inline BOOL is_beyond_limit(')
fixture += block('static void cap_horizon_arm64ec_address_space(')
fixture += r'''
typedef struct
{
    unsigned long long MmHighestPhysicalPage;
    unsigned long long MmLowestPhysicalPage;
    unsigned long long MmNumberOfPhysicalPages;
    unsigned long long KeMaximumIncrement;
    uintptr_t PageSize;
    uintptr_t AllocationGranularity;
    void *LowestUserAddress;
    void *HighestUserAddress;
    uintptr_t ActiveProcessorsAffinityMask;
    unsigned int NumberOfProcessors;
    unsigned int unknown;
} SYSTEM_BASIC_INFORMATION;
static void horizon_get_memory_info(unsigned long long *total, unsigned long long *used)
{
    *total = 8ull << 30;
    *used = 1ull << 30;
}
static uintptr_t get_system_affinity_mask(void) { return 0xf; }
static uintptr_t get_wow_user_space_limit(void) { return 0x7fff0000; }
'''
fixture += block('void virtual_get_system_info(')
fixture += r'''
#define WINDOWS_LIMIT ((uintptr_t)0x7fffffff0000ull)
#define HORIZON_39BIT_LIMIT ((uintptr_t)0x8000000000ull)

static void reset_limits(uintptr_t host, int ec)
{
    address_space_limit = (void *)WINDOWS_LIMIT;
    user_space_limit = (void *)WINDOWS_LIMIT;
    working_set_limit = (void *)WINDOWS_LIMIT;
    host_addr_space_limit = (void *)host;
    arm64ec = ec;
}

int main(void)
{
    SYSTEM_BASIC_INFORMATION info;

    reset_limits(HORIZON_39BIT_LIMIT, TRUE);
    cap_horizon_arm64ec_address_space();
    assert((uintptr_t)address_space_limit == HORIZON_39BIT_LIMIT);
    assert((uintptr_t)user_space_limit == HORIZON_39BIT_LIMIT);
    assert((uintptr_t)working_set_limit == HORIZON_39BIT_LIMIT);
    memset(&info, 0, sizeof(info));
    virtual_get_system_info(&info, FALSE);
    assert((uintptr_t)info.HighestUserAddress == HORIZON_39BIT_LIMIT - 1);
    assert(!is_beyond_limit((void *)(HORIZON_39BIT_LIMIT - page_size), page_size,
                            address_space_limit));
    assert(is_beyond_limit((void *)HORIZON_39BIT_LIMIT, page_size, address_space_limit));

    reset_limits(0xffffffff0000ull, TRUE);
    cap_horizon_arm64ec_address_space();
    assert((uintptr_t)address_space_limit == WINDOWS_LIMIT);
    assert((uintptr_t)user_space_limit == WINDOWS_LIMIT);
    assert((uintptr_t)working_set_limit == WINDOWS_LIMIT);
    virtual_get_system_info(&info, FALSE);
    assert((uintptr_t)info.HighestUserAddress == WINDOWS_LIMIT - 1);

    reset_limits(HORIZON_39BIT_LIMIT, FALSE);
    cap_horizon_arm64ec_address_space();
    assert((uintptr_t)address_space_limit == WINDOWS_LIMIT);
    assert((uintptr_t)user_space_limit == WINDOWS_LIMIT);
    assert((uintptr_t)working_set_limit == WINDOWS_LIMIT);
    virtual_get_system_info(&info, FALSE);
    assert((uintptr_t)info.HighestUserAddress == WINDOWS_LIMIT - 1);
    assert(!is_beyond_limit((void *)HORIZON_39BIT_LIMIT, page_size, address_space_limit));

    address_space_limit = (void *)0xc0000000;
    user_space_limit = working_set_limit = (void *)0x7fff0000;
    host_addr_space_limit = (void *)HORIZON_39BIT_LIMIT;
    arm64ec = FALSE;
    cap_horizon_arm64ec_address_space();
    assert((uintptr_t)address_space_limit == 0xc0000000);
    assert((uintptr_t)user_space_limit == 0x7fff0000);
    assert((uintptr_t)working_set_limit == 0x7fff0000);

    puts("Horizon ARM64EC address-space cap: 39-bit, non-widening and non-EC cases passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='wine-nx-address-limit-') as temp:
    temp = Path(temp)
    test = temp / 'address_limit.c'
    binary = temp / 'address_limit'
    test.write_text(fixture)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
