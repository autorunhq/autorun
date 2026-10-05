/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/rsa.h>
#ifdef __SWITCH__
#define Service HorizonService
#include <switch.h>
#undef Service
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "bcrypt.h"
#include "sspi.h"
#include "../../dlls/secur32/secur32_priv.h"
#include "wine/unixlib.h"

#ifdef __SWITCH__
extern void wine_nx_runtime_trace(const char *message);
#endif

static void tls_error(int error)
{
    char message[160], detail[120];
    mbedtls_strerror(error, detail, sizeof(detail));
    snprintf(message, sizeof(message), "[TLS] %s (%d)", detail, error);
#ifdef __SWITCH__
    wine_nx_runtime_trace(message);
#else
    fprintf(stderr, "%s\n", message);
#endif
}

struct tls_credentials
{
    unsigned int refs;
    pthread_mutex_t key_lock;
    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
};

struct tls_buffers
{
    const SecBufferDesc *desc;
    unsigned int index;
    size_t offset, remaining, used;
};

struct tls_session
{
    pthread_mutex_t lock;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_ctr_drbg_context rng;
#ifndef __SWITCH__
    mbedtls_entropy_context entropy;
#endif
    struct tls_credentials *credentials;
    struct tls_buffers input, output;
    char **alpn;
    BOOL server;
};

static struct tls_session *session_from_handle(schan_session handle)
{
    return (struct tls_session *)(ULONG_PTR)handle;
}

static void init_buffers(struct tls_buffers *buffers, const SecBufferDesc *desc, size_t limit)
{
    *buffers = (struct tls_buffers){ .desc = desc, .remaining = limit };
}

static int transfer(struct tls_buffers *buffers, unsigned char *data, size_t size, BOOL output)
{
    SecBuffer *buffer;
    size_t available;

    if (!size) return 0;
    while (buffers->desc && buffers->index < buffers->desc->cBuffers && buffers->remaining)
    {
        buffer = &buffers->desc->pBuffers[buffers->index];
        if (buffers->offset > buffer->cbBuffer) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
        available = buffer->cbBuffer - buffers->offset;
        if (!available)
        {
            if (buffers->index + 1 == buffers->desc->cBuffers) break;
            ++buffers->index;
            buffers->offset = 0;
            continue;
        }
        if (!buffer->pvBuffer) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
        if (size > available) size = available;
        if (size > buffers->remaining) size = buffers->remaining;
        if (size > INT_MAX) size = INT_MAX;
        if (output) memcpy((char *)buffer->pvBuffer + buffers->offset, data, size);
        else memcpy(data, (char *)buffer->pvBuffer + buffers->offset, size);
        buffers->offset += size;
        buffers->used += size;
        buffers->remaining -= size;
        return size;
    }
    return output ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_WANT_READ;
}

static int pull(void *context, unsigned char *data, size_t size)
{
    return transfer(&((struct tls_session *)context)->input, data, size, FALSE);
}

static int push(void *context, const unsigned char *data, size_t size)
{
    return transfer(&((struct tls_session *)context)->output, (unsigned char *)data, size, TRUE);
}

static void output_position(const struct tls_session *session, int *index, ULONG *offset)
{
    *index = session->output.used ? (int)session->output.index : -1;
    *offset = session->output.offset;
}

static NTSTATUS tls_status(int error)
{
    switch (error)
    {
    case 0: return SEC_E_OK;
    case MBEDTLS_ERR_SSL_WANT_READ: return SEC_I_CONTINUE_NEEDED;
    case MBEDTLS_ERR_SSL_WANT_WRITE: return SEC_E_BUFFER_TOO_SMALL;
    case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY: return SEC_I_CONTEXT_EXPIRED;
    case MBEDTLS_ERR_SSL_ALLOC_FAILED: return SEC_E_INSUFFICIENT_MEMORY;
    case MBEDTLS_ERR_SSL_INVALID_MAC: return SEC_E_MESSAGE_ALTERED;
    case MBEDTLS_ERR_SSL_BAD_INPUT_DATA: return SEC_E_INVALID_TOKEN;
    default:
        tls_error(error);
        return SEC_E_INTERNAL_ERROR;
    }
}

#ifdef __SWITCH__
static int entropy(void *context, unsigned char *data, size_t size)
{
    (void)context;
    return R_SUCCEEDED(csrngGetRandomBytes(data, size)) ? 0 : MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
}
#endif

static NTSTATUS process_attach(void *args)
{
    (void)args;
#ifdef __SWITCH__
    Result rc = csrngInitialize();
    if (R_FAILED(rc))
    {
        wine_nx_runtime_trace("[TLS] Cannot initialize Horizon entropy service");
        return STATUS_UNSUCCESSFUL;
    }
#endif
    return STATUS_SUCCESS;
}

static NTSTATUS process_detach(void *args)
{
    (void)args;
#ifdef __SWITCH__
    csrngExit();
#endif
    return STATUS_SUCCESS;
}

static void release_credentials(struct tls_credentials *credentials)
{
    if (!__atomic_sub_fetch(&credentials->refs, 1, __ATOMIC_ACQ_REL))
    {
        mbedtls_x509_crt_free(&credentials->cert);
        mbedtls_pk_free(&credentials->key);
        pthread_mutex_destroy(&credentials->key_lock);
        free(credentials);
    }
}

static int import_rsa_key(mbedtls_pk_context *key, const BYTE *blob, size_t size)
{
    const BCRYPT_RSAKEY_BLOB *header;
    const BYTE *exponent, *modulus, *prime1, *prime2, *private_exponent;
    uint64_t required;
    int error;

    if (size < sizeof(*header)) return MBEDTLS_ERR_PK_BAD_INPUT_DATA;
    header = (const BCRYPT_RSAKEY_BLOB *)blob;
    required = sizeof(*header) + (uint64_t)header->cbPublicExp + 2ull * header->cbModulus +
               3ull * header->cbPrime1 + 2ull * header->cbPrime2;
    if (header->Magic != BCRYPT_RSAFULLPRIVATE_MAGIC || required > size ||
        !header->cbPublicExp || !header->cbModulus || !header->cbPrime1 || !header->cbPrime2)
        return MBEDTLS_ERR_PK_BAD_INPUT_DATA;
    exponent = (const BYTE *)(header + 1);
    modulus = exponent + header->cbPublicExp;
    prime1 = modulus + header->cbModulus;
    prime2 = prime1 + header->cbPrime1;
    private_exponent = prime2 + 2ull * header->cbPrime2 + 2ull * header->cbPrime1;
    if ((error = mbedtls_pk_setup(key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)))) return error;
    if ((error = mbedtls_rsa_import_raw(mbedtls_pk_rsa(*key), modulus, header->cbModulus,
                                      prime1, header->cbPrime1, prime2, header->cbPrime2,
                                      private_exponent, header->cbModulus,
                                      exponent, header->cbPublicExp))) return error;
    if ((error = mbedtls_rsa_complete(mbedtls_pk_rsa(*key)))) return error;
    return mbedtls_rsa_check_privkey(mbedtls_pk_rsa(*key));
}

static NTSTATUS schan_allocate_certificate_credentials(void *args)
{
    const struct allocate_certificate_credentials_params *params = args;
    struct tls_credentials *credentials;
    int error = 0;

    params->c->credentials = 0;
    if (!(credentials = calloc(1, sizeof(*credentials)))) return STATUS_NO_MEMORY;
    if (pthread_mutex_init(&credentials->key_lock, NULL))
    {
        free(credentials);
        return STATUS_UNSUCCESSFUL;
    }
    credentials->refs = 1;
    mbedtls_x509_crt_init(&credentials->cert);
    mbedtls_pk_init(&credentials->key);
    if (params->cert_blob)
    {
        if (params->cert_encoding != X509_ASN_ENCODING || !params->key_blob)
            error = MBEDTLS_ERR_PK_BAD_INPUT_DATA;
        else if (!(error = mbedtls_x509_crt_parse_der(&credentials->cert, params->cert_blob, params->cert_size)))
            error = import_rsa_key(&credentials->key, params->key_blob, params->key_size);
        if (!error) error = mbedtls_pk_check_pair(&credentials->cert.pk, &credentials->key);
    }
    if (error)
    {
        release_credentials(credentials);
        return tls_status(error);
    }
    params->c->credentials = (ULONG_PTR)credentials;
    return STATUS_SUCCESS;
}

static NTSTATUS schan_free_certificate_credentials(void *args)
{
    const struct free_certificate_credentials_params *params = args;
    release_credentials((struct tls_credentials *)(ULONG_PTR)params->c->credentials);
    params->c->credentials = 0;
    return STATUS_SUCCESS;
}

static void free_session(struct tls_session *session)
{
    unsigned int i;

    mbedtls_ssl_free(&session->ssl);
    mbedtls_ssl_config_free(&session->config);
    mbedtls_ctr_drbg_free(&session->rng);
#ifndef __SWITCH__
    mbedtls_entropy_free(&session->entropy);
#endif
    if (session->alpn)
    {
        for (i = 0; session->alpn[i]; ++i) free(session->alpn[i]);
        free(session->alpn);
    }
    if (session->credentials) release_credentials(session->credentials);
    pthread_mutex_destroy(&session->lock);
    free(session);
}

static NTSTATUS schan_get_enabled_protocols(void *args)
{
    (void)args;
    return SP_PROT_TLS1_0_CLIENT | SP_PROT_TLS1_1_CLIENT | SP_PROT_TLS1_2_CLIENT |
           SP_PROT_TLS1_0_SERVER | SP_PROT_TLS1_1_SERVER | SP_PROT_TLS1_2_SERVER;
}

static NTSTATUS schan_create_session(void *args)
{
    const struct create_session_params *params = args;
    const schan_credentials *cred = params->cred;
    static const int disabled_ciphers[] = { 0 };
    static const DWORD client_protocols[] = { SP_PROT_TLS1_0_CLIENT, SP_PROT_TLS1_1_CLIENT, SP_PROT_TLS1_2_CLIENT };
    static const DWORD server_protocols[] = { SP_PROT_TLS1_0_SERVER, SP_PROT_TLS1_1_SERVER, SP_PROT_TLS1_2_SERVER };
    struct tls_session *session;
    const DWORD *protocols;
    unsigned int i, minimum = 4, maximum = 0;
    int error;

    *params->session = 0;
    if (cred->enabled_protocols & SP_PROT_DTLS1_X) return STATUS_NOT_SUPPORTED;
    if (!(session = calloc(1, sizeof(*session)))) return STATUS_NO_MEMORY;
    if (pthread_mutex_init(&session->lock, NULL))
    {
        free(session);
        return STATUS_UNSUCCESSFUL;
    }
    session->server = cred->credential_use == SECPKG_CRED_INBOUND;
    mbedtls_ssl_init(&session->ssl);
    mbedtls_ssl_config_init(&session->config);
    mbedtls_ctr_drbg_init(&session->rng);
#ifdef __SWITCH__
    error = mbedtls_ctr_drbg_seed(&session->rng, entropy, NULL, NULL, 0);
#else
    mbedtls_entropy_init(&session->entropy);
    error = mbedtls_ctr_drbg_seed(&session->rng, mbedtls_entropy_func, &session->entropy, NULL, 0);
#endif
    if (error) goto failed;
    error = mbedtls_ssl_config_defaults(&session->config,
        session->server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (error) goto failed;
    protocols = session->server ? server_protocols : client_protocols;
    for (i = 0; i < ARRAY_SIZE(client_protocols); ++i)
    {
        if (cred->enabled_protocols & protocols[i])
        {
            if (minimum > i + 1) minimum = i + 1;
            maximum = i + 1;
        }
        else mbedtls_ssl_conf_ciphersuites_for_version(&session->config, disabled_ciphers, 3, i + 1);
    }
    if (!maximum)
    {
        free_session(session);
        return SEC_E_ALGORITHM_MISMATCH;
    }
    mbedtls_ssl_conf_min_version(&session->config, 3, minimum);
    mbedtls_ssl_conf_max_version(&session->config, 3, maximum);
    mbedtls_ssl_conf_rng(&session->config, mbedtls_ctr_drbg_random, &session->rng);
    /* Wine's callers validate the exported peer chain through crypt32. */
    mbedtls_ssl_conf_authmode(&session->config, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_cbc_record_splitting(&session->config, MBEDTLS_SSL_CBC_RECORD_SPLITTING_DISABLED);
    mbedtls_ssl_conf_session_tickets(&session->config, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
    session->credentials = (struct tls_credentials *)(ULONG_PTR)cred->credentials;
    __atomic_add_fetch(&session->credentials->refs, 1, __ATOMIC_RELAXED);
    if (session->credentials->cert.raw.p)
    {
        error = mbedtls_ssl_conf_own_cert(&session->config, &session->credentials->cert, &session->credentials->key);
        if (error) goto failed;
    }
    if ((error = mbedtls_ssl_setup(&session->ssl, &session->config))) goto failed;
    mbedtls_ssl_set_bio(&session->ssl, session, push, pull, NULL);
    *params->session = (ULONG_PTR)session;
    return STATUS_SUCCESS;
failed:
    free_session(session);
    return tls_status(error);
}

static NTSTATUS schan_dispose_session(void *args)
{
    const struct session_params *params = args;
    free_session(session_from_handle(params->session));
    return STATUS_SUCCESS;
}

static NTSTATUS schan_set_session_target(void *args)
{
    const struct set_session_target_params *params = args;
    struct tls_session *session = session_from_handle(params->session);
    int error;
    pthread_mutex_lock(&session->lock);
    error = mbedtls_ssl_set_hostname(&session->ssl, params->target);
    pthread_mutex_unlock(&session->lock);
    return tls_status(error);
}

static NTSTATUS schan_handshake(void *args)
{
    const struct handshake_params *params = args;
    struct tls_session *session = session_from_handle(params->session);
    int error;

    pthread_mutex_lock(&session->lock);
    init_buffers(&session->input, params->input, params->input_size);
    init_buffers(&session->output, params->output, SIZE_MAX);
    if (params->control_token == CONTROL_TOKEN_SHUTDOWN)
        error = mbedtls_ssl_close_notify(&session->ssl);
    else if (params->control_token == CONTROL_TOKEN_ALERT)
        error = mbedtls_ssl_send_alert_message(&session->ssl, params->alert_type, params->alert_number);
    else
    {
        if (session->credentials->cert.raw.p) pthread_mutex_lock(&session->credentials->key_lock);
        error = mbedtls_ssl_handshake(&session->ssl);
        if (session->credentials->cert.raw.p) pthread_mutex_unlock(&session->credentials->key_lock);
    }
    *params->input_offset = session->input.used;
    output_position(session, params->output_buffer_idx, params->output_offset);
    session->input.desc = session->output.desc = NULL;
    pthread_mutex_unlock(&session->lock);
    return tls_status(error);
}

static const mbedtls_ssl_ciphersuite_t *get_suite(struct tls_session *session)
{
    const char *name = mbedtls_ssl_get_ciphersuite(&session->ssl);
    return name ? mbedtls_ssl_ciphersuite_from_id(mbedtls_ssl_get_ciphersuite_id(name)) : NULL;
}

static NTSTATUS schan_get_max_message_size(void *args)
{
    const struct session_params *params = args;
    int size = mbedtls_ssl_get_max_out_record_payload(&session_from_handle(params->session)->ssl);
    return size > 0 ? size : 0;
}

static NTSTATUS schan_get_session_cipher_block_size(void *args)
{
    const struct session_params *params = args;
    const mbedtls_ssl_ciphersuite_t *suite = get_suite(session_from_handle(params->session));
    const mbedtls_cipher_info_t *cipher;

    if (!suite || !(cipher = mbedtls_cipher_info_from_type(suite->cipher))) return 0;
    return cipher->block_size;
}

static ALG_ID signature_algorithm(const mbedtls_ssl_ciphersuite_t *suite)
{
    if (!suite) return 0;
    return suite->key_exchange == MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA ||
           suite->key_exchange == MBEDTLS_KEY_EXCHANGE_ECDH_ECDSA ? CALG_ECDSA : CALG_RSA_SIGN;
}

static NTSTATUS schan_get_key_signature_algorithm(void *args)
{
    const struct session_params *params = args;
    return signature_algorithm(get_suite(session_from_handle(params->session)));
}

static NTSTATUS schan_get_connection_info(void *args)
{
    const struct get_connection_info_params *params = args;
    struct tls_session *session = session_from_handle(params->session);
    const mbedtls_ssl_ciphersuite_t *suite = get_suite(session);
    const mbedtls_cipher_info_t *cipher;
    const mbedtls_md_info_t *hash;
    const mbedtls_x509_crt *cert;
    SecPkgContext_ConnectionInfo *info = params->info;

    if (!suite || !(cipher = mbedtls_cipher_info_from_type(suite->cipher))) return SEC_E_INVALID_HANDLE;
    memset(info, 0, sizeof(*info));
    switch (session->ssl.minor_ver)
    {
    case 1: info->dwProtocol = session->server ? SP_PROT_TLS1_0_SERVER : SP_PROT_TLS1_0_CLIENT; break;
    case 2: info->dwProtocol = session->server ? SP_PROT_TLS1_1_SERVER : SP_PROT_TLS1_1_CLIENT; break;
    case 3: info->dwProtocol = session->server ? SP_PROT_TLS1_2_SERVER : SP_PROT_TLS1_2_CLIENT; break;
    default: return SEC_E_INTERNAL_ERROR;
    }
    switch (cipher->type)
    {
    case MBEDTLS_CIPHER_AES_128_CBC: case MBEDTLS_CIPHER_AES_128_GCM: case MBEDTLS_CIPHER_AES_128_CCM:
        info->aiCipher = CALG_AES_128; break;
    case MBEDTLS_CIPHER_AES_256_CBC: case MBEDTLS_CIPHER_AES_256_GCM: case MBEDTLS_CIPHER_AES_256_CCM:
        info->aiCipher = CALG_AES_256; break;
    case MBEDTLS_CIPHER_DES_EDE3_CBC: info->aiCipher = CALG_3DES; break;
    default: break;
    }
    info->dwCipherStrength = cipher->key_bitlen;
    switch (suite->mac)
    {
    case MBEDTLS_MD_SHA1: info->aiHash = CALG_SHA1; break;
    case MBEDTLS_MD_SHA256: info->aiHash = CALG_SHA_256; break;
    case MBEDTLS_MD_SHA384: info->aiHash = CALG_SHA_384; break;
    default: break;
    }
    if ((hash = mbedtls_md_info_from_type(suite->mac)))
        info->dwHashStrength = mbedtls_md_get_size(hash) * 8;
    if (suite->key_exchange == MBEDTLS_KEY_EXCHANGE_RSA) info->aiExch = CALG_RSA_KEYX;
    else if (suite->key_exchange == MBEDTLS_KEY_EXCHANGE_DHE_RSA) info->aiExch = CALG_DH_EPHEM;
    else if (mbedtls_ssl_ciphersuite_uses_ecdhe(suite)) info->aiExch = CALG_ECDH_EPHEM;
    else if (mbedtls_ssl_ciphersuite_uses_ecdh(suite)) info->aiExch = CALG_ECDH;
    cert = mbedtls_ssl_get_peer_cert(&session->ssl);
    if (cert && suite->key_exchange == MBEDTLS_KEY_EXCHANGE_RSA) info->dwExchStrength = mbedtls_pk_get_bitlen(&cert->pk);
    return SEC_E_OK;
}

static void widen(WCHAR *output, size_t capacity, const char *input)
{
    size_t i;
    for (i = 0; i + 1 < capacity && input[i]; ++i) output[i] = (unsigned char)input[i];
    output[i] = 0;
}

static NTSTATUS schan_get_cipher_info(void *args)
{
    const struct get_cipher_info_params *params = args;
    struct tls_session *session = session_from_handle(params->session);
    const mbedtls_ssl_ciphersuite_t *suite = get_suite(session);
    const mbedtls_cipher_info_t *cipher;
    const mbedtls_md_info_t *hash;
    SecPkgContext_CipherInfo *info = params->info;
    const char *exchange;

    if (!suite || !(cipher = mbedtls_cipher_info_from_type(suite->cipher))) return SEC_E_INVALID_HANDLE;
    memset(info, 0, sizeof(*info));
    info->dwVersion = SECPKGCONTEXT_CIPHERINFO_V1;
    info->dwProtocol = 0x300 | session->ssl.minor_ver;
    info->dwCipherSuite = info->dwBaseCipherSuite = suite->id;
    widen(info->szCipherSuite, ARRAY_SIZE(info->szCipherSuite), suite->name);
    for (unsigned int i = 0; info->szCipherSuite[i]; ++i)
        if (info->szCipherSuite[i] == '-') info->szCipherSuite[i] = '_';
    widen(info->szCipher, ARRAY_SIZE(info->szCipher), !strncmp(cipher->name, "AES", 3) ? "AES" : cipher->name);
    info->dwCipherLen = cipher->key_bitlen;
    info->dwCipherBlockLen = cipher->block_size * 8;
    if ((hash = mbedtls_md_info_from_type(suite->mac)))
    {
        widen(info->szHash, ARRAY_SIZE(info->szHash), mbedtls_md_get_name(hash));
        info->dwHashLen = mbedtls_md_get_size(hash) * 8;
    }
    exchange = mbedtls_ssl_ciphersuite_uses_ecdhe(suite) ? "ECDH" :
               suite->key_exchange == MBEDTLS_KEY_EXCHANGE_DHE_RSA ? "DH" : "RSA";
    widen(info->szExchange, ARRAY_SIZE(info->szExchange), exchange);
    widen(info->szCertificate, ARRAY_SIZE(info->szCertificate), signature_algorithm(suite) == CALG_ECDSA ? "ECDSA" : "RSA");
    return SEC_E_OK;
}

static NTSTATUS schan_get_session_peer_certificate(void *args)
{
    const struct get_session_peer_certificate_params *params = args;
    const mbedtls_x509_crt *cert, *chain = mbedtls_ssl_get_peer_cert(&session_from_handle(params->session)->ssl);
    size_t size = 0;
    ULONG count = 0, *sizes;
    BYTE *data;

    if (!chain) return SEC_E_NO_CREDENTIALS;
    for (cert = chain; cert; cert = cert->next)
    {
        if (size > UINT32_MAX - sizeof(ULONG) || cert->raw.len > UINT32_MAX - sizeof(ULONG) - size)
            return SEC_E_INSUFFICIENT_MEMORY;
        size += sizeof(ULONG) + cert->raw.len;
        ++count;
    }
    *params->retcount = count;
    if (!params->buffer || *params->bufsize < size)
    {
        *params->bufsize = size;
        return SEC_E_BUFFER_TOO_SMALL;
    }
    sizes = (ULONG *)params->buffer;
    data = params->buffer + count * sizeof(*sizes);
    for (cert = chain; cert; cert = cert->next)
    {
        *sizes++ = cert->raw.len;
        memcpy(data, cert->raw.p, cert->raw.len);
        data += cert->raw.len;
    }
    *params->bufsize = size;
    return SEC_E_OK;
}

static NTSTATUS schan_get_unique_channel_binding(void *args)
{
    const struct get_unique_channel_binding_params *params = args;
    struct tls_session *session = session_from_handle(params->session);
    size_t size = session->ssl.verify_data_len;

    if (!size) return SEC_E_UNSUPPORTED_FUNCTION;
    if (!params->buffer || *params->bufsize < size)
    {
        *params->bufsize = size;
        return SEC_E_BUFFER_TOO_SMALL;
    }
    memcpy(params->buffer, session->server ? session->ssl.peer_verify_data : session->ssl.own_verify_data, size);
    *params->bufsize = size;
    return SEC_E_OK;
}

static NTSTATUS schan_send(void *args)
{
    const struct send_params *params = args;
    struct tls_session *session = session_from_handle(params->session);
    size_t capacity = 0;
    unsigned int i;
    NTSTATUS status;
    int written, expansion;

    pthread_mutex_lock(&session->lock);
    expansion = mbedtls_ssl_get_record_expansion(&session->ssl);
    *params->output_buffer_idx = -1;
    *params->output_offset = 0;
    if (params->length > (ULONG)schan_get_max_message_size(&(struct session_params){ params->session }))
    {
        status = SEC_E_BUFFER_TOO_SMALL;
        goto done;
    }
    if (params->output)
        for (i = 0; i < params->output->cBuffers; ++i) capacity += params->output->pBuffers[i].cbBuffer;
    if (expansion < 0)
    {
        status = tls_status(expansion);
        goto done;
    }
    if (capacity < (size_t)params->length + expansion)
    {
        status = SEC_E_BUFFER_TOO_SMALL;
        goto done;
    }
    init_buffers(&session->input, NULL, 0);
    init_buffers(&session->output, params->output, SIZE_MAX);
    written = mbedtls_ssl_write(&session->ssl, params->buffer, params->length);
    output_position(session, params->output_buffer_idx, params->output_offset);
    session->output.desc = NULL;
    status = written < 0 ? tls_status(written) : (ULONG)written == params->length ? SEC_E_OK : SEC_E_BUFFER_TOO_SMALL;
done:
    pthread_mutex_unlock(&session->lock);
    return status;
}

static NTSTATUS schan_recv(void *args)
{
    const struct recv_params *params = args;
    struct tls_session *session = session_from_handle(params->session);
    int received;

    if (!*params->length) return SEC_E_OK;
    pthread_mutex_lock(&session->lock);
    init_buffers(&session->input, params->input, params->input_size);
    init_buffers(&session->output, NULL, 0);
    received = mbedtls_ssl_read(&session->ssl, params->buffer, *params->length);
    *params->length = received > 0 ? received : 0;
    session->input.desc = NULL;
    pthread_mutex_unlock(&session->lock);
    if (received == MBEDTLS_ERR_SSL_WANT_READ) return SEC_E_OK;
    if (!received) return SEC_I_CONTEXT_EXPIRED;
    return received < 0 ? tls_status(received) : SEC_E_OK;
}

static NTSTATUS schan_set_application_protocols(void *args)
{
    const struct set_application_protocols_params *params = args;
    struct tls_session *session = session_from_handle(params->session);
    unsigned int extension_size, extension, count = 0, i;
    unsigned short size;
    const BYTE *data, *end;
    char **protocols;
    int error;

    if (params->buflen < 10 || !params->buffer) return STATUS_INVALID_PARAMETER;
    if (session->alpn) return STATUS_INVALID_PARAMETER;
    memcpy(&extension_size, params->buffer, 4);
    memcpy(&extension, params->buffer + 4, 4);
    memcpy(&size, params->buffer + 8, 2);
    if (extension_size != (unsigned int)size + 6 || extension_size > params->buflen - 4)
        return STATUS_INVALID_PARAMETER;
    if (extension != SecApplicationProtocolNegotiationExt_ALPN) return STATUS_NOT_SUPPORTED;
    data = params->buffer + 10;
    end = data + size;
    while (data < end)
    {
        unsigned int length = *data++;
        if (!length || length > (size_t)(end - data) || memchr(data, 0, length)) return STATUS_INVALID_PARAMETER;
        ++count;
        data += length;
    }
    if (!count) return STATUS_INVALID_PARAMETER;
    if (!(protocols = calloc(count + 1, sizeof(*protocols)))) return STATUS_NO_MEMORY;
    data = params->buffer + 10;
    for (i = 0; i < count; ++i)
    {
        unsigned int length = *data++;
        if (!(protocols[i] = malloc(length + 1))) break;
        memcpy(protocols[i], data, length);
        protocols[i][length] = 0;
        data += length;
    }
    if (i != count) error = MBEDTLS_ERR_SSL_ALLOC_FAILED;
    else error = mbedtls_ssl_conf_alpn_protocols(&session->config, (const char **)protocols);
    if (error)
    {
        for (i = 0; protocols[i]; ++i) free(protocols[i]);
        free(protocols);
        return tls_status(error);
    }
    session->alpn = protocols;
    return STATUS_SUCCESS;
}

static NTSTATUS schan_get_application_protocol(void *args)
{
    const struct get_application_protocol_params *params = args;
    const char *protocol = mbedtls_ssl_get_alpn_protocol(&session_from_handle(params->session)->ssl);

    memset(params->protocol, 0, sizeof(*params->protocol));
    if (protocol)
    {
        size_t length = strlen(protocol);
        if (length > sizeof(params->protocol->ProtocolId)) return SEC_E_INTERNAL_ERROR;
        params->protocol->ProtoNegoStatus = SecApplicationProtocolNegotiationStatus_Success;
        params->protocol->ProtoNegoExt = SecApplicationProtocolNegotiationExt_ALPN;
        params->protocol->ProtocolIdSize = length;
        memcpy(params->protocol->ProtocolId, protocol, length);
    }
    return SEC_E_OK;
}

static NTSTATUS schan_set_dtls_mtu(void *args) { (void)args; return STATUS_NOT_SUPPORTED; }
static NTSTATUS schan_set_dtls_timeouts(void *args) { (void)args; return STATUS_NOT_SUPPORTED; }

const unixlib_entry_t wine_nx_secur32_unix_funcs[] =
{
    process_attach, process_detach, schan_allocate_certificate_credentials,
    schan_create_session, schan_dispose_session, schan_free_certificate_credentials,
    schan_get_application_protocol, schan_get_cipher_info, schan_get_connection_info,
    schan_get_enabled_protocols, schan_get_key_signature_algorithm, schan_get_max_message_size,
    schan_get_session_cipher_block_size, schan_get_session_peer_certificate,
    schan_get_unique_channel_binding, schan_handshake, schan_recv, schan_send,
    schan_set_application_protocols, schan_set_dtls_mtu, schan_set_session_target, schan_set_dtls_timeouts,
};

C_ASSERT(ARRAY_SIZE(wine_nx_secur32_unix_funcs) == unix_funcs_count);

struct secbuffer32 { ULONG size, type, data; };
struct secbufferdesc32 { ULONG version, count, buffers; };

static BOOL convert_buffers(ULONG ptr, SecBufferDesc *desc, SecBuffer *buffers, unsigned int capacity)
{
    const struct secbufferdesc32 *source = ULongToPtr(ptr);
    const struct secbuffer32 *buffer;
    unsigned int i;

    memset(desc, 0, sizeof(*desc));
    desc->pBuffers = buffers;
    if (!source) return TRUE;
    if (source->count > capacity || (source->count && !source->buffers)) return FALSE;
    desc->ulVersion = source->version;
    desc->cBuffers = source->count;
    buffer = ULongToPtr(source->buffers);
    for (i = 0; i < source->count; ++i)
        buffers[i] = (SecBuffer){ buffer[i].size, buffer[i].type, ULongToPtr(buffer[i].data) };
    return TRUE;
}

static NTSTATUS wow64_allocate_credentials(void *args)
{
    const struct { ULONG cred, encoding, cert_size, cert, key_size, key; } *p = args;
    struct allocate_certificate_credentials_params params =
        { ULongToPtr(p->cred), p->encoding, p->cert_size, ULongToPtr(p->cert), p->key_size, ULongToPtr(p->key) };
    return schan_allocate_certificate_credentials(&params);
}

static NTSTATUS wow64_create_session(void *args)
{
    const struct { ULONG cred, session; } *p = args;
    struct create_session_params params = { ULongToPtr(p->cred), ULongToPtr(p->session) };
    return schan_create_session(&params);
}

static NTSTATUS wow64_free_credentials(void *args)
{
    const ULONG *p = args;
    struct free_certificate_credentials_params params = { ULongToPtr(*p) };
    return schan_free_certificate_credentials(&params);
}

#define WOW64_QUERY(name, field, function) \
static NTSTATUS wow64_##name(void *args) \
{ \
    const struct { schan_session session; ULONG field; } *p = args; \
    struct name##_params params = { p->session, ULongToPtr(p->field) }; \
    return function(&params); \
}

WOW64_QUERY(get_application_protocol, protocol, schan_get_application_protocol)
WOW64_QUERY(get_connection_info, info, schan_get_connection_info)
WOW64_QUERY(get_cipher_info, info, schan_get_cipher_info)
WOW64_QUERY(set_session_target, target, schan_set_session_target)
#undef WOW64_QUERY

static NTSTATUS wow64_get_peer_certificate(void *args)
{
    const struct { schan_session session; ULONG buffer, size, count; } *p = args;
    struct get_session_peer_certificate_params params =
        { p->session, ULongToPtr(p->buffer), ULongToPtr(p->size), ULongToPtr(p->count) };
    return schan_get_session_peer_certificate(&params);
}

static NTSTATUS wow64_get_unique_binding(void *args)
{
    const struct { schan_session session; ULONG buffer, size; } *p = args;
    struct get_unique_channel_binding_params params =
        { p->session, ULongToPtr(p->buffer), ULongToPtr(p->size) };
    return schan_get_unique_channel_binding(&params);
}

static NTSTATUS wow64_handshake(void *args)
{
    const struct
    {
        schan_session session;
        ULONG input, input_size, output, input_offset, output_index, output_offset;
        enum control_token control;
        unsigned int alert_type, alert_number;
    } *p = args;
    SecBuffer input_buffers[3], output_buffers[3];
    SecBufferDesc input, output;
    struct handshake_params params =
    {
        p->session, p->input ? &input : NULL, p->input_size, p->output ? &output : NULL,
        ULongToPtr(p->input_offset), ULongToPtr(p->output_index), ULongToPtr(p->output_offset),
        p->control, p->alert_type, p->alert_number,
    };
    if (!convert_buffers(p->input, &input, input_buffers, ARRAY_SIZE(input_buffers)) ||
        !convert_buffers(p->output, &output, output_buffers, ARRAY_SIZE(output_buffers)))
        return STATUS_INVALID_PARAMETER;
    return schan_handshake(&params);
}

static NTSTATUS wow64_recv(void *args)
{
    const struct { schan_session session; ULONG input, input_size, buffer, length; } *p = args;
    SecBuffer buffers[3];
    SecBufferDesc input;
    struct recv_params params =
        { p->session, p->input ? &input : NULL, p->input_size, ULongToPtr(p->buffer), ULongToPtr(p->length) };
    if (!convert_buffers(p->input, &input, buffers, ARRAY_SIZE(buffers))) return STATUS_INVALID_PARAMETER;
    return schan_recv(&params);
}

static NTSTATUS wow64_send(void *args)
{
    const struct { schan_session session; ULONG output, buffer, length, output_index, output_offset; } *p = args;
    SecBuffer buffers[3];
    SecBufferDesc output;
    struct send_params params =
    {
        p->session, p->output ? &output : NULL, ULongToPtr(p->buffer), p->length,
        ULongToPtr(p->output_index), ULongToPtr(p->output_offset),
    };
    if (!convert_buffers(p->output, &output, buffers, ARRAY_SIZE(buffers))) return STATUS_INVALID_PARAMETER;
    return schan_send(&params);
}

static NTSTATUS wow64_set_application_protocols(void *args)
{
    const struct { schan_session session; ULONG buffer, size; } *p = args;
    struct set_application_protocols_params params = { p->session, ULongToPtr(p->buffer), p->size };
    return schan_set_application_protocols(&params);
}

const unixlib_entry_t wine_nx_secur32_wow64_unix_funcs[] =
{
    process_attach, process_detach, wow64_allocate_credentials,
    wow64_create_session, schan_dispose_session, wow64_free_credentials,
    wow64_get_application_protocol, wow64_get_cipher_info, wow64_get_connection_info,
    schan_get_enabled_protocols, schan_get_key_signature_algorithm, schan_get_max_message_size,
    schan_get_session_cipher_block_size, wow64_get_peer_certificate,
    wow64_get_unique_binding, wow64_handshake, wow64_recv, wow64_send,
    wow64_set_application_protocols, schan_set_dtls_mtu, wow64_set_session_target, schan_set_dtls_timeouts,
};

C_ASSERT(ARRAY_SIZE(wine_nx_secur32_wow64_unix_funcs) == unix_funcs_count);
const unsigned int wine_nx_secur32_wow64_unix_count = ARRAY_SIZE(wine_nx_secur32_wow64_unix_funcs);
