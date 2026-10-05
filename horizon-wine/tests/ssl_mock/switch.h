#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint32_t u32, Result;
typedef uint64_t u64;
typedef struct { u32 cert_id, status; u64 cert_size; unsigned char *cert_data; } SslBuiltInCertificateInfo;
#define SslCaCertificateId_All -1
#define SslTrustedCertStatus_EnabledTrusted 1
#define SslTrustedCertStatus_EnabledNotTrusted 2
#define SslTrustedCertStatus_Revoked 3
#define R_FAILED(result) ((result) != 0)
#define R_SUCCEEDED(result) ((result) == 0)
Result sslInitialize(u32 count);
void sslExit(void);
Result sslGetCertificateBufSize(u32 *ids, u32 count, u32 *size);
Result sslGetCertificates(void *buffer, u32 size, u32 *ids, u32 count, u32 *total);
