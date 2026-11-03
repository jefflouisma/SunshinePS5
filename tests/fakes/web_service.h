#ifndef SUNSHINE_TESTS_FAKES_WEB_SERVICE_H
#define SUNSHINE_TESTS_FAKES_WEB_SERVICE_H
/* POSIX FIXTURE transport for sp5_web, shared by the loopback management
 * fixture and the private-address fake host. It frames bounded requests,
 * dispatches through the real core router with the accepted socket's peer and
 * writes the core response head (web/assets.h). Not a console adapter. */
#include "web/server.h"
#include <netinet/in.h>
#include <poll.h>

enum {
    SP5_FAKE_WEB_CONNECTIONS = 8,
    SP5_FAKE_WEB_REQUEST_BYTES = 8192,
    SP5_FAKE_WEB_WIRE_BYTES = SP5_WEB_RESPONSE_LIMIT + 1024,
    SP5_FAKE_WEB_POLLS = SP5_FAKE_WEB_CONNECTIONS + 1
};
typedef struct {
    int fd;
    uint64_t deadline_us;
    char peer[INET_ADDRSTRLEN];
    uint8_t request[SP5_FAKE_WEB_REQUEST_BYTES], wire[SP5_FAKE_WEB_WIRE_BYTES];
    size_t used, sent, size;
} sp5_fake_web_connection;
typedef struct {
    sp5_web *web;
    int listener;
    /* Loopback browser fixture only: replaces 127.0.0.1 at this adapter
     * boundary. NULL keeps the actual accept() peer, as a console must. */
    const char *synthetic_peer;
    bool truncate_streams;
    /* stopping: acknowledged and flushing; stopped: finish_stop completed. */
    bool stopping, stopped, failed;
    /* First cleanup failure stays observable while later steps retry ownership. */
    sp5_reason cleanup_error;
    sp5_fake_web_connection connections[SP5_FAKE_WEB_CONNECTIONS];
    uint8_t body[SP5_WEB_RESPONSE_LIMIT];
} sp5_fake_web;
/* Nonblocking IPv4 listener; port 0 selects an ephemeral port. Writes the exact
 * served authority ("address:port"). Returns the descriptor or -1. */
int sp5_fake_web_listen(const char *ipv4, uint16_t port, char *authority, size_t capacity);
void sp5_fake_web_init(sp5_fake_web *service, sp5_web *web, int listener,
                       const char *synthetic_peer);
/* Writes SP5_FAKE_WEB_POLLS entries: the listener, then each connection slot. */
void sp5_fake_web_poll(const sp5_fake_web *service, struct pollfd *fds);
/* Processes the entries written by sp5_fake_web_poll after poll() returned.
 * busy answers 503 without routing while fixture-owned threads run. Once an
 * acknowledged stop has been flushed, retries sp5_web_finish_stop once per step.
 * Sets stopped only after success; cleanup_error retains the first failure. */
void sp5_fake_web_step(sp5_fake_web *service, const struct pollfd *fds, uint64_t now_us, bool busy);
void sp5_fake_web_close(sp5_fake_web *service);
#endif
