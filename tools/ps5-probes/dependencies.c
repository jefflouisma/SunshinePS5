/* Nonshipping T043 dependency smoke. No console capture/input API is called.
 * OpenSSL ABI/source: 45e844fa2a14ec92d146bd8f5778ac130b6625fb,
 * include/openssl/{ssl,x509,pem,rand,evp}.h.in and crypto/threads_pthread.c.
 * ENet ABI/source: cgutman/enet aca87840b57f045a1f7f9299e4b1b9b8e2a5e2f1,
 * include/enet/enet.h, unix.c, host.c, protocol.c; protocol-notes.md section 11.
 * POSIX ABI: SDK a7685ce8e97ce1f8a4f38253a41d54241c9124f1 target headers
 * pthread.h, sys/socket.h, sys/time.h, time.h, unistd.h. Stock crt/crt.c ->
 * crt/patch.c performs the owner-accepted per-process privileged bootstrap.
 * No added credential writes, hooks, firmware offsets or persistence.
 * Build DEPENDENCIES_ENET for a separate bounded echo server on port 9043;
 * DEPENDENCIES_HOST binds loopback, target binds only the isolated link.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifdef DEPENDENCIES_ENET
#include <enet/enet.h>

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (enet_initialize() != 0) {
        return 1;
    }
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(9043);
#ifdef DEPENDENCIES_HOST
    const char *address = "127.0.0.1";
#else
    const char *address = "192.168.2.2";
#endif
    ENetAddress bind_address;
    if (inet_pton(AF_INET, address, &local.sin_addr) != 1 ||
        enet_address_set_address(&bind_address, (struct sockaddr *)&local, sizeof(local)) != 0) {
        enet_deinitialize();
        return 1;
    }
    ENetHost *host = enet_host_create(AF_INET, &bind_address, 1, 0x30, 0, 0);
    if (host == NULL) {
        enet_deinitialize();
        return 1;
    }
    host->maximumPacketSize = 8192;
    host->maximumWaitingData = 65536;
    printf("T043_ENET ready=%s:9043 channels=48 checksum=none compression=none mtu=%u\n", address,
           host->mtu);
    bool connected = false, disconnected = false;
    unsigned received = 0;
    int result = 1;
    const enet_uint32 started = enet_time_get();
    while (enet_time_get() - started < 15000U) {
        ENetEvent event;
        int status = enet_host_service(host, &event, 20);
        if (status < 0) {
            break;
        }
        if (status == 0) {
            continue;
        }
        if (event.type == ENET_EVENT_TYPE_CONNECT) {
            printf("T043_ENET connect_data=%08x channels=%zu\n", event.data,
                   event.peer->channelCount);
            if (event.data != UINT32_C(0x12345678)) {
                break;
            }
            connected = true;
        } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            printf("T043_ENET receive channel=%u flags=%u bytes=%zu\n", event.channelID,
                   event.packet->flags, event.packet->dataLength);
            ENetPacket *reply = enet_packet_create(event.packet->data, event.packet->dataLength,
                                                   ENET_PACKET_FLAG_RELIABLE);
            enet_packet_destroy(event.packet);
            if (reply == NULL) {
                break;
            }
            if (enet_peer_send(event.peer, 0, reply) != 0) {
                enet_packet_destroy(reply);
                break;
            }
            ++received;
        } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            disconnected = true;
            result = connected && received == 6U ? 0 : 1;
            break;
        }
    }
    printf("T043_ENET received=%u disconnect=%s result=%d\n", received,
           disconnected ? "observed" : "absent", result);
    enet_host_destroy(host);
    enet_deinitialize();
    return result;
}
#else
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

static int entropy(void)
{
    unsigned char a[32], b[32];
    return RAND_poll() == 1 && RAND_status() == 1 && RAND_bytes(a, sizeof(a)) == 1 &&
           RAND_bytes(b, sizeof(b)) == 1 && memcmp(a, b, sizeof(a)) != 0;
}

typedef struct {
    SSL_CTX *context;
    int listener;
    int passed;
} Server;

static void *serve(void *argument)
{
    Server *server = argument;
    int connection = accept(server->listener, NULL, NULL);
    SSL *tls = SSL_new(server->context);
    struct timeval timeout = {3, 0};
    char bytes[4];
    if (connection >= 0 && tls != NULL &&
        setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
        setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0 &&
        SSL_set_fd(tls, connection) == 1 && SSL_accept(tls) == 1 &&
        SSL_get_verify_result(tls) == X509_V_OK && SSL_read(tls, bytes, sizeof(bytes)) == 4 &&
        memcmp(bytes, "ping", 4) == 0 && SSL_write(tls, "pong", 4) == 4 && entropy()) {
        server->passed = 1;
    }
    SSL_free(tls);
    if (connection >= 0) {
        close(connection);
    }
    return NULL;
}

static int configure(SSL_CTX *context, X509 *certificate, EVP_PKEY *key)
{
    if (context == NULL || SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) != 1 ||
        SSL_CTX_use_certificate(context, certificate) != 1 ||
        SSL_CTX_use_PrivateKey(context, key) != 1 || SSL_CTX_check_private_key(context) != 1 ||
        X509_STORE_add_cert(SSL_CTX_get_cert_store(context), certificate) != 1) {
        return 0;
    }
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    return 1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("T043_OPENSSL version=%s threads=enabled seed=os no_added_privileged_changes=yes\n",
           OpenSSL_version(OPENSSL_VERSION));
    int result = 1, connection = -1;
    EVP_PKEY_CTX *key_context = NULL;
    EVP_PKEY *key = NULL;
    X509 *certificate = NULL, *parsed = NULL;
    BIO *pem = NULL;
    X509_STORE *untrusted = NULL;
    X509_STORE_CTX *verification = NULL;
    SSL_CTX *server_context = NULL, *client_context = NULL;
    SSL *client = NULL;
    Server server = {NULL, -1, 0};
    pthread_t thread;
    bool thread_started = false;
    if (!entropy()) {
        goto cleanup;
    }
    puts("T043_ENTROPY poll=1 status=1 distinct_32_byte_outputs=yes source_strength=not_measured");
    key_context = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
    if (key_context == NULL || EVP_PKEY_keygen_init(key_context) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(key_context, 2048) <= 0 ||
        EVP_PKEY_keygen(key_context, &key) <= 0) {
        goto cleanup;
    }
    certificate = X509_new();
    if (certificate == NULL || X509_set_version(certificate, 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1) != 1 ||
        X509_gmtime_adj(X509_getm_notBefore(certificate), -60) == NULL ||
        X509_gmtime_adj(X509_getm_notAfter(certificate), 3600) == NULL ||
        X509_set_pubkey(certificate, key) != 1) {
        goto cleanup;
    }
    X509_NAME *name = X509_get_subject_name(certificate);
    if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   (const unsigned char *)"nonshipping-smoke", -1, -1, 0) != 1 ||
        X509_set_issuer_name(certificate, name) != 1 ||
        X509_sign(certificate, key, EVP_sha256()) <= 0 || X509_verify(certificate, key) != 1) {
        goto cleanup;
    }
    pem = BIO_new(BIO_s_mem());
    if (pem == NULL || PEM_write_bio_X509(pem, certificate) != 1 ||
        (parsed = PEM_read_bio_X509(pem, NULL, NULL, NULL)) == NULL ||
        X509_cmp(parsed, certificate) != 0) {
        goto cleanup;
    }
    puts("T043_CERT rsa_bits=2048 signature=sha256 pem_roundtrip=pass signature_verify=pass");
    untrusted = X509_STORE_new();
    verification = X509_STORE_CTX_new();
    if (untrusted == NULL || verification == NULL ||
        X509_STORE_CTX_init(verification, untrusted, parsed, NULL) != 1 ||
        X509_verify_cert(verification) != 0 ||
        X509_STORE_CTX_get_error(verification) != X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT) {
        goto cleanup;
    }
    puts("T043_CERT untrusted_self_signed=rejected");
    server_context = SSL_CTX_new(TLS_server_method());
    client_context = SSL_CTX_new(TLS_client_method());
    if (!configure(server_context, certificate, key) ||
        !configure(client_context, certificate, key)) {
        goto cleanup;
    }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server.listener = socket(AF_INET, SOCK_STREAM, 0);
    connection = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval timeout = {3, 0};
    if (server.listener < 0 || connection < 0 ||
        setsockopt(server.listener, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ||
        bind(server.listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server.listener, 1) != 0) {
        goto cleanup;
    }
    socklen_t length = sizeof(address);
    if (getsockname(server.listener, (struct sockaddr *)&address, &length) != 0 ||
        connect(connection, (struct sockaddr *)&address, length) != 0) {
        goto cleanup;
    }
    server.context = server_context;
    if (pthread_create(&thread, NULL, serve, &server) != 0) {
        goto cleanup;
    }
    thread_started = true;
    client = SSL_new(client_context);
    char response[4];
    if (client == NULL || SSL_set_fd(client, connection) != 1 || SSL_connect(client) != 1 ||
        SSL_get_verify_result(client) != X509_V_OK || SSL_write(client, "ping", 4) != 4 ||
        SSL_read(client, response, sizeof(response)) != 4 || memcmp(response, "pong", 4) != 0 ||
        !entropy()) {
        goto cleanup;
    }
    printf("T043_TLS protocol=%s cipher=%s mutual_cert_verify=pass socket_echo=pass\n",
           SSL_get_version(client), SSL_get_cipher_name(client));
    result = 0;
cleanup:
    if (thread_started) {
        if (pthread_join(thread, NULL) != 0 || !server.passed) {
            result = 1;
        }
    }
    SSL_free(client);
    if (connection >= 0) {
        close(connection);
    }
    if (server.listener >= 0) {
        close(server.listener);
    }
    SSL_CTX_free(client_context);
    SSL_CTX_free(server_context);
    BIO_free(pem);
    X509_STORE_CTX_free(verification);
    X509_STORE_free(untrusted);
    X509_free(parsed);
    X509_free(certificate);
    EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(key_context);
    if (result != 0) {
        ERR_print_errors_fp(stdout);
    }
    printf("T043_DEPENDENCIES result=%d thread_join=%s resources=released errno=%d\n", result,
           thread_started ? "observed" : "not_started", errno);
    return result;
}
#endif
