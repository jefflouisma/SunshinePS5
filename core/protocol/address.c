#include "protocol.h"

#include <arpa/inet.h>
#include <string.h>

static bool private_v4(const uint8_t *address)
{
    return address[0] == 10 || (address[0] == 172 && (address[1] & 0xf0) == 16) ||
           (address[0] == 192 && address[1] == 168);
}

bool sp5_peer_permitted(const char *peer, bool on_link)
{
    if (peer == NULL) {
        return false;
    }
    uint8_t address[16];
    if (inet_pton(AF_INET, peer, address) == 1) {
        return private_v4(address);
    }
    if (inet_pton(AF_INET6, peer, address) != 1) {
        return false;
    }
    /* FR-040/R9: mapped peers have the underlying IPv4 policy. Neither
     * transport family nor receiving-interface locality grants an exemption. */
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (memcmp(address, mapped, sizeof(mapped)) == 0) {
        return private_v4(address + sizeof(mapped));
    }
    return (address[0] & 0xfe) == 0xfc ||
           (on_link && address[0] == 0xfe && (address[1] & 0xc0) == 0x80);
}
