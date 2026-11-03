/* Provisional console web transport; see web_service.h. */
#include "web_service.h"
#include "web/assets.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t monotonic_us(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0 || t.tv_sec < 0)
        return 0;
    return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}

int sp5_console_web_listen(const char *ipv4, uint16_t port, char *authority, size_t capacity)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (ipv4 == NULL || authority == NULL || inet_pton(AF_INET, ipv4, &address.sin_addr) != 1)
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int reuse = 1;
    socklen_t size = sizeof(address);
    int n = -1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0 &&
        bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0 &&
        listen(fd, SP5_CONSOLE_WEB_CONNECTIONS) == 0 &&
        getsockname(fd, (struct sockaddr *)&address, &size) == 0 &&
        fcntl(fd, F_SETFL, O_NONBLOCK) == 0)
        n = snprintf(authority, capacity, "%s:%u", ipv4, (unsigned)ntohs(address.sin_port));
    if (n < 0 || (size_t)n >= capacity) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

void sp5_console_web_init(sp5_console_web *s, sp5_web *web, int listener)
{
    memset(s, 0, sizeof(*s));
    s->web = web;
    s->listener = listener;
    for (size_t i = 0; i < SP5_CONSOLE_WEB_CONNECTIONS; ++i)
        s->connections[i].fd = -1;
}

void sp5_console_web_poll(const sp5_console_web *s, struct pollfd *fds)
{
    fds[0] = (struct pollfd){s->listener, POLLIN, 0};
    for (size_t i = 0; i < SP5_CONSOLE_WEB_CONNECTIONS; ++i)
        fds[i + 1] =
            (struct pollfd){s->connections[i].fd, s->connections[i].size ? POLLOUT : POLLIN, 0};
}

/* Complete headers plus a bounded Content-Length body, or a request that can
 * never become valid (dispatch then answers it with a bounded error). */
static bool request_ready(sp5_bytes raw)
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
                    if (raw.data[j] < '0' || raw.data[j] > '9' ||
                        length > SP5_CONSOLE_WEB_REQUEST_BYTES / 10)
                        return true;
                    length = length * 10 + (raw.data[j] - '0');
                }
            }
            at = end + 2;
        }
        return length > SP5_WEB_BODY_LIMIT || raw.size >= i + 4 + length;
    }
    return raw.size == SP5_CONSOLE_WEB_REQUEST_BYTES;
}

typedef struct {
    int fd;
    uint64_t deadline;
} chunk_writer;

static sp5_result send_bytes(chunk_writer *writer, sp5_bytes bytes)
{
    size_t sent = 0;
    while (sent < bytes.size && monotonic_us() < writer->deadline) {
        ssize_t n = send(writer->fd, bytes.data + sent, bytes.size - sent, 0);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            return sp5_failure(SP5_REASON_PLATFORM);
        struct pollfd wait = {writer->fd, POLLOUT, 0};
        if (poll(&wait, 1, 10) < 0 && errno != EINTR)
            return sp5_failure(SP5_REASON_PLATFORM);
    }
    return sent == bytes.size ? sp5_success() : sp5_failure(SP5_REASON_WOULD_BLOCK);
}

static sp5_result send_chunk(void *context, sp5_bytes bytes)
{
    chunk_writer *writer = context;
    char prefix[32];
    int n = snprintf(prefix, sizeof(prefix), "%zx\r\n", bytes.size);
    if (n < 0 || (size_t)n >= sizeof(prefix))
        return sp5_failure(SP5_REASON_CAPACITY);
    sp5_result result = send_bytes(writer, (sp5_bytes){(const uint8_t *)prefix, (size_t)n});
    if (result.status == SP5_OK)
        result = send_bytes(writer, bytes);
    if (result.status == SP5_OK)
        result = send_bytes(writer, (sp5_bytes){(const uint8_t *)"\r\n", 2});
    return result;
}

/* Error/backpressure deliberately omits the terminal chunk: a client must
 * detect incomplete framing and never accept a partial body as success. */
static void stream_response(sp5_console_web *s, sp5_console_web_connection *c,
                            const sp5_web_response *response)
{
    (void)s;
    uint8_t head[1024];
    sp5_buffer framing = {head, sizeof(head), 0};
    chunk_writer writer = {c->fd, monotonic_us() + 3000000};
    if (sp5_web_format_head(response, &framing).status != SP5_OK ||
        send_bytes(&writer, (sp5_bytes){framing.data, framing.size}).status != SP5_OK)
        return;
    sp5_metrics_sink sink = {&writer, send_chunk};
    if (response->stream(response->stream_context, &sink).status == SP5_OK)
        (void)send_bytes(&writer, (sp5_bytes){(const uint8_t *)"0\r\n\r\n", 5});
}

/* Returns true when the connection must close immediately. A request that
 * cannot be dispatched or formatted (e.g. an oversized body) closes only its
 * own connection; it is not a host failure. */
static bool respond(sp5_console_web *s, sp5_console_web_connection *c, uint64_t now)
{
    sp5_web_response response = {.body = {s->body, sizeof(s->body), 0}};
    if (sp5_web_dispatch(s->web, c->peer, false, (sp5_bytes){c->request, c->used}, now, &response)
            .status != SP5_OK)
        return true;
    if (response.stream != NULL) {
        stream_response(s, c, &response);
        return true;
    }
    sp5_buffer wire = {c->wire, sizeof(c->wire), 0};
    if (sp5_web_format_head(&response, &wire).status != SP5_OK ||
        sp5_buffer_append(&wire, (sp5_bytes){response.body.data, response.body.size}).status !=
            SP5_OK)
        return true;
    c->size = wire.size;
    c->sent = 0;
    if (response.stopping)
        s->stopping = true;
    return false;
}

static void accept_one(sp5_console_web *s, uint64_t now)
{
    struct sockaddr_in remote;
    socklen_t size = sizeof(remote);
    int fd = accept(s->listener, (struct sockaddr *)&remote, &size);
    if (fd < 0)
        return;
    size_t slot = 0;
    while (slot < SP5_CONSOLE_WEB_CONNECTIONS && s->connections[slot].fd >= 0)
        ++slot;
    sp5_console_web_connection *c =
        slot < SP5_CONSOLE_WEB_CONNECTIONS ? &s->connections[slot] : NULL;
    if (c == NULL || s->stopping || fcntl(fd, F_SETFL, O_NONBLOCK) != 0 ||
        inet_ntop(AF_INET, &remote.sin_addr, c->peer, sizeof(c->peer)) == NULL) {
        if (close(fd) != 0)
            s->failed = true;
        return;
    }
    c->fd = fd;
    c->deadline_us = now + 3000000;
    c->used = c->sent = c->size = 0;
}

void sp5_console_web_step(sp5_console_web *s, const struct pollfd *fds, uint64_t now)
{
    if (fds[0].revents & POLLIN)
        accept_one(s, now);
    for (size_t i = 0; i < SP5_CONSOLE_WEB_CONNECTIONS; i++) {
        sp5_console_web_connection *c = &s->connections[i];
        short events = 0;
        if (fds[i + 1].fd == c->fd)
            events = fds[i + 1].revents;
        if (c->fd < 0)
            continue;
        bool close_now = now >= c->deadline_us || (events & (POLLERR | POLLHUP | POLLNVAL));
        if (!close_now && !c->size && (events & POLLIN)) {
            ssize_t n = recv(c->fd, c->request + c->used, sizeof(c->request) - c->used, 0);
            if (n > 0)
                c->used += (size_t)n;
            else if (n == 0 || (errno != EAGAIN && errno != EINTR))
                close_now = true;
            if (!close_now && request_ready((sp5_bytes){c->request, c->used}))
                close_now = respond(s, c, now);
        }
        if (!close_now && c->size && (events & POLLOUT)) {
            ssize_t n = send(c->fd, c->wire + c->sent, c->size - c->sent, 0);
            if (n > 0)
                c->sent += (size_t)n;
            else if (n == 0 || (errno != EAGAIN && errno != EINTR))
                close_now = true;
            if (c->sent == c->size)
                close_now = true;
        }
        if (close_now) {
            if (close(c->fd) != 0)
                s->failed = true;
            c->fd = -1;
            c->size = 0;
        }
    }
    if (s->stopping && !s->stopped) {
        bool pending = false;
        for (size_t i = 0; i < SP5_CONSOLE_WEB_CONNECTIONS; i++)
            pending = pending || (s->connections[i].fd >= 0 &&
                                  s->connections[i].size > s->connections[i].sent);
        if (!pending) {
            sp5_result result = sp5_web_finish_stop(s->web);
            if (result.status == SP5_OK)
                s->stopped = true;
            else if (s->cleanup_error == SP5_REASON_NONE)
                s->cleanup_error = result.reason;
        }
    }
}

void sp5_console_web_close(sp5_console_web *s)
{
    for (size_t i = 0; i < SP5_CONSOLE_WEB_CONNECTIONS; ++i)
        if (s->connections[i].fd >= 0) {
            if (close(s->connections[i].fd) != 0)
                s->failed = true;
            s->connections[i].fd = -1;
        }
    if (s->listener >= 0 && close(s->listener) != 0)
        s->failed = true;
    s->listener = -1;
}
