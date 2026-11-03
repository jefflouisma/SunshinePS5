#include "../fakes/platform.h"
#include "controller.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            if (fputs(__FILE__ ": " #c "\n", stderr) == EOF)                                       \
                return 2;                                                                          \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
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
typedef struct {
    uint8_t id;
    uint16_t caps;
} arrival_fields;
static void arrival(uint8_t p[16], arrival_fields fields)
{
    memset(p, 0, 16);
    p[3] = 12;
    put32(p + 4, 0x55000004);
    p[8] = fields.id;
    p[9] = 2;
    put16(p + 10, fields.caps);
    put32(p + 12, 0x003ff7ff);
}
static void gamepad(uint8_t p[34], uint16_t id, uint16_t mask)
{
    memset(p, 0, 34);
    p[3] = 30;
    p[4] = 12;
    put16(p + 8, 0x1a);
    put16(p + 10, id);
    put16(p + 12, mask);
    put16(p + 14, 0x14);
    put16(p + 16, 0x400);
    p[18] = 127;
    put16(p + 28, 0x9c);
    put16(p + 30, 0x10);
    put16(p + 32, 0x55);
}
typedef struct {
    uint8_t type;
    uint32_t id;
    uint8_t surface;
} touch_fields;
static void touch(uint8_t p[28], touch_fields fields)
{
    memset(p, 0, 28);
    p[3] = 24;
    put32(p + 4, 0x55000005);
    p[8] = 15;
    p[9] = fields.type;
    p[11] = fields.surface;
    put32(p + 12, fields.id);
    put32(p + 16, 0x3e800000);
    put32(p + 20, 0x3f400000);
    put32(p + 24, 0x3f000000);
}
static int malformed(void)
{
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_controller_input *in = sp5_controller_create(91, &fake.input);
    CHECK(in != NULL);
    uint8_t p[34];
    gamepad(p, 15, 0x8000);
    sp5_input_evidence e = {.session_id = 91, .host_received = fake.now};
    for (size_t n = 0; n < 34; ++n)
        CHECK(sp5_controller_receive(in, (sp5_bytes){p, n}, false, &e).status == SP5_ERROR);
    CHECK(fake.calls[SP5_FAKE_INPUT_ADD] == 0);
    CHECK(sp5_controller_receive(in, (sp5_bytes){p, 34}, false, &e).status == SP5_OK);
    CHECK(e.controller_id == 0 && fake.controllers[0].state.buttons == 0x100400);
    uint64_t deliveries = fake.calls[SP5_FAKE_INPUT_DELIVER];
    p[3] = 31;
    CHECK(sp5_controller_receive(in, (sp5_bytes){p, 34}, false, &e).status == SP5_ERROR);
    p[3] = 30;
    put16(p + 10, 65535);
    CHECK(sp5_controller_receive(in, (sp5_bytes){p, 34}, false, &e).status == SP5_ERROR);
    gamepad(p, 15, 0x8000);
    p[4] = 3;
    CHECK(sp5_controller_receive(in, (sp5_bytes){p, 34}, false, &e).reason ==
          SP5_REASON_UNSUPPORTED);
    CHECK(fake.calls[SP5_FAKE_INPUT_DELIVER] == deliveries);
    uint8_t t[28];
    touch(t, (touch_fields){1, 1, 0});
    CHECK(sp5_controller_receive(in, (sp5_bytes){t, 28}, false, &e).status == SP5_OK);
    CHECK(fake.controllers[0].state.touches[0].pressure == 0.5F);
    touch(t, (touch_fields){1, 2, 1});
    CHECK(sp5_controller_receive(in, (sp5_bytes){t, 28}, false, &e).status == SP5_OK);
    CHECK(fake.controllers[0].state.touches[1].touchpad_index == 1);
    touch(t, (touch_fields){1, 3, 0});
    CHECK(sp5_controller_receive(in, (sp5_bytes){t, 28}, false, &e).reason == SP5_REASON_CAPACITY);
    touch(t, (touch_fields){3, 1, 0});
    put32(t + 16, 0x7fc00000);
    CHECK(sp5_controller_receive(in, (sp5_bytes){t, 28}, false, &e).status == SP5_ERROR);
    touch(t, (touch_fields){3, 1, 0});
    put32(t + 16, 0xbf800000);
    put32(t + 20, 0x40000000);
    CHECK(sp5_controller_receive(in, (sp5_bytes){t, 28}, false, &e).status == SP5_OK);
    CHECK(fake.controllers[0].state.touches[0].x == 0 &&
          fake.controllers[0].state.touches[0].y == 1);
    touch(t, (touch_fields){7, 0, 0});
    CHECK(sp5_controller_receive(in, (sp5_bytes){t, 28}, false, &e).status == SP5_OK);
    CHECK(!fake.controllers[0].state.touches[0].active &&
          !fake.controllers[0].state.touches[1].active);
    uint8_t motion[24] = {0, 0, 0, 20, 6, 0, 0, 0x55, 15, 1};
    put32(motion + 12, 0x411d0000);
    CHECK(sp5_controller_receive(in, (sp5_bytes){motion, 24}, false, &e).status == SP5_OK);
    CHECK(fake.controllers[0].state.acceleration[0] == 9.8125F);
    put32(motion + 16, 0x7f800000);
    CHECK(sp5_controller_receive(in, (sp5_bytes){motion, 24}, false, &e).status == SP5_ERROR);
    fake.fail_next[SP5_FAKE_INPUT_RELEASE_ALL] = SP5_REASON_PLATFORM;
    CHECK(sp5_controller_close(in).status == SP5_ERROR);
    CHECK(!fake.controllers[0].live && fake.controllers[0].state.buttons == 0);
    CHECK(sp5_controller_close(in).status == SP5_OK);
    sp5_controller_destroy(in);
    return 0;
}
typedef struct {
    uint8_t bytes[40000];
    size_t size;
} file_store;
static sp5_result read_file(void *ctx, sp5_store_file file, sp5_buffer *out)
{
    file_store *f = ctx;
    if (file != SP5_STORE_DEVICES || !f->size)
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    return sp5_buffer_append(out, (sp5_bytes){f->bytes, f->size});
}
static sp5_result write_file(void *ctx, sp5_store_file file, sp5_bytes bytes)
{
    file_store *f = ctx;
    if (file != SP5_STORE_DEVICES || bytes.size > sizeof(f->bytes))
        return sp5_failure(SP5_REASON_CAPACITY);
    memcpy(f->bytes, bytes.data, bytes.size);
    f->size = bytes.size;
    return sp5_success();
}
static sp5_result release_input(void *ctx, sp5_end_reason reason)
{
    (void)reason;
    return sp5_controller_close(ctx);
}
static sp5_authority *revoke_authority;
static sp5_bytes revoke_certificate;
static sp5_input_ops original_ops;
static sp5_result revoke_on_add(void *context, uint64_t session_id, uint32_t controller_id)
{
    sp5_result result = original_ops.add(context, session_id, controller_id);
    if (result.status != SP5_OK)
        return result;
    return sp5_authority_revoke(revoke_authority, revoke_certificate);
}
static bool feedback_ready;
static sp5_result feedback(void *ctx, sp5_controller_feedback *out)
{
    (void)ctx;
    if (!feedback_ready)
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    feedback_ready = false;
    *out = (sp5_controller_feedback){0, 0x1234, 0xabcd};
    return sp5_success();
}
static sp5_result take_wire(sp5_control *control, sp5_buffer *wire)
{
    wire->size = 0;
    return sp5_control_take_wire(control, wire);
}
static int authority_feedback(void)
{
    file_store f = {0};
    sp5_storage_ops ops = {&f, read_file, write_file, NULL};
    sp5_devices *devices = sp5_devices_create(&ops);
    CHECK(devices && sp5_devices_load(devices).status == SP5_OK);
    sp5_authority *auth = sp5_authority_create(devices);
    sp5_session *session = sp5_session_create(auth);
    const uint8_t cert_bytes[] = {1, 2, 3};
    sp5_bytes cert = {cert_bytes, sizeof(cert_bytes)};
    CHECK(sp5_authority_pair(auth, cert).status == SP5_OK &&
          sp5_session_admit(session, cert, 91).status == SP5_OK);
    sp5_host host = {0};
    host.supported_mode_count = 1;
    host.supported_modes[0] = (sp5_host_mode){1280, 720, 60, "h264", SP5_SDR, 100, 1000};
    CHECK(sp5_session_configure(session, &host, &(sp5_session_parameters){.requested_kbps = 500})
                  .status == SP5_OK &&
          sp5_session_activate(session).status == SP5_OK);
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    fake.input.poll_feedback = feedback;
    sp5_controller_input *in = sp5_controller_create_bound(session, cert, &fake.input);
    CHECK(in != NULL);
    sp5_control_config config = {.session = session,
                                 .verified_certificate = cert,
                                 .peer = "10.1.2.3",
                                 .release_input = release_input,
                                 .end_context = in,
                                 .launch = {.session_id = 91, .control_connect_data = 42},
                                 .negotiated = {.encryption_flags = 7}};
    memset(config.launch.key, 0x42, 16);
    sp5_control *control = sp5_control_create(&config);
    CHECK(control != NULL &&
          sp5_control_accept(control, "10.1.2.3", false, 42, 0).status == SP5_OK);
    sp5_control_event p = {
        .type = 0x0206, .channel = 31, .delivery = SP5_CONTROL_RELIABLE, .payload_size = 16};
    arrival(p.payload, (arrival_fields){15, 0x3b});
    sp5_input_evidence e = {.session_id = 91, .host_received = fake.now};
    p.channel = 16;
    CHECK(sp5_controller_receive_control(in, &p, false, &e).status == SP5_ERROR);
    p.channel = 31;
    CHECK(sp5_controller_receive_control(in, &p, false, &e).status == SP5_OK);
    gamepad(p.payload, 15, 0x8000);
    p.payload_size = 34;
    p.delivery = SP5_CONTROL_UNRELIABLE;
    CHECK(sp5_controller_receive_control(in, &p, false, &e).status == SP5_ERROR);
    p.delivery = SP5_CONTROL_RELIABLE;
    CHECK(sp5_controller_receive_control(in, &p, false, &e).status == SP5_OK);
    CHECK(fake.controllers[0].state.buttons == 0x100400);
    feedback_ready = true;
    CHECK(sp5_controller_forward_feedback(in, control).status == SP5_OK);
    uint8_t bytes[SP5_CONTROL_WIRE_LIMIT], plain[64], iv[12] = {0};
    iv[10] = 'H';
    iv[11] = 'C';
    sp5_buffer wire = {bytes, sizeof(bytes), 0};
    CHECK(take_wire(control, &wire).status == SP5_OK);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    CHECK(ctx != NULL);
    int n = 0, tail = 0;
    CHECK(EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, config.launch.key, iv) == 1);
    CHECK(EVP_DecryptUpdate(ctx, plain, &n, bytes + 24, (int)wire.size - 24) == 1);
    CHECK(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, bytes + 8) == 1 &&
          EVP_DecryptFinal_ex(ctx, plain + n, &tail) == 1);
    const uint8_t expected[] = {0x0b, 1, 10, 0, 0xee, 0xff, 0xc0, 0, 15, 0, 0x34, 0x12, 0xcd, 0xab};
    CHECK(n + tail == (int)sizeof(expected) && memcmp(plain, expected, sizeof(expected)) == 0);
    EVP_CIPHER_CTX_free(ctx);
    CHECK(sp5_controller_motion_request(in, control, (sp5_controller_motion_config){0, 200, 2})
              .status == SP5_OK);
    CHECK(take_wire(control, &wire).status == SP5_OK);
    iv[0] = 1;
    ctx = EVP_CIPHER_CTX_new();
    CHECK(ctx != NULL);
    CHECK(EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, config.launch.key, iv) == 1);
    CHECK(EVP_DecryptUpdate(ctx, plain, &n, bytes + 24, (int)wire.size - 24) == 1);
    CHECK(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, bytes + 8) == 1 &&
          EVP_DecryptFinal_ex(ctx, plain + n, &tail) == 1);
    const uint8_t motion_expected[] = {1, 0x55, 5, 0, 15, 0, 200, 0, 2};
    CHECK(n + tail == (int)sizeof(motion_expected) &&
          memcmp(plain, motion_expected, sizeof(motion_expected)) == 0);
    EVP_CIPHER_CTX_free(ctx);
    for (unsigned i = 0; i < SP5_CONTROL_QUEUE_CAPACITY; ++i)
        CHECK(sp5_control_send(control, 0x010b, (sp5_bytes){expected + 4, 10}).status == SP5_OK);
    feedback_ready = true;
    CHECK(sp5_controller_forward_feedback(in, control).reason == SP5_REASON_WOULD_BLOCK);
    CHECK(take_wire(control, &wire).status == SP5_OK);
    CHECK(sp5_controller_forward_feedback(in, control).status == SP5_OK && !feedback_ready);
    while (take_wire(control, &wire).status == SP5_OK) {
    }
    arrival(p.payload, (arrival_fields){15, 0x10});
    p.payload_size = 16;
    CHECK(sp5_controller_receive_control(in, &p, false, &e).status == SP5_OK);
    feedback_ready = true;
    CHECK(sp5_controller_forward_feedback(in, control).reason == SP5_REASON_UNSUPPORTED);
    CHECK(sp5_controller_motion_request(in, control, (sp5_controller_motion_config){0, 200, 2})
              .reason == SP5_REASON_UNSUPPORTED);
    arrival(p.payload, (arrival_fields){15, 0x3b});
    CHECK(sp5_controller_receive_control(in, &p, false, &e).status == SP5_OK);
    gamepad(p.payload, 15, 0x8000);
    p.payload_size = 34;
    sp5_controller_input *stale = sp5_controller_create_bound(session, cert, &fake.input);
    CHECK(stale != NULL && sp5_session_authorized_id(session, cert, 91));
    CHECK(sp5_authority_revoke(auth, cert).status == SP5_OK);
    uint64_t delivered = fake.calls[SP5_FAKE_INPUT_DELIVER];
    CHECK(sp5_controller_receive_control(in, &p, false, &e).status == SP5_ERROR &&
          fake.calls[SP5_FAKE_INPUT_DELIVER] == delivered);
    CHECK(sp5_controller_forward_feedback(in, control).status == SP5_ERROR);
    CHECK(sp5_control_tick(control, 1).status == SP5_ERROR && !fake.controllers[0].live);
    sp5_control_destroy(control);
    CHECK(sp5_session_end(session, SP5_END_REVOKED).status == SP5_OK);
    CHECK(sp5_authority_pair(auth, cert).status == SP5_OK);
    CHECK(sp5_session_admit(session, cert, 92).status == SP5_OK);
    CHECK(sp5_session_configure(session, &host, &(sp5_session_parameters){.requested_kbps = 500})
              .status == SP5_OK);
    CHECK(sp5_session_activate(session).status == SP5_OK);
    CHECK(sp5_session_authorized(session, cert));
    CHECK(!sp5_session_authorized_id(session, cert, 91));
    CHECK(sp5_session_authorized_id(session, cert, 92));
    CHECK(sp5_controller_receive_control(stale, &p, false, &e).status == SP5_ERROR);
    CHECK(fake.calls[SP5_FAKE_INPUT_DELIVER] == delivered && !fake.controllers[0].live);
    sp5_controller_destroy(stale);
    /* Deterministic revoke interleaving between platform add and delivery:
     * authorization must be checked again immediately before injection. */
    original_ops = fake.input;
    revoke_authority = auth;
    revoke_certificate = cert;
    fake.input.add = revoke_on_add;
    sp5_controller_input *racing = sp5_controller_create_bound(session, cert, &fake.input);
    CHECK(racing != NULL);
    e.session_id = 92;
    CHECK(sp5_controller_receive_control(racing, &p, false, &e).status == SP5_ERROR);
    CHECK(fake.calls[SP5_FAKE_INPUT_DELIVER] == delivered);
    CHECK(!sp5_session_authorized_id(session, cert, 92));
    CHECK(sp5_controller_close(racing).status == SP5_OK && !fake.controllers[0].live);
    sp5_controller_destroy(racing);
    sp5_controller_destroy(in);
    sp5_session_destroy(session);
    sp5_authority_destroy(auth);
    sp5_devices_destroy(devices);
    return 0;
}
int main(void)
{
    CHECK(malformed() == 0);
    CHECK(authority_feedback() == 0);
    return 0;
}
