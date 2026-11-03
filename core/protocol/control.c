#include "control.h"

#include <arpa/inet.h>
#include <enet/enet.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Protocol constants: notes §§9/11, mcc 41cedb8 ControlStream.c:203–298,
 * 548–768, 1403–1592; Limelight-internal.h:47–65; Sunshine d0b4a88
 * stream.cpp:451–459,583–631,1178–1315. Gen7 encrypted CONTROL_V2 only;
 * legacy 8-bit IVs are refused during negotiation, before activation. */
enum {
    REPLAY_BITS = 1024,
    SMALL_BLOCKS = 512,
    SMALL_SIZE = 2048,
    LARGE_BLOCKS = 8,
    LARGE_SIZE = 65536
};
typedef union {
    max_align_t alignment;
    uint8_t bytes[SMALL_SIZE];
} small_block;
typedef union {
    max_align_t alignment;
    uint8_t bytes[LARGE_SIZE];
} large_block;
struct sp5_control_runtime {
    small_block small[SMALL_BLOCKS];
    large_block large[LARGE_BLOCKS];
    bool small_used[SMALL_BLOCKS], large_used[LARGE_BLOCKS];
    sp5_control_pool_view view;
    size_t transports;
};
static sp5_control_runtime *allocator;
static void *pool_allocate(size_t size)
{
    if (allocator == NULL)
        return NULL;
    if (size <= SMALL_SIZE)
        for (size_t i = 0; i < SMALL_BLOCKS; ++i)
            if (!allocator->small_used[i]) {
                allocator->small_used[i] = true;
                ++allocator->view.used_blocks;
                if (allocator->view.used_blocks > allocator->view.high_water)
                    allocator->view.high_water = allocator->view.used_blocks;
                return allocator->small[i].bytes;
            }
    if (size <= LARGE_SIZE)
        for (size_t i = 0; i < LARGE_BLOCKS; ++i)
            if (!allocator->large_used[i]) {
                allocator->large_used[i] = true;
                ++allocator->view.used_blocks;
                if (allocator->view.used_blocks > allocator->view.high_water)
                    allocator->view.high_water = allocator->view.used_blocks;
                return allocator->large[i].bytes;
            }
    return NULL;
}
static void pool_free(void *pointer)
{
    if (allocator == NULL || pointer == NULL)
        return;
    for (size_t i = 0; i < SMALL_BLOCKS; ++i)
        if (pointer == allocator->small[i].bytes && allocator->small_used[i]) {
            OPENSSL_cleanse(allocator->small[i].bytes, SMALL_SIZE);
            allocator->small_used[i] = false;
            --allocator->view.used_blocks;
            return;
        }
    for (size_t i = 0; i < LARGE_BLOCKS; ++i)
        if (pointer == allocator->large[i].bytes && allocator->large_used[i]) {
            OPENSSL_cleanse(allocator->large[i].bytes, LARGE_SIZE);
            allocator->large_used[i] = false;
            --allocator->view.used_blocks;
            return;
        }
}
static void pool_exhausted(void)
{
    if (allocator != NULL && allocator->view.exhausted != UINT64_MAX)
        ++allocator->view.exhausted;
}
sp5_control_runtime *sp5_control_runtime_create(void)
{
    if (allocator != NULL)
        return NULL;
    sp5_control_runtime *runtime = calloc(1, sizeof(*runtime));
    if (runtime == NULL)
        return NULL;
    allocator = runtime;
    ENetCallbacks callbacks = {pool_allocate, pool_free, pool_exhausted};
    if (enet_initialize_with_callbacks(ENET_VERSION, &callbacks) != 0) {
        allocator = NULL;
        free(runtime);
        return NULL;
    }
    return runtime;
}
sp5_result sp5_control_runtime_destroy(sp5_control_runtime *runtime)
{
    if (runtime == NULL || runtime != allocator)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (runtime->transports != 0 || runtime->view.used_blocks != 0)
        return sp5_failure(SP5_REASON_OWNERSHIP);
    enet_deinitialize();
    allocator = NULL;
    OPENSSL_cleanse(runtime, sizeof(*runtime));
    free(runtime);
    return sp5_success();
}
sp5_control_pool_view sp5_control_pool_read(const sp5_control_runtime *runtime)
{
    return runtime == NULL ? (sp5_control_pool_view){0} : runtime->view;
}

typedef struct {
    uint8_t data[SP5_CONTROL_WIRE_LIMIT];
    size_t size;
} outgoing;
struct sp5_control {
    sp5_control_config config;
    uint8_t certificate[SP5_CERTIFICATE_CAPACITY];
    char peer[128];
    EVP_CIPHER_CTX *encrypt, *decrypt;
    uint64_t replay[REPLAY_BITS / 64], tx_sequence, last_contact;
    uint32_t rx_highest;
    bool have_rx, connected, ended;
    sp5_reason diagnostic;
    sp5_control_event events[SP5_CONTROL_QUEUE_CAPACITY];
    size_t event_head, event_count;
    outgoing outgoing[SP5_CONTROL_QUEUE_CAPACITY];
    size_t out_head, out_count;
    uint8_t plaintext[SP5_CONTROL_PAYLOAD_LIMIT + 4];
};
static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put16(uint8_t *p, uint16_t n)
{
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
}
static void put32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(n >> (8 * i));
}
static void nonce(uint8_t iv[12], uint32_t sequence, bool host)
{
    memset(iv, 0, 12);
    put32(iv, sequence);
    iv[10] = host ? 'H' : 'C';
    iv[11] = 'C';
}
sp5_control *sp5_control_create(const sp5_control_config *config)
{
    if (config == NULL || config->session == NULL || config->release_input == NULL ||
        config->peer == NULL || strlen(config->peer) >= 128 ||
        !sp5_peer_permitted(config->peer, config->on_link) ||
        config->verified_certificate.size > SP5_CERTIFICATE_CAPACITY ||
        !sp5_session_authorized(config->session, config->verified_certificate) ||
        sp5_session_read(config->session).id != config->launch.session_id ||
        (config->negotiated.encryption_flags & 1U) == 0 ||
        (config->negotiated.encryption_flags & ~7U) != 0)
        return NULL;
    sp5_control *c = calloc(1, sizeof(*c));
    if (c == NULL)
        return NULL;
    c->config = *config;
    memcpy(c->certificate, config->verified_certificate.data, config->verified_certificate.size);
    c->config.verified_certificate = (sp5_bytes){c->certificate, config->verified_certificate.size};
    memcpy(c->peer, config->peer, strlen(config->peer) + 1);
    c->config.peer = c->peer;
    c->last_contact = config->started_us;
    c->encrypt = EVP_CIPHER_CTX_new();
    c->decrypt = EVP_CIPHER_CTX_new();
    uint8_t iv[12];
    nonce(iv, 0, true);
    if (c->encrypt == NULL || c->decrypt == NULL ||
        EVP_EncryptInit_ex(c->encrypt, EVP_aes_128_gcm(), NULL, config->launch.key, iv) != 1 ||
        EVP_DecryptInit_ex(c->decrypt, EVP_aes_128_gcm(), NULL, config->launch.key, iv) != 1) {
        EVP_CIPHER_CTX_free(c->encrypt);
        EVP_CIPHER_CTX_free(c->decrypt);
        OPENSSL_cleanse(c, sizeof(*c));
        free(c);
        return NULL;
    }
    return c;
}
static sp5_result finish(sp5_control *c, sp5_end_reason reason, sp5_reason diagnostic)
{
    if (c->ended)
        return sp5_success();
    c->ended = true;
    c->connected = false;
    c->diagnostic = diagnostic;
    c->event_count = 0;
    c->out_count = 0;
    /* Independent input owner: atomic claim only (session.h). The serialized
     * management owner records the retained reason in snapshot and history. */
    sp5_result admission = sp5_session_claim_end(
        c->config.session,
        (sp5_session_end_record){c->config.launch.session_id, reason, diagnostic});
    if (admission.status != SP5_OK && admission.reason == SP5_REASON_OWNERSHIP) {
        c->diagnostic = SP5_REASON_SOURCE_CHANGED;
        return admission;
    }
    sp5_result released = c->config.release_input(c->config.end_context, reason);
    if (released.status != SP5_OK)
        c->diagnostic = released.reason;
    OPENSSL_cleanse(c->config.launch.key, sizeof(c->config.launch.key));
    return released.status != SP5_OK ? released : admission;
}
sp5_result sp5_control_end(sp5_control *c, sp5_end_reason reason)
{
    if (c == NULL || reason > SP5_END_ERROR)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    return finish(c, reason, reason == SP5_END_ERROR ? SP5_REASON_PLATFORM : SP5_REASON_NONE);
}
void sp5_control_destroy(sp5_control *c)
{
    if (c != NULL) {
        (void)finish(c, SP5_END_HOST_STOPPING, SP5_REASON_NONE);
        EVP_CIPHER_CTX_free(c->encrypt);
        EVP_CIPHER_CTX_free(c->decrypt);
        OPENSSL_cleanse(c, sizeof(*c));
        free(c);
    }
}
static sp5_result authorized(sp5_control *c)
{
    if (c == NULL || c->ended)
        return sp5_failure(SP5_REASON_OWNERSHIP);
    /* Immutable lease/ID and atomic state only; never a management snapshot. */
    if (!sp5_session_authorized_id(c->config.session, c->config.verified_certificate,
                                   c->config.launch.session_id)) {
        sp5_end_reason reason = SP5_END_REVOKED;
        (void)sp5_session_end_claimed(c->config.session, &reason);
        (void)finish(c, reason, SP5_REASON_OWNERSHIP);
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    return sp5_success();
}
sp5_result sp5_control_tick(sp5_control *c, uint64_t now_us)
{
    sp5_result result = authorized(c);
    if (result.status != SP5_OK)
        return result;
    if (now_us < c->last_contact) {
        (void)finish(c, SP5_END_ERROR, SP5_REASON_CLOCK);
        return sp5_failure(SP5_REASON_CLOCK);
    }
    if (now_us - c->last_contact >= UINT64_C(10000000)) {
        (void)finish(c, SP5_END_CLIENT_LOST, SP5_REASON_TIMEOUT);
        return sp5_failure(SP5_REASON_TIMEOUT);
    }
    return sp5_success();
}
sp5_result sp5_control_accept(sp5_control *c, const char *peer, bool on_link, uint32_t connect_data,
                              uint64_t now_us)
{
    const struct {
        uint32_t token;
        uint64_t at_us;
    } attempt = {connect_data, now_us};
    sp5_result result = sp5_control_tick(c, attempt.at_us);
    if (result.status != SP5_OK)
        return result;
    if (c->connected || peer == NULL || !sp5_peer_permitted(peer, on_link) ||
        strcmp(peer, c->peer) != 0 ||
        ((c->config.negotiated.feature_flags & 2U) != 0 &&
         attempt.token != c->config.launch.control_connect_data))
        return sp5_failure(SP5_REASON_OWNERSHIP);
    c->connected = true;
    return sp5_success();
}
static bool fresh(const sp5_control *c, uint32_t sequence)
{
    if (!c->have_rx || sequence > c->rx_highest)
        return true;
    uint32_t distance = c->rx_highest - sequence;
    return distance < REPLAY_BITS &&
           (c->replay[distance / 64] & (UINT64_C(1) << (distance % 64))) == 0;
}
static void remember(sp5_control *c, uint32_t sequence)
{
    if (!c->have_rx) {
        c->have_rx = true;
        c->rx_highest = sequence;
        c->replay[0] = 1;
        return;
    }
    if (sequence > c->rx_highest) {
        uint32_t delta = sequence - c->rx_highest;
        for (size_t i = REPLAY_BITS / 64; i-- > 0;) {
            uint64_t bits = 0;
            if (delta < REPLAY_BITS && i >= delta / 64) {
                bits = c->replay[i - delta / 64] << (delta % 64);
                if (delta % 64 != 0 && i > delta / 64)
                    bits |= c->replay[i - delta / 64 - 1] >> (64 - delta % 64);
            }
            c->replay[i] = bits;
        }
        c->rx_highest = sequence;
        c->replay[0] |= 1;
    } else {
        uint32_t distance = c->rx_highest - sequence;
        c->replay[distance / 64] |= UINT64_C(1) << (distance % 64);
    }
}
sp5_result sp5_control_fec_read(const sp5_control_event *event, sp5_control_fec *feedback)
{
    if (feedback == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    *feedback = (sp5_control_fec){0};
    if (event == NULL || event->type != 0x5502 || event->payload_size != 21)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    const uint8_t *p = event->payload;
    /* SS_FRAME_FEC_STATUS: mcc Video.h:56–70 @41cedb8, packed 21 bytes,
     * big-endian (notes §7). Direction-specific: not host RGB LED 0x5502. */
    sp5_control_fec next = {be32(p),      be16(p + 4),  be16(p + 6),  be16(p + 8),
                            be16(p + 10), be16(p + 12), be16(p + 14), be16(p + 16),
                            p[18],        p[19],        p[20]};
    if (next.data_total == 0 || next.data_received > next.data_total ||
        next.parity_received > next.parity_total || next.fec_percent > 100 ||
        next.block_count == 0 || next.block_count > 4 || next.block_index >= next.block_count)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    *feedback = next;
    return sp5_success();
}
static bool valid_event(const sp5_control_event *e)
{
    bool reliable = e->delivery == SP5_CONTROL_RELIABLE;
    bool generic = e->channel == 0, urgent = generic || e->channel == 1;
    switch (e->type) {
    case 0x0200:
        return generic && reliable && e->payload_size == 8 && le32(e->payload) == 4 &&
               le32(e->payload + 4) == 0;
    case 0x0307:
        return generic && reliable && e->payload_size == 1 && e->payload[0] == 0;
    case 0x0302:
        return urgent && reliable && e->payload_size == 2 && le16(e->payload) == 0;
    case 0x0301:
        return urgent && reliable && e->payload_size == 24 &&
               le32(e->payload) <= le32(e->payload + 8) && le32(e->payload + 4) == 0 &&
               le32(e->payload + 12) == 0 && le32(e->payload + 16) == 0 &&
               le32(e->payload + 20) == 0;
    case 0x0350:
        return urgent && reliable && e->payload_size == 8 && le32(e->payload + 4) == 0;
    case 0x5502: {
        sp5_control_fec feedback;
        return generic && e->delivery == SP5_CONTROL_UNSEQUENCED &&
               sp5_control_fec_read(e, &feedback).status == SP5_OK;
    }
    case 0x0201:
        return generic && reliable && e->payload_size == 32;
    case 0x0206:
        /* mcc 41cedb8 Limelight-internal.h:56-65: sixteen wire IDs,
         * GAMEPAD_BASE=0x10, SENSOR_BASE=0x20, COUNT=0x30. */
        return (generic || (e->channel >= 16 && e->channel < 32) ||
                (e->channel >= 32 && e->channel < 48)) &&
               e->payload_size >= 8;
    default:
        return false;
    }
}
sp5_result sp5_control_receive(sp5_control *c, const char *peer, bool on_link, uint8_t channel,
                               sp5_control_delivery delivery, sp5_bytes wire, uint64_t now_us)
{
    sp5_result result = sp5_control_tick(c, now_us);
    if (result.status != SP5_OK)
        return result;
    if (!c->connected || peer == NULL || strcmp(peer, c->peer) != 0 ||
        !sp5_peer_permitted(peer, on_link))
        return sp5_failure(SP5_REASON_OWNERSHIP);
    if (wire.data == NULL || wire.size < 28 || wire.size > SP5_CONTROL_WIRE_LIMIT ||
        le16(wire.data) != 1 || le16(wire.data + 2) != wire.size - 4 ||
        channel >= SP5_CONTROL_CHANNEL_COUNT || delivery > SP5_CONTROL_UNSEQUENCED)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    uint32_t seq = le32(wire.data + 4);
    if (!fresh(c, seq))
        return sp5_failure(SP5_REASON_OWNERSHIP);
    uint8_t iv[12];
    nonce(iv, seq, false);
    int n = 0, tail = 0;
    if (EVP_DecryptInit_ex(c->decrypt, NULL, NULL, NULL, iv) != 1 ||
        EVP_DecryptUpdate(c->decrypt, c->plaintext, &n, wire.data + 24, (int)(wire.size - 24)) !=
            1 ||
        EVP_CIPHER_CTX_ctrl(c->decrypt, EVP_CTRL_GCM_SET_TAG, 16, (void *)(wire.data + 8)) != 1 ||
        EVP_DecryptFinal_ex(c->decrypt, c->plaintext + n, &tail) != 1) {
        OPENSSL_cleanse(c->plaintext, sizeof(c->plaintext));
        (void)finish(c, SP5_END_ERROR, SP5_REASON_OWNERSHIP);
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    if ((size_t)n + (size_t)tail != wire.size - 24 || le16(c->plaintext + 2) != wire.size - 28)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    sp5_control_event event = {.type = le16(c->plaintext),
                               .channel = channel,
                               .delivery = delivery,
                               .sequence = seq,
                               .received_us = now_us,
                               .payload_size = wire.size - 28};
    if (event.payload_size != 0)
        memcpy(event.payload, c->plaintext + 4, event.payload_size);
    OPENSSL_cleanse(c->plaintext, sizeof(c->plaintext));
    if (!valid_event(&event))
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    /* Recheck immediately before publishing input/feedback. Already queued
     * events are checked again at take(), so revocation cannot revive them. */
    result = authorized(c);
    if (result.status != SP5_OK)
        return result;
    if (event.type != 0x0200 && event.type != 0x0307) {
        if (c->event_count == SP5_CONTROL_QUEUE_CAPACITY) {
            (void)finish(c, SP5_END_ERROR, SP5_REASON_CAPACITY);
            return sp5_failure(SP5_REASON_CAPACITY);
        }
        c->events[(c->event_head + c->event_count) % SP5_CONTROL_QUEUE_CAPACITY] = event;
        ++c->event_count;
    }
    remember(c, seq);
    c->last_contact = now_us;
    return sp5_success();
}
sp5_result sp5_control_take(sp5_control *c, sp5_control_event *event)
{
    if (event == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    *event = (sp5_control_event){0};
    sp5_result result = authorized(c);
    if (result.status != SP5_OK)
        return result;
    if (c->event_count == 0)
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    *event = c->events[c->event_head];
    c->event_head = (c->event_head + 1) % SP5_CONTROL_QUEUE_CAPACITY;
    --c->event_count;
    return sp5_success();
}
static bool valid_host(uint16_t type, sp5_bytes payload)
{
    if (payload.data == NULL)
        return false;
    switch (type) {
    case 0x0109:
        return payload.size == 4;
    case 0x010b:
        return payload.size == 10;
    case 0x010e:
        return payload.size == 27;
    case 0x5500:
        return payload.size == 6;
    case 0x5501:
    case 0x5502:
        return payload.size == 5;
    case 0x5503:
        return payload.size == 25;
    case 0x5504:
        return payload.size == 4;
    default:
        return false;
    }
}
sp5_result sp5_control_send(sp5_control *c, uint16_t type, sp5_bytes payload)
{
    sp5_result result = authorized(c);
    if (result.status != SP5_OK)
        return result;
    if (!c->connected || !valid_host(type, payload))
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (c->out_count == SP5_CONTROL_QUEUE_CAPACITY)
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    if (c->tx_sequence > UINT32_MAX) {
        (void)finish(c, SP5_END_ERROR, SP5_REASON_OVERFLOW);
        return sp5_failure(SP5_REASON_OVERFLOW);
    }
    outgoing *out = &c->outgoing[(c->out_head + c->out_count) % SP5_CONTROL_QUEUE_CAPACITY];
    uint32_t seq = (uint32_t)c->tx_sequence++;
    uint8_t iv[12];
    nonce(iv, seq, true);
    uint8_t plain[SP5_CONTROL_PAYLOAD_LIMIT + 4];
    put16(plain, type);
    put16(plain + 2, (uint16_t)payload.size);
    memcpy(plain + 4, payload.data, payload.size);
    put16(out->data, 1);
    put16(out->data + 2, (uint16_t)(payload.size + 24));
    put32(out->data + 4, seq);
    int n = 0, tail = 0;
    bool ok =
        EVP_EncryptInit_ex(c->encrypt, NULL, NULL, NULL, iv) == 1 &&
        EVP_EncryptUpdate(c->encrypt, out->data + 24, &n, plain, (int)payload.size + 4) == 1 &&
        EVP_EncryptFinal_ex(c->encrypt, out->data + 24 + n, &tail) == 1 &&
        EVP_CIPHER_CTX_ctrl(c->encrypt, EVP_CTRL_GCM_GET_TAG, 16, out->data + 8) == 1;
    OPENSSL_cleanse(plain, sizeof(plain));
    if (!ok) {
        (void)finish(c, SP5_END_ERROR, SP5_REASON_PLATFORM);
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    out->size = 24 + (size_t)n + (size_t)tail;
    ++c->out_count;
    return sp5_success();
}
sp5_result sp5_control_take_wire(sp5_control *c, sp5_buffer *wire)
{
    sp5_result result = authorized(c);
    if (result.status != SP5_OK)
        return result;
    if (c->out_count == 0)
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    outgoing *out = &c->outgoing[c->out_head];
    result = sp5_buffer_append(wire, (sp5_bytes){out->data, out->size});
    if (result.status == SP5_OK) {
        OPENSSL_cleanse(out, sizeof(*out));
        c->out_head = (c->out_head + 1) % SP5_CONTROL_QUEUE_CAPACITY;
        --c->out_count;
    }
    return result;
}
sp5_reason sp5_control_diagnostic(const sp5_control *c)
{
    return c == NULL ? SP5_REASON_INVALID_ARGUMENT : c->diagnostic;
}

struct sp5_control_transport {
    sp5_control_runtime *runtime;
    sp5_control *control;
    ENetHost *host;
    ENetPeer *peer;
    enet_uint32 last_peer_receive;
};
static bool address(ENetAddress *out, const char *text, uint16_t port)
{
    struct sockaddr_in v4 = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (inet_pton(AF_INET, text, &v4.sin_addr) == 1)
        return enet_address_set_address(out, (struct sockaddr *)&v4, sizeof(v4)) == 0;
    struct sockaddr_in6 v6 = {.sin6_family = AF_INET6, .sin6_port = htons(port)};
    return inet_pton(AF_INET6, text, &v6.sin6_addr) == 1 &&
           enet_address_set_address(out, (struct sockaddr *)&v6, sizeof(v6)) == 0;
}
static bool peer_text(const ENetAddress *a, char text[128])
{
    if (a->address.ss_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)&a->address;
        return inet_ntop(AF_INET, &v4->sin_addr, text, 128) != NULL;
    }
    if (a->address.ss_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)&a->address;
        return inet_ntop(AF_INET6, &v6->sin6_addr, text, 128) != NULL;
    }
    return false;
}
sp5_control_transport *sp5_control_transport_open(sp5_control_runtime *runtime,
                                                  sp5_control *control, const char *bind_address,
                                                  uint16_t port)
{
    if (runtime == NULL || runtime != allocator || control == NULL ||
        authorized(control).status != SP5_OK || bind_address == NULL ||
        !sp5_peer_permitted(bind_address, true))
        return NULL;
    ENetAddress bind;
    if (!address(&bind, bind_address, port))
        return NULL;
    sp5_control_transport *t = calloc(1, sizeof(*t));
    if (t == NULL)
        return NULL;
    /* Two bounded peers let an unauthenticated connect be refused without
     * replacing the active one. Source notes §11, enet@aca8784 enet.h/host.c. */
    t->host = enet_host_create(bind.address.ss_family, &bind, 2, SP5_CONTROL_CHANNEL_COUNT, 0, 0);
    if (t->host == NULL) {
        free(t);
        return NULL;
    }
    t->host->maximumPacketSize = SP5_CONTROL_WIRE_LIMIT;
    t->host->maximumWaitingData = 65536;
    t->runtime = runtime;
    t->control = control;
    ++runtime->transports;
    return t;
}
uint16_t sp5_control_transport_port(const sp5_control_transport *t)
{
    if (t == NULL)
        return 0;
    if (t->host->address.address.ss_family == AF_INET)
        return ntohs(((const struct sockaddr_in *)&t->host->address.address)->sin_port);
    if (t->host->address.address.ss_family == AF_INET6)
        return ntohs(((const struct sockaddr_in6 *)&t->host->address.address)->sin6_port);
    return 0;
}
sp5_result sp5_control_transport_step(sp5_control_transport *t, uint64_t now_us)
{
    if (t == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    sp5_result result = sp5_control_tick(t->control, now_us);
    if (result.status != SP5_OK) {
        if (t->peer != NULL) {
            enet_peer_disconnect_now(t->peer, 0);
            t->peer = NULL;
        }
        return result;
    }
    uint64_t exhausted = t->runtime->view.exhausted;
    /* Bounded event drain. No wait: input dispatch is independent of media. */
    for (unsigned i = 0; i < SP5_CONTROL_QUEUE_CAPACITY; ++i) {
        ENetEvent event;
        int received = enet_host_service(t->host, &event, 0);
        if (received < 0) {
            (void)finish(t->control, SP5_END_ERROR, SP5_REASON_PLATFORM);
            return sp5_failure(SP5_REASON_PLATFORM);
        }
        if (received == 0)
            break;
        char actual_peer[128];
        bool actual = peer_text(&event.peer->address, actual_peer);
        if (event.type == ENET_EVENT_TYPE_CONNECT) {
            if (!actual || t->peer != NULL ||
                sp5_control_accept(t->control, actual_peer, t->control->config.on_link, event.data,
                                   now_us)
                        .status != SP5_OK)
                enet_peer_disconnect_now(event.peer, 0);
            else {
                t->peer = event.peer;
                t->last_peer_receive = event.peer->lastReceiveTime;
                enet_peer_timeout(t->peer, 2, 10000, 10000);
                enet_peer_ping_interval(t->peer, 500);
            }
        } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            if (actual && event.peer == t->peer) {
                sp5_control_delivery delivery =
                    (event.packet->flags & ENET_PACKET_FLAG_RELIABLE) != 0 ? SP5_CONTROL_RELIABLE
                    : (event.packet->flags & ENET_PACKET_FLAG_UNSEQUENCED) != 0
                        ? SP5_CONTROL_UNSEQUENCED
                        : SP5_CONTROL_UNRELIABLE;
                result = sp5_control_receive(
                    t->control, actual_peer, t->control->config.on_link, event.channelID, delivery,
                    (sp5_bytes){event.packet->data, event.packet->dataLength}, now_us);
            }
            enet_packet_destroy(event.packet);
            if (t->control->ended) {
                if (t->peer != NULL) {
                    enet_peer_disconnect_now(t->peer, 0);
                    t->peer = NULL;
                }
                return result;
            }
        } else if (event.type == ENET_EVENT_TYPE_DISCONNECT && event.peer == t->peer) {
            /* Only a matched graceful DISCONNECT means client_quit; the
             * authenticated-contact deadline above detects silent loss first. */
            (void)finish(t->control, SP5_END_CLIENT_QUIT, SP5_REASON_NONE);
            t->peer = NULL;
            return sp5_success();
        }
    }
    /* ENet ping/ACK traffic is contact even when the client sends no input.
     * Without this, the 10-second input-contact deadline drops an idle
     * official Moonlight session while its control stream is healthy. */
    if (t->peer != NULL && t->peer->lastReceiveTime != t->last_peer_receive) {
        t->last_peer_receive = t->peer->lastReceiveTime;
        t->control->last_contact = now_us;
    }
    for (unsigned i = 0; i < SP5_CONTROL_QUEUE_CAPACITY && t->peer != NULL; ++i) {
        uint8_t data[SP5_CONTROL_WIRE_LIMIT];
        sp5_buffer wire = {data, sizeof(data), 0};
        result = sp5_control_take_wire(t->control, &wire);
        if (result.status != SP5_OK)
            break;
        ENetPacket *packet = enet_packet_create(wire.data, wire.size, ENET_PACKET_FLAG_RELIABLE);
        if (packet == NULL || enet_peer_send(t->peer, 0, packet) != 0) {
            if (packet != NULL)
                enet_packet_destroy(packet);
            (void)finish(t->control, SP5_END_ERROR, SP5_REASON_CAPACITY);
            return sp5_failure(SP5_REASON_CAPACITY);
        }
    }
    enet_host_flush(t->host);
    if (t->runtime->view.exhausted != exhausted) {
        (void)finish(t->control, SP5_END_ERROR, SP5_REASON_CAPACITY);
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    return sp5_success();
}
void sp5_control_transport_close(sp5_control_transport *t)
{
    if (t != NULL) {
        (void)finish(t->control, SP5_END_HOST_STOPPING, SP5_REASON_NONE);
        if (t->peer != NULL)
            enet_peer_disconnect_now(t->peer, 0);
        enet_host_destroy(t->host);
        --t->runtime->transports;
        free(t);
    }
}
