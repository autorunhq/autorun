#include <assert.h>
#include <stdio.h>
#include <mbedtls/certs.h>
#include <mbedtls/net_sockets.h>
#include "../source/schannel_mbedtls.c"

struct wire { unsigned char bytes[65536]; unsigned int size; };

static void consume(struct wire *wire, size_t size)
{
    assert(size <= wire->size);
    wire->size -= size;
    memmove(wire->bytes, wire->bytes + size, wire->size);
}

static NTSTATUS step(schan_session session, struct wire *in, struct wire *out, unsigned int chunk)
{
    SecBuffer ib = { in->size > 19 ? 19 : in->size, SECBUFFER_TOKEN, in->bytes };
    SecBuffer ob = { chunk, SECBUFFER_TOKEN, out->bytes + out->size };
    SecBufferDesc input = { 0, 1, &ib }, output = { 0, 1, &ob };
    ULONG consumed, written;
    int index;
    struct handshake_params params = { session, &input, ib.cbBuffer, &output, &consumed, &index, &written };
    NTSTATUS status = schan_handshake(&params);
    assert(status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED || status == SEC_E_BUFFER_TOO_SMALL);
    consume(in, consumed);
    if (index >= 0) out->size += written;
    assert(!session_from_handle(session)->input.desc && !session_from_handle(session)->output.desc);
    return status;
}

static BYTE *rsa_blob(const mbedtls_rsa_context *rsa, size_t *size)
{
    mbedtls_mpi n, p, q, d, e;
    BCRYPT_RSAKEY_BLOB *blob;
    BYTE *ptr;

    mbedtls_mpi_init(&n); mbedtls_mpi_init(&p); mbedtls_mpi_init(&q);
    mbedtls_mpi_init(&d); mbedtls_mpi_init(&e);
    assert(!mbedtls_rsa_export(rsa, &n, &p, &q, &d, &e));
    *size = sizeof(*blob) + mbedtls_mpi_size(&e) + 2 * mbedtls_mpi_size(&n) +
            3 * mbedtls_mpi_size(&p) + 2 * mbedtls_mpi_size(&q);
    assert((blob = calloc(1, *size)));
    *blob = (BCRYPT_RSAKEY_BLOB){ BCRYPT_RSAFULLPRIVATE_MAGIC, mbedtls_mpi_bitlen(&n),
        mbedtls_mpi_size(&e), mbedtls_mpi_size(&n), mbedtls_mpi_size(&p), mbedtls_mpi_size(&q) };
    ptr = (BYTE *)(blob + 1);
    assert(!mbedtls_mpi_write_binary(&e, ptr, blob->cbPublicExp)); ptr += blob->cbPublicExp;
    assert(!mbedtls_mpi_write_binary(&n, ptr, blob->cbModulus)); ptr += blob->cbModulus;
    assert(!mbedtls_mpi_write_binary(&p, ptr, blob->cbPrime1)); ptr += blob->cbPrime1;
    assert(!mbedtls_mpi_write_binary(&q, ptr, blob->cbPrime2));
    ptr += 2 * blob->cbPrime2 + 2 * blob->cbPrime1;
    assert(!mbedtls_mpi_write_binary(&d, ptr, blob->cbModulus));
    mbedtls_mpi_free(&n); mbedtls_mpi_free(&p); mbedtls_mpi_free(&q);
    mbedtls_mpi_free(&d); mbedtls_mpi_free(&e);
    return (BYTE *)blob;
}

static void transfer_message(schan_session from, schan_session to, BOOL corrupt)
{
    unsigned char data[16384], encrypted[17000], decrypted[17000];
    ULONG length = sizeof(decrypted), offset;
    unsigned int i;
    int index;
    SecBuffer buffers[] =
    {
        { 5, SECBUFFER_STREAM_HEADER, encrypted },
        { sizeof(data), SECBUFFER_DATA, encrypted + 5 },
        { sizeof(encrypted) - 5 - sizeof(data), SECBUFFER_STREAM_TRAILER, encrypted + 5 + sizeof(data) },
    };
    SecBufferDesc output = { 0, ARRAY_SIZE(buffers), buffers };
    struct send_params send = { from, &output, data, sizeof(data), &index, &offset };
    struct recv_params recv = { to, &output, 0, decrypted, &length };

    for (i = 0; i < sizeof(data); ++i) data[i] = i;
    buffers[2].cbBuffer = 0;
    assert(schan_send(&send) == SEC_E_BUFFER_TOO_SMALL && index == -1 && !offset);
    buffers[2].cbBuffer = sizeof(encrypted) - 5 - sizeof(data);
    assert(schan_send(&send) == SEC_E_OK);
    assert(index == 2 && offset < buffers[2].cbBuffer);
    recv.input_size = 5 + sizeof(data) + offset;
    if (corrupt) encrypted[50] ^= 1;
    if (corrupt) assert(schan_recv(&recv) == SEC_E_MESSAGE_ALTERED);
    else
    {
        assert(schan_recv(&recv) == SEC_E_OK && length == sizeof(data));
        assert(!memcmp(data, decrypted, sizeof(data)));
    }
}

static void test_session(unsigned int chunk, int cipher, unsigned int version)
{
    static const DWORD clients[] = { SP_PROT_TLS1_0_CLIENT, SP_PROT_TLS1_1_CLIENT, SP_PROT_TLS1_2_CLIENT };
    static const DWORD servers[] = { SP_PROT_TLS1_0_SERVER, SP_PROT_TLS1_1_SERVER, SP_PROT_TLS1_2_SERVER };
    schan_credentials client = { SECPKG_CRED_OUTBOUND, clients[version] };
    schan_credentials server = { SECPKG_CRED_INBOUND, servers[version] };
    schan_session cs, ss;
    struct allocate_certificate_credentials_params ccred = { .c = &client }, scred = { .c = &server };
    struct create_session_params csession = { &client, &cs }, ssession = { &server, &ss };
    mbedtls_x509_crt cert, ca;
    mbedtls_pk_context key;
    size_t blob_size;
    struct wire to_client = { 0 }, to_server = { 0 };
    BOOL client_done = FALSE, server_done = FALSE;
    NTSTATUS status;
    unsigned int i;
    uint32_t flags;
    ULONG cert_size = 0, count = 0;
    BYTE *encoded;
    struct get_session_peer_certificate_params peer;
    SecPkgContext_ConnectionInfo info;
    struct get_connection_info_params connection;
    unsigned char alpn[] = { 18,0,0,0, 2,0,0,0, 12,0, 2,'h','2', 8,'h','t','t','p','/','1','.','1' };
    struct set_application_protocols_params app;
    SecPkgContext_ApplicationProtocol selected;
    struct get_application_protocol_params query;

    mbedtls_x509_crt_init(&cert); mbedtls_x509_crt_init(&ca); mbedtls_pk_init(&key);
    assert(!mbedtls_x509_crt_parse(&cert, (const BYTE *)mbedtls_test_srv_crt, mbedtls_test_srv_crt_len));
    assert(!mbedtls_x509_crt_parse(&ca, (const BYTE *)mbedtls_test_ca_crt, mbedtls_test_ca_crt_len));
    assert(!mbedtls_pk_parse_key(&key, (const BYTE *)mbedtls_test_srv_key, mbedtls_test_srv_key_len, NULL, 0));
    scred.cert_encoding = X509_ASN_ENCODING;
    scred.cert_blob = cert.raw.p; scred.cert_size = cert.raw.len;
    scred.key_blob = rsa_blob(mbedtls_pk_rsa(key), &blob_size); scred.key_size = blob_size;
    assert(schan_allocate_certificate_credentials(&ccred) == STATUS_SUCCESS);
    assert(schan_allocate_certificate_credentials(&scred) == STATUS_SUCCESS);
    assert(schan_create_session(&csession) == STATUS_SUCCESS);
    assert(schan_create_session(&ssession) == STATUS_SUCCESS);
    free(scred.key_blob);
    assert(schan_free_certificate_credentials(&(struct free_certificate_credentials_params){ &client }) == STATUS_SUCCESS);
    assert(schan_free_certificate_credentials(&(struct free_certificate_credentials_params){ &server }) == STATUS_SUCCESS);
    assert(!schan_set_session_target(&(struct set_session_target_params){ cs, "localhost" }));
    if (cipher)
    {
        static int suites[2];
        suites[0] = cipher;
        mbedtls_ssl_conf_ciphersuites(&session_from_handle(cs)->config, suites);
        mbedtls_ssl_conf_ciphersuites(&session_from_handle(ss)->config, suites);
    }
    app = (struct set_application_protocols_params){ cs, alpn, sizeof(alpn) };
    alpn[10] = 0;
    assert(schan_set_application_protocols(&app) == STATUS_INVALID_PARAMETER);
    alpn[10] = 2;
    assert(!schan_set_application_protocols(&app));
    app.session = ss;
    assert(!schan_set_application_protocols(&app));
    for (i = 0; i < 20000 && !(client_done && server_done); ++i)
    {
        if (!client_done) { status = step(cs, &to_client, &to_server, chunk); client_done = status == SEC_E_OK; }
        if (!server_done) { status = step(ss, &to_server, &to_client, chunk); server_done = status == SEC_E_OK; }
    }
    if (!(client_done && server_done))
        fprintf(stderr, "chunk=%u states=%d/%d queues=%u/%u\n", chunk,
            session_from_handle(cs)->ssl.state, session_from_handle(ss)->ssl.state, to_client.size, to_server.size);
    assert(client_done && server_done);
    connection = (struct get_connection_info_params){ cs, &info };
    assert(!schan_get_connection_info(&connection) && info.dwProtocol == clients[version]);
    assert(info.dwCipherStrength >= 128 && info.aiCipher);
    query = (struct get_application_protocol_params){ cs, &selected };
    assert(!schan_get_application_protocol(&query));
    assert(selected.ProtocolIdSize == 2 && !memcmp(selected.ProtocolId, "h2", 2));
    peer = (struct get_session_peer_certificate_params){ cs, NULL, &cert_size, &count };
    assert(schan_get_session_peer_certificate(&peer) == SEC_E_BUFFER_TOO_SMALL && count);
    assert((encoded = malloc(cert_size)));
    peer.buffer = encoded;
    assert(!schan_get_session_peer_certificate(&peer));
    assert(!memcmp(encoded + count * sizeof(ULONG), cert.raw.p, ((ULONG *)encoded)[0]));
    assert(!mbedtls_x509_crt_verify((mbedtls_x509_crt *)mbedtls_ssl_get_peer_cert(&session_from_handle(cs)->ssl), &ca,
                                  NULL, "localhost", &flags, NULL, NULL));
    assert(mbedtls_x509_crt_verify((mbedtls_x509_crt *)mbedtls_ssl_get_peer_cert(&session_from_handle(cs)->ssl), &ca,
                                 NULL, "wrong.example", &flags, NULL, NULL));
    assert(flags & MBEDTLS_X509_BADCERT_CN_MISMATCH);
    free(encoded);
    {
        unsigned char cb1[36], cb2[36];
        ULONG sz1 = sizeof(cb1), sz2 = sizeof(cb2);
        struct get_unique_channel_binding_params b1 = { cs, cb1, &sz1 }, b2 = { ss, cb2, &sz2 };
        assert(!schan_get_unique_channel_binding(&b1) && !schan_get_unique_channel_binding(&b2));
        assert(sz1 == sz2 && sz1 == 12 && !memcmp(cb1, cb2, sz1));
    }
    transfer_message(cs, ss, FALSE);
    transfer_message(ss, cs, FALSE);
    {
        unsigned char record[256], data[16];
        ULONG consumed, written, length = sizeof(data);
        int index;
        SecBuffer buffer = { sizeof(record), SECBUFFER_TOKEN, record };
        SecBufferDesc output = { 0, 1, &buffer };
        struct handshake_params close = { ss, NULL, 0, &output, &consumed, &index, &written, CONTROL_TOKEN_SHUTDOWN };
        struct recv_params recv = { cs, &output, 0, data, &length };
        assert(!schan_handshake(&close) && index == 0 && written);
        recv.input_size = written;
        assert(schan_recv(&recv) == SEC_I_CONTEXT_EXPIRED && !length);
    }
    transfer_message(cs, ss, TRUE);
    assert(!schan_dispose_session(&(struct session_params){ cs }));
    assert(!schan_dispose_session(&(struct session_params){ ss }));
    mbedtls_x509_crt_free(&cert); mbedtls_x509_crt_free(&ca); mbedtls_pk_free(&key);
}

static void send_wire(mbedtls_net_context *net, struct wire *wire)
{
    while (wire->size)
    {
        int sent = mbedtls_net_send(net, wire->bytes, wire->size);
        assert(sent > 0);
        consume(wire, sent);
    }
}

static void test_https(const char *host, const char *ca_file)
{
    schan_credentials client = { SECPKG_CRED_OUTBOUND, SP_PROT_TLS1_2_CLIENT };
    schan_session session;
    struct allocate_certificate_credentials_params cred = { .c = &client };
    struct create_session_params create = { &client, &session };
    mbedtls_net_context net;
    mbedtls_x509_crt ca;
    struct wire in = { 0 }, out = { 0 };
    NTSTATUS status;
    uint32_t flags;
    char request[1024], reply[4096];
    ULONG consumed, written, length;
    int index, received;
    SecBuffer ib = { 0, SECBUFFER_TOKEN, in.bytes }, ob = { sizeof(out.bytes), SECBUFFER_TOKEN, out.bytes };
    SecBufferDesc input = { 0, 1, &ib }, output = { 0, 1, &ob };
    struct handshake_params handshake = { 0, &input, 0, &output, &consumed, &index, &written };
    struct send_params send = { 0, &output, request, 0, &index, &written };
    struct recv_params recv = { 0, &input, 0, reply, &length };

    mbedtls_net_init(&net);
    mbedtls_x509_crt_init(&ca);
    assert(!mbedtls_x509_crt_parse_file(&ca, ca_file));
    assert(!mbedtls_net_connect(&net, host, "443", MBEDTLS_NET_PROTO_TCP));
    assert(!schan_allocate_certificate_credentials(&cred));
    assert(!schan_create_session(&create));
    assert(!schan_set_session_target(&(struct set_session_target_params){ session, host }));
    handshake.session = send.session = recv.session = session;
    do
    {
        ib.cbBuffer = handshake.input_size = in.size;
        status = schan_handshake(&handshake);
        assert(status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED);
        consume(&in, consumed);
        out.size = index < 0 ? 0 : written;
        send_wire(&net, &out);
        if (status == SEC_I_CONTINUE_NEEDED)
        {
            received = mbedtls_net_recv_timeout(&net, in.bytes + in.size, sizeof(in.bytes) - in.size, 10000);
            assert(received > 0);
            in.size += received;
        }
    } while (status != SEC_E_OK);
    assert(!mbedtls_x509_crt_verify((mbedtls_x509_crt *)mbedtls_ssl_get_peer_cert(&session_from_handle(session)->ssl),
                                  &ca, NULL, host, &flags, NULL, NULL));
    send.length = snprintf(request, sizeof(request), "GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    assert(!schan_send(&send) && index == 0);
    out.size = written;
    send_wire(&net, &out);
    do
    {
        if (!in.size)
        {
            received = mbedtls_net_recv_timeout(&net, in.bytes, sizeof(in.bytes), 10000);
            assert(received > 0);
            in.size = received;
        }
        ib.cbBuffer = recv.input_size = in.size;
        length = sizeof(reply) - 1;
        assert(!schan_recv(&recv));
        consume(&in, session_from_handle(session)->input.used);
    } while (!length);
    reply[length] = 0;
    assert(!strncmp(reply, "HTTP/1.1 ", 9));
    printf("HTTPS %s: TLS 1.2, trusted certificate/hostname and %.12s passed\n", host, reply);
    assert(!schan_dispose_session(&(struct session_params){ session }));
    assert(!schan_free_certificate_credentials(&(struct free_certificate_credentials_params){ &client }));
    mbedtls_x509_crt_free(&ca);
    mbedtls_net_free(&net);
}

static void test_wow64(void)
{
    struct low_memory
    {
        schan_credentials cred;
        schan_session session;
        struct secbufferdesc32 desc;
        struct secbuffer32 buffer;
        ULONG consumed, written;
        int index;
        unsigned char data[4096];
        char host[32];
    } *low = VirtualAlloc((void *)0x30000000, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    struct { ULONG cred, encoding, cert_size, cert, key_size, key; } allocate;
    struct { ULONG cred, session; } create;
    struct { schan_session session; ULONG target; } target;
    struct
    {
        schan_session session;
        ULONG input, input_size, output, consumed, index, written;
        enum control_token control;
        unsigned int alert_type, alert_number;
    } handshake = { 0 };
    SecPkgContext_ConnectionInfo info;

    assert(low && (uintptr_t)low + sizeof(*low) < UINT32_MAX);
    low->cred = (schan_credentials){ SECPKG_CRED_OUTBOUND, SP_PROT_TLS1_2_CLIENT };
    allocate = (typeof(allocate)){ PtrToUlong(&low->cred), 0, 0, 0, 0, 0 };
    assert(!wow64_allocate_credentials(&allocate));
    create = (typeof(create)){ PtrToUlong(&low->cred), PtrToUlong(&low->session) };
    low->cred.enabled_protocols = SP_PROT_DTLS1_2_CLIENT;
    assert(wow64_create_session(&create) == STATUS_NOT_SUPPORTED && !low->session);
    low->cred.enabled_protocols = SP_PROT_TLS1_2_CLIENT;
    assert(!wow64_create_session(&create));
    assert(schan_get_connection_info(&(struct get_connection_info_params){ low->session, &info }) == SEC_E_INVALID_HANDLE);
    strcpy(low->host, "localhost");
    target = (typeof(target)){ low->session, PtrToUlong(low->host) };
    assert(!wow64_set_session_target(&target));
    assert(!wow64_free_credentials(&allocate.cred));
    low->desc = (struct secbufferdesc32){ 0, 1, PtrToUlong(&low->buffer) };
    low->buffer = (struct secbuffer32){ sizeof(low->data), SECBUFFER_TOKEN, PtrToUlong(low->data) };
    handshake.session = low->session;
    handshake.output = PtrToUlong(&low->desc);
    handshake.consumed = PtrToUlong(&low->consumed);
    handshake.index = PtrToUlong(&low->index);
    handshake.written = PtrToUlong(&low->written);
    assert(wow64_handshake(&handshake) == SEC_I_CONTINUE_NEEDED);
    assert(low->index == 0 && low->written && !low->consumed);
    assert(!session_from_handle(low->session)->output.desc);
    low->desc.count = 4;
    assert(wow64_handshake(&handshake) == STATUS_INVALID_PARAMETER);
    assert(!schan_dispose_session(&(struct session_params){ low->session }));
    assert(VirtualFree(low, 0, MEM_RELEASE));
}

int main(int argc, char **argv)
{
    assert(ARRAY_SIZE(wine_nx_secur32_unix_funcs) == unix_funcs_count);
    assert(wine_nx_secur32_wow64_unix_count == unix_funcs_count);
    assert(!process_attach(NULL));
    test_session(65536, MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, 2);
    test_session(29, MBEDTLS_TLS_RSA_WITH_AES_128_CBC_SHA256, 2);
    test_session(29, MBEDTLS_TLS_RSA_WITH_AES_128_CBC_SHA, 0);
    test_session(65536, MBEDTLS_TLS_RSA_WITH_AES_128_CBC_SHA, 1);
    test_wow64();
    if (argc == 3) test_https(argv[1], argv[2]);
    assert(!process_detach(NULL));
    puts("Schannel TLS: TLS 1.0-1.2, fragmented buffers, GCM/CBC, ALPN, certificates, bindings, Win32 and lifetimes passed");
    return 0;
}
