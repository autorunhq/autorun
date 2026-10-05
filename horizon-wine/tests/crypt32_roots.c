#include <assert.h>
#include <stdio.h>
#include "../source/crypt32_roots.c"

static int initialize_count, exit_count, fetch_count;
static unsigned int fail_stage;
static BOOL malformed;
static char last_trace[128];

void wine_nx_runtime_trace(const char *text)
{
    snprintf(last_trace, sizeof(last_trace), "%s", text);
}

Result sslInitialize(u32 count)
{
    ++initialize_count;
    assert(count == 1);
    return fail_stage == 1 ? 0x6159 : 0;
}

void sslExit(void) { ++exit_count; }

Result sslGetCertificateBufSize(u32 *ids, u32 count, u32 *size)
{
    assert(count == 1 && ids[0] == UINT32_MAX);
    *size = 4 * sizeof(SslBuiltInCertificateInfo) + 12;
    return fail_stage == 2 ? 0x6159 : 0;
}

Result sslGetCertificates(void *buffer, u32 size, u32 *ids, u32 count, u32 *total)
{
    SslBuiltInCertificateInfo *certs = buffer;
    unsigned char *data = (unsigned char *)(certs + 4);
    static const unsigned int status[] = { 1, 2, 3, 1 };
    unsigned int i;

    (void)size; (void)ids; (void)count;
    ++fetch_count;
    if (fail_stage == 3) return 0x6159;
    *total = 4;
    for (i = 0; i < 4; ++i)
    {
        certs[i] = (SslBuiltInCertificateInfo){ i, status[i], 3, data + i * 3 };
        memset(certs[i].cert_data, i + 10, 3);
    }
    if (malformed) certs[3].cert_size = UINT64_MAX;
    return 0;
}

int main(void)
{
    unsigned char output[4] = { 0 };
    DWORD needed = 0;
    struct enum_root_certs_params params = { output, 2, &needed };

    assert(!process_attach(NULL));
    assert(!enum_root_certs(&params) && needed == 3 && root_index == 0);
    assert(output[0] == 0 && fetch_count == 1 && exit_count == 1);
    params.size = sizeof(output);
    assert(!enum_root_certs(&params) && output[0] == 10 && root_index == 1);
    assert(!enum_root_certs(&params) && output[0] == 13 && root_index == 4);
    assert(enum_root_certs(&params) == STATUS_NO_MORE_ENTRIES && !roots);
    assert(enum_root_certs(&params) == STATUS_NO_MORE_ENTRIES && fetch_count == 1);
    assert(!process_detach(NULL));
    fail_stage = 1;
    assert(enum_root_certs(&params) == STATUS_UNSUCCESSFUL && !roots_loaded);
    assert(strstr(last_trace, "(initialize): 0x6159"));
    assert(exit_count == 1);
    fail_stage = 2;
    assert(enum_root_certs(&params) == STATUS_UNSUCCESSFUL && !roots_loaded);
    assert(strstr(last_trace, "(buffer size): 0x6159"));
    assert(exit_count == 2);
    fail_stage = 3;
    assert(enum_root_certs(&params) == STATUS_UNSUCCESSFUL && !roots_loaded);
    assert(strstr(last_trace, "(certificate fetch): 0x6159"));
    assert(exit_count == 3);
    fail_stage = 0; malformed = TRUE;
    assert(enum_root_certs(&params) == STATUS_INVALID_PARAMETER && !roots && !roots_loaded);
    assert(exit_count == 4);
    malformed = FALSE;
    assert(!enum_root_certs(&params) && output[0] == 10);
    assert(!process_detach(NULL));
    puts("crypt32 roots: trust filtering, retry sizing, cleanup and service failures passed");
    return 0;
}
