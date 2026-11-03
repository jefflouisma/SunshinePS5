/* Provisional production console composition: PAIR, WEB and STATUS only.
 *
 * This entry point starts the real portable core over the POSIX storage and
 * the provisional firmware-13.60 system adapter. It serves:
 *   - the embedded core web app and its read-only status on the private link;
 *   - Moonlight HTTP /serverinfo and /pair with deferred pairing responses;
 *   - the Moonlight HTTPS pairing/certificate listener.
 *
 * It does NOT compose capture, encode, audio, input, RTSP, control, metrics or
 * media sockets. A /launch or /resume request receives the core XML response
 * but no session is activated by this host; streaming is not composed yet. No
 * console or network deployment is done here, and no synthetic peer or hardware
 * claim is made.
 *
 * Console system operations are the real provisional T075 adapter. Its notify
 * has no sourced console API and returns STAGE_UNAVAILABLE; this composition
 * treats only that result as "notification skipped" so pairing can continue via
 * the web PIN page. Every other system result is propagated unchanged. */
#include "firmware/13.60.h"
#include "lifecycle.h"
#include "pairing.h"
#include "protocol/http.h"
#include "protocol/pairing_flow.h"
#include "runtime.h"
#include "session/lifecycle.h"
#include "session/session.h"
#include "settings/host.h"
#include "system/ps5.h"
#include "web/server.h"
#include "web_service.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef SP5_CONSOLE_DEFAULT_RUN_SECONDS
#define SP5_CONSOLE_DEFAULT_RUN_SECONDS 0
#endif

enum { CONSOLE_HTTP_CONNECTIONS = 8, CONSOLE_BACKLOG = 8 };

typedef struct {
    const char *bind_address;
    const char *state_directory;
    uint16_t http_port;
    uint16_t https_port;
    uint16_t web_port;
    uint64_t run_seconds;
} console_options;

typedef struct {
    int fd;
    SSL *tls;
    bool secure;
    uint64_t deadline_us;
    uint64_t request_id;
    bool deferred;
    char peer[INET_ADDRSTRLEN];
    uint8_t request[SP5_HTTP_LIMIT];
    size_t used;
} console_http_connection;

typedef struct {
    sp5_system_ops base;
} console_system;

typedef struct {
    console_options options;
    sp5_posix_storage *storage;
    sp5_store *store;
    sp5_host host;
    sp5_authority *authority;
    sp5_session *session;
    sp5_pairing *pairing;
    sp5_pair_flow *pair;
    sp5_http_service *http;
    sp5_web *web;
    sp5_ps5_system system;
    console_system console_system;
    sp5_system_ops system_ops;
    bool system_prepared;
    sp5_bytes certificate;
    sp5_bytes private_key;
    int http_listener;
    uint16_t http_bound_port;
    int https_listener;
    uint16_t https_bound_port;
    SSL_CTX *tls_context;
    char web_authority[64];
    char pin_url[128];
    console_http_connection http_connections[CONSOLE_HTTP_CONNECTIONS];
    sp5_console_web web_service;
} console_host;

static volatile sig_atomic_t received_signal;

/* The console composition links no lifecycle/media/control stack yet. These
 * optional web-owner hooks are only reached when a lifecycle is supplied; this
 * host always passes NULL. They fail closed if that invariant is ever broken. */
sp5_session_view sp5_lifecycle_read(const sp5_lifecycle *lifecycle)
{
    (void)lifecycle;
    return (sp5_session_view){0};
}

sp5_result sp5_lifecycle_stop_input(sp5_lifecycle *lifecycle, sp5_end_reason reason)
{
    (void)lifecycle;
    (void)reason;
    return sp5_failure(SP5_REASON_UNSUPPORTED);
}

sp5_result sp5_lifecycle_end(sp5_lifecycle *lifecycle, sp5_end_reason reason)
{
    (void)lifecycle;
    (void)reason;
    return sp5_failure(SP5_REASON_UNSUPPORTED);
}

static void on_signal(int signal_number)
{
    received_signal = signal_number;
}

static uint64_t monotonic_us(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0 || t.tv_sec < 0)
        return 0;
    return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}

static sp5_optional_u64 wall_clock_us(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_REALTIME, &t) != 0 || t.tv_sec < 0)
        return (sp5_optional_u64){0};
    return (sp5_optional_u64){true, (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000};
}

static int nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int open_listener(const char *bind_address, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int reuse = 1;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0 ||
        inet_pton(AF_INET, bind_address, &address.sin_addr) != 1 ||
        bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, CONSOLE_BACKLOG) != 0 || nonblocking(fd) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

static bool listener_port(int fd, uint16_t *port)
{
    struct sockaddr_in address;
    socklen_t size = sizeof(address);
    if (getsockname(fd, (struct sockaddr *)&address, &size) != 0)
        return false;
    *port = ntohs(address.sin_port);
    return true;
}

static size_t json_escape(char *output, size_t capacity, const char *input)
{
    static const char hex[] = "0123456789abcdef";
    size_t size = 0;
    for (const unsigned char *at = (const unsigned char *)input; *at != 0; ++at) {
        if (*at == '"' || *at == '\\') {
            if (size + 2 >= capacity)
                return 0;
            output[size++] = '\\';
            output[size++] = (char)*at;
        } else if (*at < 0x20) {
            if (size + 6 >= capacity)
                return 0;
            output[size++] = '\\';
            output[size++] = 'u';
            output[size++] = '0';
            output[size++] = '0';
            output[size++] = hex[*at >> 4];
            output[size++] = hex[*at & 15];
        } else {
            if (size + 1 >= capacity)
                return 0;
            output[size++] = (char)*at;
        }
    }
    output[size] = 0;
    return size;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static int emit_ready(const char *host_name, const char *address, const char *authority,
                      uint16_t http_port, uint16_t https_port)
{
    char escaped[SP5_NAME_CAPACITY * 6];
    if (json_escape(escaped, sizeof(escaped), host_name) == 0)
        return -1;
    return printf("{\"event\":\"ready\",\"host\":\"%s\",\"address\":\"%s\",\"web\":\"http://%s\","
                  "\"http_port\":%u,\"https_port\":%u,\"firmware\":\"13.60\"}\n",
                  escaped, address, authority, (unsigned)http_port, (unsigned)https_port) < 0
               ? -1
               : 0;
}

static int emit_stopped(const char *reason)
{
    return printf("{\"event\":\"stopped\",\"reason\":\"%s\"}\n", reason) < 0 ? -1 : 0;
}

static void print_usage(FILE *stream)
{
    (void)fputs("usage: sunshineps5 [--bind IPV4] [--state DIR] [--http-port N]\n"
                "                   [--https-port N] [--web-port N] [--run-seconds N] [--help]\n"
                "\n"
                "  --bind IPV4        private IPv4 to serve (default 192.168.2.2)\n"
                "  --state DIR        existing 0700 state directory\n"
                "                     (default /data/ps5-dev/sunshine-state)\n"
                "  --http-port N      Moonlight HTTP pairing/serverinfo port (default 47989)\n"
                "  --https-port N     Moonlight HTTPS pairing port (default 47984)\n"
                "  --web-port N       core web app/status port (default 8081)\n"
                "  --run-seconds N    exit after N seconds; 0 means forever (default 0)\n"
                "  --help             print this text\n",
                stream);
}

static bool parse_port(const char *text, uint16_t *port)
{
    if (text == NULL || text[0] == 0)
        return false;
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != 0 || value > 65535)
        return false;
    *port = (uint16_t)value;
    return true;
}

static bool parse_seconds(const char *text, uint64_t *seconds)
{
    if (text == NULL || text[0] == 0 || text[0] == '-')
        return false;
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != 0 || value > UINT64_MAX / 1000000)
        return false;
    *seconds = (uint64_t)value;
    return true;
}

static int parse_options(int argc, char **argv, console_options *options, bool *help)
{
    *options = (console_options){.bind_address = "192.168.2.2",
                                 .state_directory = "/data/ps5-dev/sunshine-state",
                                 .http_port = 47989,
                                 .https_port = 47984,
                                 .web_port = 8081,
                                 .run_seconds = SP5_CONSOLE_DEFAULT_RUN_SECONDS};
    *help = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            *help = true;
        } else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
            options->bind_address = argv[++i];
        } else if (strcmp(argv[i], "--state") == 0 && i + 1 < argc) {
            options->state_directory = argv[++i];
        } else if (strcmp(argv[i], "--http-port") == 0 && i + 1 < argc) {
            if (!parse_port(argv[++i], &options->http_port))
                return 64;
        } else if (strcmp(argv[i], "--https-port") == 0 && i + 1 < argc) {
            if (!parse_port(argv[++i], &options->https_port) || options->https_port == 0)
                return 64;
        } else if (strcmp(argv[i], "--web-port") == 0 && i + 1 < argc) {
            if (!parse_port(argv[++i], &options->web_port))
                return 64;
        } else if (strcmp(argv[i], "--run-seconds") == 0 && i + 1 < argc) {
            if (!parse_seconds(argv[++i], &options->run_seconds))
                return 64;
        } else {
            return 64;
        }
    }
    return options->http_port == options->https_port ? 64 : 0;
}

static sp5_result console_notify(void *context, const char *message, size_t size)
{
    console_system *system = context;
    sp5_result result = system->base.notify(system->base.context, message, size);
    /* No sourced console notification API exists. Pairing still owns the PIN
     * page and must not be aborted merely because the visual notice is absent. */
    if (result.status != SP5_OK && result.reason == SP5_REASON_STAGE_UNAVAILABLE)
        return sp5_success();
    return result;
}

static sp5_system_ops console_system_ops(console_system *wrapper, sp5_ps5_system *system)
{
    wrapper->base = sp5_ps5_system_ops(system);
    sp5_system_ops ops = wrapper->base;
    ops.context = wrapper;
    ops.notify = console_notify;
    return ops;
}

static void close_http(console_http_connection *connection)
{
    SSL_free(connection->tls);
    if (connection->fd >= 0)
        (void)close(connection->fd);
    *connection = (console_http_connection){.fd = -1};
}

static bool http_request_ready(sp5_bytes raw)
{
    for (size_t i = 0; i + 3 < raw.size; i++) {
        if (memcmp(raw.data + i, "\r\n\r\n", 4) != 0)
            continue;
        size_t length = 0, at = 0;
        bool found = false;
        while (at < i) {
            size_t end = at;
            while (end < i && raw.data[end] != '\r')
                end++;
            static const char key[] = "content-length:";
            bool match = end - at >= sizeof(key) - 1;
            for (size_t j = 0; match && j < sizeof(key) - 1; j++) {
                uint8_t c = raw.data[at + j];
                if (c >= 'A' && c <= 'Z')
                    c = (uint8_t)(c + 32);
                match = c == (uint8_t)key[j];
            }
            if (match) {
                if (found)
                    return true;
                found = true;
                size_t start = at + sizeof(key) - 1;
                while (start < end && (raw.data[start] == ' ' || raw.data[start] == '\t'))
                    start++;
                if (start == end)
                    return true;
                for (size_t j = start; j < end; j++) {
                    if (raw.data[j] < '0' || raw.data[j] > '9' || length > SP5_HTTP_LIMIT / 10)
                        return true;
                    length = length * 10 + (raw.data[j] - '0');
                }
            }
            at = end + 2;
        }
        return raw.size >= i + 4 + length;
    }
    return raw.size == SP5_HTTP_LIMIT;
}

static bool send_all(console_http_connection *connection, sp5_bytes wire)
{
    size_t sent = 0;
    uint64_t deadline = monotonic_us() + 1000000;
    while (sent < wire.size && monotonic_us() < deadline) {
        ssize_t count;
        short events = POLLOUT;
        if (connection->secure) {
            int result = SSL_write(connection->tls, wire.data + sent, (int)(wire.size - sent));
            if (result > 0) {
                sent += (size_t)result;
                continue;
            }
            int error = SSL_get_error(connection->tls, result);
            if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
                return false;
            events = error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
        } else {
            count = send(connection->fd, wire.data + sent, wire.size - sent, 0);
            if (count > 0) {
                sent += (size_t)count;
                continue;
            }
            if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                return false;
        }
        struct pollfd wait = {connection->fd, events, 0};
        if (poll(&wait, 1, 10) < 0 && errno != EINTR)
            return false;
    }
    return sent == wire.size;
}

static void reply_http(console_http_connection *connection, const sp5_http_response *response)
{
    uint8_t wire[SP5_HTTP_LIMIT + 512];
    sp5_buffer output = {wire, sizeof(wire), 0};
    if (sp5_http_render(response, &output).status == SP5_OK)
        (void)send_all(connection, (sp5_bytes){output.data, output.size});
    close_http(connection);
}

static bool tls_wants(int error)
{
    return error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE;
}

static void service_http(console_host *host, console_http_connection *connection)
{
    uint64_t now = monotonic_us();
    if (connection->deferred) {
        uint8_t fields[SP5_HTTP_LIMIT];
        sp5_http_response response = {.code = 200, .fields = {fields, sizeof(fields), 0}};
        sp5_result result =
            sp5_pair_flow_take_certificate(host->pair, connection->request_id, now, &response);
        if (result.reason == SP5_REASON_WOULD_BLOCK)
            return;
        if (result.status != SP5_OK) {
            close_http(connection);
            return;
        }
        reply_http(connection, &response);
        return;
    }
    if (connection->secure && !SSL_is_init_finished(connection->tls)) {
        int result = SSL_accept(connection->tls);
        if (result != 1) {
            int error = SSL_get_error(connection->tls, result);
            if (tls_wants(error))
                return;
            close_http(connection);
            return;
        }
    }
    ssize_t count;
    if (connection->secure) {
        int result = SSL_read(connection->tls, connection->request + connection->used,
                              (int)(sizeof(connection->request) - connection->used));
        if (result <= 0) {
            int error = SSL_get_error(connection->tls, result);
            if (tls_wants(error))
                return;
            close_http(connection);
            return;
        }
        count = result;
    } else {
        count = recv(connection->fd, connection->request + connection->used,
                     sizeof(connection->request) - connection->used, 0);
        if (count <= 0) {
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                return;
            close_http(connection);
            return;
        }
    }
    connection->used += (size_t)count;
    connection->deadline_us = now + 10000000;
    if (!http_request_ready((sp5_bytes){connection->request, connection->used}))
        return;
    sp5_request request;
    uint8_t fields[SP5_HTTP_LIMIT];
    sp5_http_response response = {.code = 200, .fields = {fields, sizeof(fields), 0}};
    if (sp5_http_parse((sp5_bytes){connection->request, connection->used},
                       sizeof(connection->request), &request)
            .status != SP5_OK) {
        response.code = 400;
    } else {
        sp5_result result = sp5_http_route(host->http, connection->peer, false, connection->tls,
                                           &request, now, wall_clock_us(), &response);
        if (result.status != SP5_OK) {
            close_http(connection);
            return;
        }
        /* /launch or /resume may return the core XML response here, but this
         * composition activates no media session; streaming is not composed. */
        if (response.deferred) {
            sp5_pair_view pairing = sp5_pairing_read(host->pairing, now);
            if (pairing.request_id == 0) {
                close_http(connection);
                return;
            }
            connection->deferred = true;
            connection->request_id = pairing.request_id;
            connection->deadline_us = now + SP5_PAIR_EXPIRY_US;
            return;
        }
    }
    reply_http(connection, &response);
}

static void accept_http(console_host *host, int listener, bool secure)
{
    struct sockaddr_in remote;
    socklen_t size = sizeof(remote);
    int fd = accept(listener, (struct sockaddr *)&remote, &size);
    if (fd < 0)
        return;
    char peer[INET_ADDRSTRLEN];
    size_t slot = 0;
    while (slot < CONSOLE_HTTP_CONNECTIONS && host->http_connections[slot].fd >= 0)
        ++slot;
    if (slot == CONSOLE_HTTP_CONNECTIONS ||
        inet_ntop(AF_INET, &remote.sin_addr, peer, sizeof(peer)) == NULL ||
        !sp5_peer_permitted(peer, false) || nonblocking(fd) != 0) {
        (void)close(fd);
        return;
    }
    SSL *tls = NULL;
    if (secure) {
        tls = SSL_new(host->tls_context);
        if (tls == NULL || SSL_set_fd(tls, fd) != 1) {
            SSL_free(tls);
            (void)close(fd);
            return;
        }
        SSL_set_accept_state(tls);
    }
    console_http_connection *connection = &host->http_connections[slot];
    *connection = (console_http_connection){
        .fd = fd, .tls = tls, .secure = secure, .deadline_us = monotonic_us() + 10000000};
    memcpy(connection->peer, peer, strlen(peer) + 1);
}

static void destroy_console(console_host *host)
{
    for (size_t i = 0; i < CONSOLE_HTTP_CONNECTIONS; ++i)
        close_http(&host->http_connections[i]);
    if (host->http_listener >= 0)
        (void)close(host->http_listener);
    host->http_listener = -1;
    if (host->https_listener >= 0)
        (void)close(host->https_listener);
    host->https_listener = -1;
    host->tls_context = NULL;
    sp5_console_web_close(&host->web_service);
    sp5_web_destroy(host->web);
    host->web = NULL;
    sp5_http_destroy(host->http);
    host->http = NULL;
    sp5_pair_flow_destroy(host->pair);
    host->pair = NULL;
    sp5_pairing_destroy(host->pairing);
    host->pairing = NULL;
    sp5_session_destroy(host->session);
    host->session = NULL;
    sp5_authority_destroy(host->authority);
    host->authority = NULL;
    if (host->system_prepared)
        (void)sp5_ps5_system_rollback(&host->system);
    host->system_prepared = false;
    sp5_store_destroy(host->store);
    host->store = NULL;
    if (host->storage != NULL)
        (void)sp5_posix_storage_close(host->storage);
    host->storage = NULL;
}

static int start_console(console_host *host)
{
    host->http_listener = -1;
    host->https_listener = -1;
    for (size_t i = 0; i < CONSOLE_HTTP_CONNECTIONS; ++i)
        host->http_connections[i].fd = -1;
    sp5_console_web_init(&host->web_service, NULL, -1);

    host->storage = sp5_posix_storage_open(host->options.state_directory);
    if (host->storage == NULL) {
        (void)fputs("state directory is not an existing 0700 directory: ", stderr);
        (void)fputs(host->options.state_directory, stderr);
        (void)fputc('\n', stderr);
        return 1;
    }
    sp5_storage_ops storage_ops = sp5_posix_storage_operations(host->storage);
    host->store = sp5_store_create(&storage_ops);
    if (host->store == NULL || sp5_store_load(host->store).status != SP5_OK) {
        (void)fputs("could not load the console store\n", stderr);
        return 1;
    }
    if (sp5_host_init(&host->host, "console-provisional-0.1", "13.60", host->options.bind_address)
                .status != SP5_OK ||
        sp5_host_refresh(&host->host, host->store).status != SP5_OK ||
        sp5_host_transition(&host->host, SP5_HOST_RUNNING).status != SP5_OK) {
        (void)fputs("could not initialize the host\n", stderr);
        return 1;
    }
    host->authority = sp5_authority_create(sp5_store_devices(host->store));
    host->session = sp5_session_create(host->authority);
    host->pairing = sp5_pairing_create();
    if (host->authority == NULL || host->session == NULL || host->pairing == NULL) {
        (void)fputs("could not create session state\n", stderr);
        return 1;
    }
    sp5_bytes identity = sp5_store_identity(host->store);
    const char certificate_begin[] = "-----BEGIN CERTIFICATE-----";
    size_t certificate_offset = 0;
    while (certificate_offset + sizeof(certificate_begin) - 1 <= identity.size &&
           memcmp(identity.data + certificate_offset, certificate_begin,
                  sizeof(certificate_begin) - 1) != 0)
        ++certificate_offset;
    if (certificate_offset + sizeof(certificate_begin) - 1 > identity.size) {
        (void)fputs("stored identity has no PEM certificate\n", stderr);
        return 1;
    }
    host->certificate =
        (sp5_bytes){identity.data + certificate_offset, identity.size - certificate_offset};
    host->private_key = (sp5_bytes){identity.data, certificate_offset};

    sp5_ps5_system_init(&host->system);
    sp5_platform_provenance provenance;
    if (sp5_ps5_system_probe(&host->system, &provenance).status != SP5_OK ||
        sp5_ps5_system_prepare(&host->system).status != SP5_OK) {
        (void)fputs("firmware 13.60 system probe refused\n", stderr);
        return 1;
    }
    host->system_prepared = true;
    host->system_ops = console_system_ops(&host->console_system, &host->system);

    int web_listener = sp5_console_web_listen(host->options.bind_address, host->options.web_port,
                                              host->web_authority, sizeof(host->web_authority));
    if (web_listener < 0) {
        (void)fputs("could not listen on the web port\n", stderr);
        return 1;
    }
    int url_size =
        snprintf(host->pin_url, sizeof(host->pin_url), "http://%s/", host->web_authority);
    if (url_size < 0 || (size_t)url_size >= sizeof(host->pin_url)) {
        (void)close(web_listener);
        (void)fputs("web authority is too long\n", stderr);
        return 1;
    }
    sp5_pair_flow_config pair_config = {.state = host->pairing,
                                        .devices = sp5_store_devices(host->store),
                                        .certificate_pem = host->certificate,
                                        .private_key_pem = host->private_key,
                                        .system = host->system_ops,
                                        .pin_url = host->pin_url};
    host->pair = sp5_pair_flow_create(&pair_config);
    if (host->pair == NULL) {
        (void)close(web_listener);
        (void)fputs("could not create the pairing flow\n", stderr);
        return 1;
    }
    sp5_http_config http_config = {.host = &host->host,
                                   .devices = sp5_store_devices(host->store),
                                   .session = host->session,
                                   .certificate_pem = host->certificate,
                                   .private_key_pem = host->private_key,
                                   .pair_context = host->pair,
                                   .pair = sp5_pair_flow_handle};
    host->http = sp5_http_create(&http_config);
    if (host->http == NULL) {
        (void)close(web_listener);
        (void)fputs("could not create the HTTP service\n", stderr);
        return 1;
    }
    host->http_listener = open_listener(host->options.bind_address, host->options.http_port);
    if (host->http_listener < 0 || !listener_port(host->http_listener, &host->http_bound_port)) {
        (void)close(web_listener);
        (void)fputs("could not listen on the HTTP port\n", stderr);
        return 1;
    }
    host->tls_context = sp5_http_tls_context(host->http);
    if (host->tls_context == NULL) {
        (void)close(web_listener);
        (void)fputs("could not obtain the HTTP TLS context\n", stderr);
        return 1;
    }
    host->https_listener = open_listener(host->options.bind_address, host->options.https_port);
    if (host->https_listener < 0 || !listener_port(host->https_listener, &host->https_bound_port)) {
        (void)close(web_listener);
        (void)fputs("could not listen on the HTTPS port\n", stderr);
        return 1;
    }
    if (host->https_bound_port == host->http_bound_port) {
        (void)close(web_listener);
        (void)fputs("HTTP and HTTPS ports must differ\n", stderr);
        return 1;
    }
    /* No lifecycle is composed: web mutations run only while no media
     * producers exist, and the read-only status is the core status route. */
    sp5_web_config web_config = {.store = host->store,
                                 .host = &host->host,
                                 .session = host->session,
                                 .pairing = host->pairing,
                                 .pair_flow = host->pair,
                                 .authority = host->web_authority};
    host->web = sp5_web_create(&web_config);
    if (host->web == NULL) {
        (void)close(web_listener);
        (void)fputs("could not create the web service\n", stderr);
        return 1;
    }
    sp5_console_web_init(&host->web_service, host->web, web_listener);
    return 0;
}

int main(int argc, char **argv)
{
    console_host *host = calloc(1, sizeof(*host));
    if (host == NULL)
        return 1;
    bool help = false;
    int parsed = parse_options(argc, argv, &host->options, &help);
    if (parsed != 0) {
        print_usage(stderr);
        free(host);
        return parsed;
    }
    if (help) {
        print_usage(stdout);
        free(host);
        return 0;
    }
    struct in_addr bind_address;
    if (inet_pton(AF_INET, host->options.bind_address, &bind_address) != 1 ||
        !sp5_peer_permitted(host->options.bind_address, false)) {
        (void)fputs("bind address is not a permitted private IPv4 address: ", stderr);
        (void)fputs(host->options.bind_address, stderr);
        (void)fputc('\n', stderr);
        free(host);
        return 64;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR || signal(SIGINT, on_signal) == SIG_ERR ||
        signal(SIGTERM, on_signal) == SIG_ERR || setvbuf(stdout, NULL, _IOLBF, 0) != 0) {
        (void)fputs("could not install process handlers\n", stderr);
        free(host);
        return 1;
    }
    if (start_console(host) != 0) {
        (void)emit_stopped("error");
        destroy_console(host);
        free(host);
        return 1;
    }
    sp5_settings settings = sp5_store_settings(host->store);
    if (emit_ready(settings.host_name, host->options.bind_address, host->web_authority,
                   host->http_bound_port, host->https_bound_port) != 0) {
        destroy_console(host);
        free(host);
        return 1;
    }

    const char *reason = "error";
    int status = 0;
    uint64_t started = monotonic_us();
    uint64_t deadline =
        host->options.run_seconds == 0 ? UINT64_MAX : started + host->options.run_seconds * 1000000;
    for (;;) {
        if (received_signal != 0) {
            reason = received_signal == SIGINT    ? "sigint"
                     : received_signal == SIGTERM ? "sigterm"
                                                  : "signal";
            break;
        }
        uint64_t now = monotonic_us();
        if (now >= deadline) {
            reason = "run-seconds";
            break;
        }
        struct pollfd polls[2 + CONSOLE_HTTP_CONNECTIONS + SP5_CONSOLE_WEB_POLLS];
        polls[0] = (struct pollfd){host->http_listener, POLLIN, 0};
        polls[1] = (struct pollfd){host->https_listener, POLLIN, 0};
        for (size_t i = 0; i < CONSOLE_HTTP_CONNECTIONS; ++i) {
            short events = POLLIN;
            if (host->http_connections[i].secure && SSL_want_write(host->http_connections[i].tls))
                events |= POLLOUT;
            polls[i + 2] = (struct pollfd){host->http_connections[i].fd, events, 0};
        }
        struct pollfd *web_polls = polls + 2 + CONSOLE_HTTP_CONNECTIONS;
        sp5_console_web_poll(&host->web_service, web_polls);
        int ready = poll(polls, 2 + CONSOLE_HTTP_CONNECTIONS + SP5_CONSOLE_WEB_POLLS, 10);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            status = 1;
            break;
        }
        if ((polls[0].revents & POLLIN) != 0)
            accept_http(host, host->http_listener, false);
        if ((polls[1].revents & POLLIN) != 0)
            accept_http(host, host->https_listener, true);
        for (size_t i = 0; i < CONSOLE_HTTP_CONNECTIONS; ++i)
            if (host->http_connections[i].fd >= 0) {
                if (now >= host->http_connections[i].deadline_us)
                    close_http(&host->http_connections[i]);
                else
                    service_http(host, &host->http_connections[i]);
            }
        sp5_console_web_step(&host->web_service, web_polls, monotonic_us());
        if (host->web_service.failed) {
            status = 1;
            break;
        }
        if (host->web_service.stopped) {
            reason = "web-stop";
            break;
        }
        if (sp5_pair_flow_tick(host->pair, monotonic_us()).status != SP5_OK) {
            status = 1;
            break;
        }
    }
    (void)emit_stopped(reason);
    destroy_console(host);
    free(host);
    return status;
}
