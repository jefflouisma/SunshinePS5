/* Deterministic regression for the pinned ENet timeout guard. A silent
 * loopback UDP sink receives genuine CONNECT retransmissions. Only the public
 * outgoing-command counter and ENet's test clock are primed to boundary values;
 * enet_host_service executes the real patched timeout path. No hardware claim. */
#include <arpa/inet.h>
#include <enet/enet.h>
#include <stdio.h>
#include <sys/socket.h>
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            if (fputs(#c "\n", stderr) == EOF)                                                     \
                return 2;                                                                          \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static int attempts(enet_uint16 count)
{
    ENetAddress address = {0};
    struct sockaddr_in bind_address = {.sin_family = AF_INET};
    CHECK(inet_pton(AF_INET, "127.0.0.1", &bind_address.sin_addr) == 1);
    CHECK(enet_address_set_address(&address, (struct sockaddr *)&bind_address,
                                   sizeof(bind_address)) == 0);
    ENetHost *sink = enet_host_create(AF_INET, &address, 1, 48, 0, 0);
    CHECK(sink != NULL);
    ENetHost *host = enet_host_create(AF_INET, NULL, 1, 48, 0, 0);
    CHECK(host != NULL);
    enet_time_set(1000);
    ENetPeer *peer = enet_host_connect(host, &sink->address, 48, 0);
    CHECK(peer != NULL);
    enet_peer_timeout(peer, 2, 10000, 10000);
    enet_host_flush(host);
    CHECK(!enet_list_empty(&peer->sentReliableCommands));
    ENetOutgoingCommand *command =
        (ENetOutgoingCommand *)enet_list_front(&peer->sentReliableCommands);
    command->sendAttempts = count;
    command->sentTime = 1000;
    command->roundTripTimeout = 1;
    peer->nextTimeout = 1001;
    peer->earliestTimeout = 1000;
    enet_time_set(1002);
    ENetEvent event;
    CHECK(enet_host_service(host, &event, 0) >= 0);
    CHECK(peer->state != ENET_PEER_STATE_DISCONNECTED);
    enet_time_set(11003);
    CHECK(enet_host_service(host, &event, 0) >= 0);
    CHECK(peer->state == ENET_PEER_STATE_DISCONNECTED);
    enet_host_destroy(host);
    enet_host_destroy(sink);
    return 0;
}
int main(void)
{
    CHECK(enet_initialize() == 0);
    CHECK(attempts(0) == 0);
    CHECK(attempts(1) == 0);
    CHECK(attempts(32) == 0);
    CHECK(attempts(33) == 0);
    CHECK(attempts(65535) == 0);
    enet_deinitialize();
    return 0;
}
