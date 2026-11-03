/* T111 synthetic harness: the authenticated control channel as a bounded
 * operation stream on one connection. No ENet socket, console adapter or real
 * client; release_input is a counting stub and the GCM key is the all-zero
 * test key of the fixture launch.
 *
 * Input: [u8 setup] then up to OPERATION_LIMIT operations, each [u8 op] ...
 *   0 receive [u32 seq][u8 channel][u8 delivery][u16 type][u16 length][payload]
 *     [u8 flags]: sealed with its own sequence (protocol-notes.md §9) unless
 *     flags bit0 sends the bytes raw; bit1 corrupts the tag; bit2 corrupts the
 *     inner length; bit3 pads a raw wire past SP5_CONTROL_WIRE_LIMIT; bit4 uses
 *     a different peer address.
 *   1 tick [u16 milliseconds x 100] (crosses the ten-second contact deadline)
 *   2 take up to [u8] queued events      3 send [u16 type][u16 length][payload]
 *   4 end [u8 reason]                    5 accept [u32 token][u8 peer]
 *   6 revoke the paired device
 * setup bit0 skips the initial accept. */
#include "protocol/control.h"
#include "harness.h"
#include <openssl/evp.h>
#include <string.h>

enum { OPERATION_LIMIT = 48, INPUT_LIMIT = 16384, ACCEPTED_LIMIT = OPERATION_LIMIT };

static sp5_result missing(void *context, sp5_store_file file, sp5_buffer *output)
{
    (void)context;
    (void)file;
    (void)output;
    return sp5_failure(SP5_REASON_WOULD_BLOCK);
}
static sp5_result replace(void *context, sp5_store_file file, sp5_bytes contents)
{
    (void)context;
    (void)file;
    (void)contents;
    return sp5_success();
}
static sp5_result release(void *context, sp5_end_reason reason)
{
    unsigned *calls = context;
    fuzz_require(reason <= SP5_END_ERROR);
    ++*calls;
    return sp5_success();
}

typedef struct {
    const uint8_t *data;
    size_t size, at;
} reader;
static bool take_bytes(reader *r, size_t size, const uint8_t **out)
{
    if (r->size - r->at < size) {
        return false;
    }
    *out = r->data + r->at;
    r->at += size;
    return true;
}
static bool u8(reader *r, uint8_t *value)
{
    const uint8_t *p = NULL;
    if (!take_bytes(r, 1, &p)) {
        return false;
    }
    *value = p[0];
    return true;
}
static bool u16(reader *r, uint16_t *value)
{
    const uint8_t *p = NULL;
    if (!take_bytes(r, 2, &p)) {
        return false;
    }
    *value = (uint16_t)(p[0] | (p[1] << 8));
    return true;
}
static bool u32(reader *r, uint32_t *value)
{
    const uint8_t *p = NULL;
    if (!take_bytes(r, 4, &p)) {
        return false;
    }
    *value =
        (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return true;
}

typedef struct {
    sp5_devices *devices;
    sp5_authority *authority;
    sp5_session *session;
    sp5_control *control;
    sp5_bytes certificate;
    unsigned releases;
    uint64_t now;
    /* Shadow model: no sequence is accepted twice, the queue never exceeds its
     * capacity and nothing is published after an explicit end or revoke. */
    uint32_t accepted[ACCEPTED_LIMIT];
    size_t accepted_count, queued;
    bool closed;
} channel;

/* Wire: le16 1, le16 (size-4), le32 seq, 16-byte tag, GCM(le16 type, le16 len,
 * payload); nonce le32(seq) .. 'C' 'C'. */
typedef struct {
    uint32_t sequence;
    uint16_t type;
    sp5_bytes payload;
    uint8_t flags;
} message;
static size_t seal(message m, uint8_t *wire)
{
    const uint32_t sequence = m.sequence;
    const sp5_bytes payload = m.payload;
    uint8_t plain[SP5_CONTROL_PAYLOAD_LIMIT + 4];
    uint8_t iv[12] = {0};
    const uint8_t key[16] = {0};
    uint16_t inner = (uint16_t)payload.size;
    if ((m.flags & 4) != 0) {
        inner ^= 0x0101;
    }
    plain[0] = (uint8_t)m.type;
    plain[1] = (uint8_t)(m.type >> 8);
    plain[2] = (uint8_t)inner;
    plain[3] = (uint8_t)(inner >> 8);
    if (payload.size != 0) {
        memcpy(plain + 4, payload.data, payload.size);
    }
    size_t size = payload.size + 28;
    wire[0] = 1;
    wire[1] = 0;
    wire[2] = (uint8_t)(size - 4);
    wire[3] = (uint8_t)((size - 4) >> 8);
    for (unsigned i = 0; i < 4; ++i) {
        wire[4 + i] = (uint8_t)(sequence >> (8 * i));
        iv[i] = (uint8_t)(sequence >> (8 * i));
    }
    iv[10] = 'C';
    iv[11] = 'C';
    EVP_CIPHER_CTX *cipher = EVP_CIPHER_CTX_new();
    fuzz_require(cipher != NULL);
    int written = 0;
    int final = 0;
    fuzz_require(EVP_EncryptInit_ex(cipher, EVP_aes_128_gcm(), NULL, key, iv) == 1);
    fuzz_require(EVP_EncryptUpdate(cipher, wire + 24, &written, plain, (int)payload.size + 4) == 1);
    fuzz_require(EVP_EncryptFinal_ex(cipher, wire + 24 + written, &final) == 1);
    fuzz_require(EVP_CIPHER_CTX_ctrl(cipher, EVP_CTRL_GCM_GET_TAG, 16, wire + 8) == 1);
    EVP_CIPHER_CTX_free(cipher);
    fuzz_require((size_t)written + (size_t) final == payload.size + 4);
    if ((m.flags & 2) != 0) {
        wire[8] ^= 0x80;
    }
    return size;
}

static bool seen(const channel *c, uint32_t sequence)
{
    for (size_t i = 0; i < c->accepted_count; ++i) {
        if (c->accepted[i] == sequence) {
            return true;
        }
    }
    return false;
}

static void receive(channel *c, reader *r)
{
    uint32_t sequence = 0;
    uint8_t channel_id = 0;
    uint8_t delivery = 0;
    uint16_t type = 0;
    uint16_t length = 0;
    uint8_t flags = 0;
    const uint8_t *payload = NULL;
    if (!u32(r, &sequence) || !u8(r, &channel_id) || !u8(r, &delivery) || !u16(r, &type) ||
        !u16(r, &length) || !take_bytes(r, length, &payload) || !u8(r, &flags)) {
        return;
    }
    static uint8_t wire[SP5_CONTROL_WIRE_LIMIT + 64];
    size_t size = 0;
    bool sealed = (flags & 1) == 0 && length <= SP5_CONTROL_PAYLOAD_LIMIT;
    if (sealed) {
        size = seal((message){sequence, type, {payload, length}, flags}, wire);
    } else {
        size = length < sizeof(wire) ? length : sizeof(wire);
        memcpy(wire, payload, size);
        if ((flags & 8) != 0) {
            /* Oversized wire: bounded garbage past the documented maximum. */
            memset(wire + size, 0xa5, sizeof(wire) - size);
            size = sizeof(wire);
        }
    }
    const char *peer = (flags & 16) != 0 ? "10.1.2.4" : "10.1.2.3";
    sp5_result result =
        sp5_control_receive(c->control, peer, false, channel_id, (sp5_control_delivery)delivery,
                            (sp5_bytes){wire, size}, c->now);
    if (result.status != SP5_OK) {
        return;
    }
    /* Only an authentic, correctly framed, fresh message on the bound peer. */
    fuzz_require(!c->closed && sealed && (flags & 6) == 0 && (flags & 16) == 0);
    fuzz_require(channel_id < SP5_CONTROL_CHANNEL_COUNT && delivery <= SP5_CONTROL_UNSEQUENCED);
    fuzz_require(!seen(c, sequence));
    fuzz_require(c->accepted_count < ACCEPTED_LIMIT);
    c->accepted[c->accepted_count++] = sequence;
    if (type != 0x0200 && type != 0x0307) {
        ++c->queued;
        fuzz_require(c->queued <= SP5_CONTROL_QUEUE_CAPACITY);
    }
}

static void drain(channel *c, reader *r)
{
    uint8_t count = 0;
    if (!u8(r, &count)) {
        return;
    }
    for (unsigned i = 0; i < count && i <= SP5_CONTROL_QUEUE_CAPACITY; ++i) {
        sp5_control_event event;
        if (sp5_control_take(c->control, &event).status != SP5_OK) {
            fuzz_require(event.payload_size == 0);
            break;
        }
        fuzz_require(!c->closed && c->queued > 0);
        --c->queued;
        fuzz_require(event.payload_size <= SP5_CONTROL_PAYLOAD_LIMIT);
        fuzz_require(seen(c, event.sequence) && event.type != 0x0200 && event.type != 0x0307);
        sp5_control_fec fec;
        if (sp5_control_fec_read(&event, &fec).status == SP5_OK) {
            fuzz_require(event.type == 0x5502 && event.payload_size == 21);
        }
    }
}

static void send(channel *c, reader *r)
{
    uint16_t type = 0;
    uint16_t length = 0;
    const uint8_t *payload = NULL;
    if (!u16(r, &type) || !u16(r, &length) || length > SP5_CONTROL_PAYLOAD_LIMIT ||
        !take_bytes(r, length, &payload)) {
        return;
    }
    static const uint8_t empty[1];
    sp5_result sent =
        sp5_control_send(c->control, type, (sp5_bytes){payload ? payload : empty, length});
    fuzz_require(sent.status != SP5_OK || !c->closed);
    uint8_t data[SP5_CONTROL_WIRE_LIMIT];
    sp5_buffer wire = {data, sizeof(data), 0};
    sp5_result taken = sp5_control_take_wire(c->control, &wire);
    fuzz_require(wire.size <= wire.capacity);
    if (taken.status == SP5_OK) {
        fuzz_require(!c->closed && wire.size >= 28 && data[0] == 1 && data[1] == 0);
        fuzz_require((size_t)(data[2] | (data[3] << 8)) == wire.size - 4);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > INPUT_LIMIT) {
        return 0;
    }
    channel c = {.now = 100};
    sp5_storage_ops storage = {.read = missing, .replace = replace};
    c.devices = sp5_devices_create(&storage);
    fuzz_require(c.devices != NULL);
    c.authority = sp5_authority_create(c.devices);
    fuzz_require(c.authority != NULL);
    c.session = sp5_session_create(c.authority);
    fuzz_require(c.session != NULL);
    static const uint8_t identity[] = "synthetic-fuzz-client";
    c.certificate = (sp5_bytes){identity, sizeof(identity) - 1};
    fuzz_require(sp5_authority_pair(c.authority, c.certificate).status == SP5_OK);
    fuzz_require(sp5_session_admit(c.session, c.certificate, 1).status == SP5_OK);
    sp5_host host = {.supported_mode_count = 1,
                     .supported_modes = {{1280, 720, 60, "h264", SP5_SDR, 100, 1000}}};
    sp5_session_parameters parameters = {.mode_index = 0, .requested_kbps = 500};
    fuzz_require(sp5_session_configure(c.session, &host, &parameters).status == SP5_OK);
    fuzz_require(sp5_session_activate(c.session).status == SP5_OK);
    sp5_control_config config = {.session = c.session,
                                 .verified_certificate = c.certificate,
                                 .launch = {.session_id = 1, .control_connect_data = 42},
                                 .negotiated = {.encryption_flags = 1, .feature_flags = 3},
                                 .peer = "10.1.2.3",
                                 .started_us = 100,
                                 .end_context = &c.releases,
                                 .release_input = release};
    c.control = sp5_control_create(&config);
    fuzz_require(c.control != NULL);
    reader r = {data, size, 1};
    /* An unaccepted connection must never publish an event. */
    fuzz_require(sp5_control_receive(c.control, config.peer, false, 0, SP5_CONTROL_RELIABLE,
                                     (sp5_bytes){data, size}, c.now)
                     .status != SP5_OK);
    if ((data[0] & 1) == 0) {
        fuzz_require(sp5_control_accept(c.control, config.peer, false, 42, c.now).status == SP5_OK);
    }
    for (unsigned count = 0; count < OPERATION_LIMIT; ++count) {
        uint8_t op = 0;
        if (!u8(&r, &op)) {
            break;
        }
        ++c.now;
        switch (op % 7) {
        case 0:
            receive(&c, &r);
            break;
        case 1: {
            uint16_t steps = 0;
            if (u16(&r, &steps)) {
                c.now += (uint64_t)steps * 100000;
                (void)sp5_control_tick(c.control, c.now);
            }
            break;
        }
        case 2:
            drain(&c, &r);
            break;
        case 3:
            send(&c, &r);
            break;
        case 4: {
            uint8_t reason = 0;
            if (u8(&r, &reason)) {
                /* A valid reason ends the channel even when cleanup reports a fault. */
                (void)sp5_control_end(c.control, (sp5_end_reason)(reason % 7));
                c.closed = c.closed || reason % 7 <= SP5_END_ERROR;
            }
            break;
        }
        case 5: {
            uint32_t token = 0;
            uint8_t peer = 0;
            if (u32(&r, &token) && u8(&r, &peer)) {
                sp5_result accepted = sp5_control_accept(
                    c.control, (peer & 1) != 0 ? "203.0.113.9" : config.peer, false, token, c.now);
                fuzz_require(accepted.status != SP5_OK ||
                             (!c.closed && token == 42 && (peer & 1) == 0));
            }
            break;
        }
        default:
            if (!c.closed) {
                fuzz_require(sp5_authority_revoke(c.authority, c.certificate).status == SP5_OK);
                c.closed = true;
            }
            break;
        }
        /* The input-first termination hook runs at most once. */
        fuzz_require(c.releases <= 1);
    }
    if (c.closed) {
        sp5_control_event event;
        fuzz_require(sp5_control_take(c.control, &event).status != SP5_OK);
    }
    sp5_control_destroy(c.control);
    fuzz_require(c.releases <= 1);
    sp5_session_destroy(c.session);
    sp5_authority_destroy(c.authority);
    sp5_devices_destroy(c.devices);
    return 0;
}
