#ifndef SUNSHINE_PLATFORM_WEB_SERVICE_H
#define SUNSHINE_PLATFORM_WEB_SERVICE_H
/* Provisional console transport for the real core web app.
 *
 * This is the private-link POSIX socket adapter, not a console media adapter.
 * It frames bounded HTTP requests and dispatches them through sp5_web with the
 * actual accepted peer. It has no synthetic peer, no TLS claim and no
 * lifecycle: the console composition serves the embedded app and read-only
 * status only. Streaming is not composed yet. */
#include "web/server.h"

#include <netinet/in.h>
#include <poll.h>

enum {
    SP5_CONSOLE_WEB_CONNECTIONS = 8,
    SP5_CONSOLE_WEB_REQUEST_BYTES = 8192,
    SP5_CONSOLE_WEB_WIRE_BYTES = SP5_WEB_RESPONSE_LIMIT + 1024,
    SP5_CONSOLE_WEB_POLLS = SP5_CONSOLE_WEB_CONNECTIONS + 1
};

typedef struct {
    int fd;
    uint64_t deadline_us;
    char peer[INET_ADDRSTRLEN];
    uint8_t request[SP5_CONSOLE_WEB_REQUEST_BYTES];
    uint8_t wire[SP5_CONSOLE_WEB_WIRE_BYTES];
    size_t used, sent, size;
} sp5_console_web_connection;

typedef struct {
    sp5_web *web;
    int listener;
    /* stopping: acknowledgement flushed; stopped: finish_stop completed.
     * failed reports an adapter write/close failure; cleanup_error retains the
     * first finish_stop failure while later steps retry ownership. */
    bool stopping, stopped, failed;
    sp5_reason cleanup_error;
    sp5_console_web_connection connections[SP5_CONSOLE_WEB_CONNECTIONS];
    uint8_t body[SP5_WEB_RESPONSE_LIMIT];
} sp5_console_web;

/* Nonblocking IPv4 listener. Port 0 selects an ephemeral port. Writes the
 * exact served authority ("address:port") into authority. Returns the
 * descriptor or -1. */
int sp5_console_web_listen(const char *ipv4, uint16_t port, char *authority, size_t capacity);
void sp5_console_web_init(sp5_console_web *service, sp5_web *web, int listener);
/* Writes SP5_CONSOLE_WEB_POLLS entries: listener, then each connection slot. */
void sp5_console_web_poll(const sp5_console_web *service, struct pollfd *fds);
/* Processes the entries written by sp5_console_web_poll after poll() returned.
 * Serves the core dispatch with each connection's actual peer. Once an
 * acknowledged stop has been flushed, retries sp5_web_finish_stop once per
 * step. Sets stopped only after success. */
void sp5_console_web_step(sp5_console_web *service, const struct pollfd *fds, uint64_t now_us);
void sp5_console_web_close(sp5_console_web *service);

#endif
