#include "protocol/control.h"
#include <enet/enet.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            if (fputs(__FILE__ ": " #c "\n", stderr) == EOF)                                       \
                return 2;                                                                          \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
typedef struct {
    uint8_t data[40000];
    size_t size;
    unsigned released;
    bool release_failed;
    bool admission_closed;
    sp5_session *session;
    sp5_end_reason reason;
} files;
static sp5_result read_file(void *ctx, sp5_store_file file, sp5_buffer *out)
{
    files *f = ctx;
    if (file != SP5_STORE_DEVICES || f->size == 0)
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    return sp5_buffer_append(out, (sp5_bytes){f->data, f->size});
}
static sp5_result write_file(void *ctx, sp5_store_file file, sp5_bytes data)
{
    files *f = ctx;
    if (file != SP5_STORE_DEVICES || data.size > sizeof(f->data))
        return sp5_failure(SP5_REASON_CAPACITY);
    memcpy(f->data, data.data, data.size);
    f->size = data.size;
    return sp5_success();
}
static sp5_result release(void *ctx, sp5_end_reason reason)
{
    files *f = ctx;
    ++f->released;
    f->admission_closed = sp5_session_read(f->session).state >= SP5_SESSION_ENDING;
    f->reason = reason;
    return f->release_failed ? sp5_failure(SP5_REASON_DELIVERY_UNCONFIRMED) : sp5_success();
}
static sp5_bytes bytes(const char *s)
{
    return (sp5_bytes){(const uint8_t *)s, strlen(s)};
}
/* Independent client-side envelope oracle: notes §9. */
static sp5_result client_packet(uint32_t sequence, uint16_t type, sp5_bytes payload,
                                sp5_buffer *wire)
{
    const struct {
        uint32_t sequence;
        uint16_t type;
    } header = {sequence, type};
    if (payload.size > SP5_CONTROL_PAYLOAD_LIMIT || wire->capacity < payload.size + 28)
        return sp5_failure(SP5_REASON_CAPACITY);
    uint8_t plain[SP5_CONTROL_PAYLOAD_LIMIT + 4] = {0}, iv[12] = {0}, key[16];
    memset(key, 0x42, sizeof(key));
    plain[0] = (uint8_t)header.type;
    plain[1] = (uint8_t)(header.type >> 8);
    plain[2] = (uint8_t)payload.size;
    plain[3] = (uint8_t)(payload.size >> 8);
    if (payload.size)
        memcpy(plain + 4, payload.data, payload.size);
    wire->data[0] = 1;
    wire->data[1] = 0;
    wire->data[2] = (uint8_t)(payload.size + 24);
    wire->data[3] = (uint8_t)((payload.size + 24) >> 8);
    for (unsigned i = 0; i < 4; ++i) {
        wire->data[4 + i] = (uint8_t)(header.sequence >> (8 * i));
        iv[i] = wire->data[4 + i];
    }
    iv[10] = 'C';
    iv[11] = 'C';
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int n = 0, tail = 0;
    if (c == NULL)
        return sp5_failure(SP5_REASON_PLATFORM);
    bool ok = EVP_EncryptInit_ex(c, EVP_aes_128_gcm(), NULL, key, iv) == 1 &&
              EVP_EncryptUpdate(c, wire->data + 24, &n, plain, (int)payload.size + 4) == 1 &&
              EVP_EncryptFinal_ex(c, wire->data + 24 + n, &tail) == 1 &&
              EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, wire->data + 8) == 1;
    EVP_CIPHER_CTX_free(c);
    wire->size = ok ? payload.size + 28 : 0;
    return ok ? sp5_success() : sp5_failure(SP5_REASON_PLATFORM);
}
static int error_cases(sp5_authority *authority, sp5_session *session, const sp5_host *host,
                       sp5_control_config config, files *f)
{
    uint8_t storage[SP5_CONTROL_WIRE_LIMIT];
    sp5_buffer wire = {storage, sizeof(storage), 0};
    const uint8_t idr[] = {0, 0}, ping[] = {4, 0, 0, 0, 0, 0, 0, 0};
    for (unsigned scenario = 0; scenario < 9; ++scenario) {
        CHECK(sp5_session_end(session, SP5_END_CLIENT_QUIT).status == SP5_OK);
        config.launch.session_id = 3 + scenario;
        CHECK(sp5_session_admit(session, config.verified_certificate, config.launch.session_id)
                  .status == SP5_OK);
        CHECK(sp5_session_configure(
                  session, host, &(sp5_session_parameters){.mode_index = 0, .requested_kbps = 500})
                      .status == SP5_OK &&
              sp5_session_activate(session).status == SP5_OK);
        sp5_control *c = sp5_control_create(&config);
        CHECK(c != NULL);
        CHECK(sp5_control_accept(c, config.peer, false, 42, 100).status == SP5_OK);
        unsigned before = f->released;
        if (scenario == 0) {
            CHECK(client_packet(0, 0x0200, (sp5_bytes){ping, 8}, &wire).status == SP5_OK);
            storage[8] ^= 1;
            CHECK(sp5_control_receive(c, config.peer, false, 0, SP5_CONTROL_RELIABLE,
                                      (sp5_bytes){storage, wire.size}, 200)
                      .status == SP5_ERROR);
            CHECK(sp5_control_diagnostic(c) == SP5_REASON_OWNERSHIP);
        } else if (scenario == 1) {
            for (unsigned i = 0; i <= SP5_CONTROL_QUEUE_CAPACITY; ++i) {
                CHECK(client_packet(i, 0x0302, (sp5_bytes){idr, 2}, &wire).status == SP5_OK);
                sp5_result result =
                    sp5_control_receive(c, config.peer, false, 1, SP5_CONTROL_RELIABLE,
                                        (sp5_bytes){storage, wire.size}, 200 + i);
                CHECK(result.status == (i < SP5_CONTROL_QUEUE_CAPACITY ? SP5_OK : SP5_ERROR));
            }
            CHECK(sp5_control_diagnostic(c) == SP5_REASON_CAPACITY);
        } else if (scenario == 2) {
            uint8_t rumble[10] = {0};
            for (unsigned i = 0; i < SP5_CONTROL_QUEUE_CAPACITY; ++i)
                CHECK(sp5_control_send(c, 0x010b, (sp5_bytes){rumble, 10}).status == SP5_OK);
            CHECK(sp5_control_send(c, 0x010b, (sp5_bytes){rumble, 10}).reason ==
                  SP5_REASON_WOULD_BLOCK);
            sp5_buffer tiny = {storage, 1, 0};
            CHECK(sp5_control_take_wire(c, &tiny).reason == SP5_REASON_CAPACITY);
            for (unsigned i = 0; i < SP5_CONTROL_QUEUE_CAPACITY; ++i) {
                wire.size = 0;
                CHECK(sp5_control_take_wire(c, &wire).status == SP5_OK);
            }
            CHECK(sp5_control_take_wire(c, &wire).reason == SP5_REASON_WOULD_BLOCK);
            CHECK(sp5_control_end(c, SP5_END_HOST_STOPPING).status == SP5_OK);
        } else if (scenario == 3) {
            f->release_failed = true;
            CHECK(sp5_control_end(c, SP5_END_HOST_STOPPING).status == SP5_ERROR);
            CHECK(sp5_control_diagnostic(c) == SP5_REASON_DELIVERY_UNCONFIRMED);
            CHECK(sp5_control_end(c, SP5_END_CLIENT_QUIT).status == SP5_OK);
            CHECK(sp5_session_read(session).end_reason == SP5_END_HOST_STOPPING);
            f->release_failed = false;
        } else if (scenario == 4) {
            CHECK(sp5_control_tick(c, 99).reason == SP5_REASON_CLOCK);
        } else if (scenario == 5) {
            const uint32_t sequences[] = {0, 1025, 1, 1024, 1024};
            for (size_t i = 0; i < sizeof(sequences) / sizeof(sequences[0]); ++i) {
                CHECK(client_packet(sequences[i], 0x0200, (sp5_bytes){ping, 8}, &wire).status ==
                      SP5_OK);
                sp5_result result =
                    sp5_control_receive(c, config.peer, false, 0, SP5_CONTROL_RELIABLE,
                                        (sp5_bytes){storage, wire.size}, 200 + i);
                CHECK(result.status == ((i == 2 || i == 4) ? SP5_ERROR : SP5_OK));
            }
            CHECK(sp5_control_end(c, SP5_END_CLIENT_QUIT).status == SP5_OK);
        } else if (scenario == 6) {
            CHECK(client_packet(0, 0x0302, (sp5_bytes){idr, 2}, &wire).status == SP5_OK);
            CHECK(sp5_control_receive(c, config.peer, false, 1, SP5_CONTROL_RELIABLE,
                                      (sp5_bytes){storage, wire.size}, 200)
                      .status == SP5_OK);
            CHECK(sp5_authority_revoke(authority, config.verified_certificate).status == SP5_OK);
            sp5_control_event event;
            CHECK(sp5_control_take(c, &event).status == SP5_ERROR && event.payload_size == 0);
            CHECK(sp5_authority_pair(authority, config.verified_certificate).status == SP5_OK);
        }
        if (scenario == 7) {
            CHECK(sp5_control_end(c, SP5_END_ERROR).status == SP5_OK);
            CHECK(sp5_control_diagnostic(c) == SP5_REASON_PLATFORM);
        }
        if (scenario == 8) {
            CHECK(sp5_session_end(session, SP5_END_CLIENT_QUIT).status == SP5_OK);
            CHECK(sp5_session_admit(session, config.verified_certificate, 100).status == SP5_OK);
            CHECK(sp5_session_configure(
                      session, host,
                      &(sp5_session_parameters){.mode_index = 0, .requested_kbps = 500})
                          .status == SP5_OK &&
                  sp5_session_activate(session).status == SP5_OK);
            CHECK(sp5_control_tick(c, 101).status == SP5_ERROR);
            CHECK(sp5_session_read(session).id == 100 &&
                  sp5_session_read(session).state == SP5_SESSION_ACTIVE);
            CHECK(f->released == before);
            sp5_control_destroy(c);
            CHECK(sp5_session_end(session, SP5_END_CLIENT_QUIT).status == SP5_OK);
            continue;
        }
        CHECK(f->released == before + 1 && f->admission_closed);
        sp5_control_destroy(c);
        CHECK(f->released == before + 1);
    }
    return 0;
}
static int owner_session(sp5_session *session, sp5_control_config *config, uint64_t id)
{
    sp5_host host = {.supported_mode_count = 1};
    host.supported_modes[0] = (sp5_host_mode){1280, 720, 60, "h264", SP5_SDR, 100, 1000};
    config->launch.session_id = id;
    CHECK(sp5_session_admit(session, config->verified_certificate, id).status == SP5_OK);
    CHECK(sp5_session_configure(session, &host,
                                &(sp5_session_parameters){.mode_index = 0, .requested_kbps = 500})
                  .status == SP5_OK &&
          sp5_session_activate(session).status == SP5_OK);
    return 0;
}
/* session.h owner boundary: the independent control/input owner closes admission
 * and claims its end reason with atomics only. Snapshot history belongs to the
 * serialized management owner, which records that first reason exactly once;
 * a later revoke/stop cannot replace it (FR-039: every end has one reason). */
static int input_owner_claim(void)
{
    files f = {0};
    sp5_storage_ops ops = {&f, read_file, write_file, NULL};
    sp5_devices *devices = sp5_devices_create(&ops);
    CHECK(devices != NULL && sp5_devices_load(devices).status == SP5_OK);
    sp5_authority *authority = sp5_authority_create(devices);
    sp5_session *session = sp5_session_create(authority);
    CHECK(session != NULL);
    f.session = session;
    sp5_control_config config = {.session = session,
                                 .verified_certificate = bytes("owner-boundary-client"),
                                 .peer = "10.1.2.3",
                                 .started_us = 100,
                                 .end_context = &f,
                                 .release_input = release,
                                 .launch = {.control_connect_data = 42},
                                 .negotiated = {.encryption_flags = 1, .feature_flags = 3}};
    CHECK(sp5_authority_pair(authority, config.verified_certificate).status == SP5_OK);
    CHECK(owner_session(session, &config, 50) == 0);
    sp5_control *c = sp5_control_create(&config);
    CHECK(c != NULL && sp5_control_accept(c, config.peer, false, 42, 100).status == SP5_OK);
    CHECK(sp5_authority_end_count(authority) == 0);
    CHECK(sp5_control_tick(c, 100 + 10000000).status == SP5_ERROR);
    CHECK(f.released == 1 && f.admission_closed && f.reason == SP5_END_CLIENT_LOST);
    CHECK(!sp5_session_authorized(session, config.verified_certificate));
    sp5_session_view view = sp5_session_read(session);
    CHECK(view.state == SP5_SESSION_ENDING && view.has_end_reason &&
          view.end_reason == SP5_END_CLIENT_LOST && view.diagnostic == SP5_REASON_TIMEOUT);
    /* The input owner never wrote the management owner's bounded history. */
    CHECK(sp5_authority_end_count(authority) == 0);
    CHECK(sp5_session_begin_end(session, SP5_END_REVOKED, SP5_REASON_NONE).status == SP5_OK);
    CHECK(sp5_authority_end_count(authority) == 1);
    CHECK(sp5_session_end(session, SP5_END_REVOKED).status == SP5_OK);
    view = sp5_session_read(session);
    CHECK(view.state == SP5_SESSION_ENDED && view.end_reason == SP5_END_CLIENT_LOST &&
          view.diagnostic == SP5_REASON_TIMEOUT);
    sp5_session_end_record record;
    CHECK(sp5_authority_end_count(authority) == 1 &&
          sp5_authority_end_at(authority, 0, &record).status == SP5_OK && record.session_id == 50 &&
          record.reason == SP5_END_CLIENT_LOST && record.diagnostic == SP5_REASON_TIMEOUT);
    sp5_control_destroy(c);
    CHECK(f.released == 1 && sp5_authority_end_count(authority) == 1);
    /* A stale control for an ended launch cannot claim the next session. */
    CHECK(owner_session(session, &config, 51) == 0);
    config.launch.session_id = 50;
    sp5_control *stale = sp5_control_create(&config);
    CHECK(stale == NULL);
    config.launch.session_id = 51;
    c = sp5_control_create(&config);
    CHECK(c != NULL && sp5_session_end(session, SP5_END_HOST_STOPPING).status == SP5_OK);
    CHECK(sp5_control_tick(c, 101).status == SP5_ERROR && f.released == 2 &&
          f.reason == SP5_END_HOST_STOPPING);
    CHECK(sp5_authority_end_count(authority) == 2 &&
          sp5_authority_end_at(authority, 1, &record).status == SP5_OK && record.session_id == 51 &&
          record.reason == SP5_END_HOST_STOPPING);
    sp5_control_destroy(c);
    sp5_session_destroy(session);
    /* A reused handle carries no earlier claim into its fresh snapshot. */
    session = sp5_session_create(authority);
    CHECK(session != NULL && sp5_session_read(session).state == SP5_SESSION_ENDED &&
          !sp5_session_read(session).has_end_reason &&
          !sp5_session_end_claimed(session, &(sp5_end_reason){0}));
    sp5_session_destroy(session);
    sp5_authority_destroy(authority);
    sp5_devices_destroy(devices);
    return 0;
}
typedef struct {
    sp5_control *control;
    atomic_int gate; /* 0 idle, 1 end now, -1 exit */
    atomic_uint released;
} owner_race;
static sp5_result race_release(void *ctx, sp5_end_reason reason)
{
    (void)reason;
    owner_race *race = ctx;
    atomic_fetch_add_explicit(&race->released, 1, memory_order_relaxed);
    return sp5_success();
}
static void *control_owner(void *ctx)
{
    owner_race *race = ctx;
    for (;;) {
        int gate;
        while ((gate = atomic_load_explicit(&race->gate, memory_order_acquire)) == 0) {
        }
        if (gate < 0)
            return NULL;
        (void)sp5_control_end(race->control, SP5_END_CLIENT_LOST);
        atomic_store_explicit(&race->gate, 0, memory_order_release);
    }
}
/* Client loss on the control owner while the management owner revokes (as
 * revoke.c does): one retained reason and one history record per session.
 * Under a local ThreadSanitizer build (not yet a CI lane) this also detects
 * unsynchronized session access. */
static int concurrent_end_owners(void)
{
    files f = {0};
    sp5_storage_ops ops = {&f, read_file, write_file, NULL};
    sp5_devices *devices = sp5_devices_create(&ops);
    CHECK(devices != NULL && sp5_devices_load(devices).status == SP5_OK);
    sp5_authority *authority = sp5_authority_create(devices);
    sp5_session *session = sp5_session_create(authority);
    CHECK(session != NULL);
    owner_race race = {0};
    atomic_init(&race.gate, 0);
    atomic_init(&race.released, 0);
    sp5_control_config config = {.session = session,
                                 .verified_certificate = bytes("owner-race-client"),
                                 .peer = "10.1.2.3",
                                 .end_context = &race,
                                 .release_input = race_release,
                                 .negotiated = {.encryption_flags = 1}};
    CHECK(sp5_authority_pair(authority, config.verified_certificate).status == SP5_OK);
    pthread_t owner;
    CHECK(pthread_create(&owner, NULL, control_owner, &race) == 0);
    int failed = 0;
    for (uint64_t id = 1; id <= 200 && failed == 0; ++id) {
        race.control =
            owner_session(session, &config, id) == 0 ? sp5_control_create(&config) : NULL;
        if (race.control == NULL) {
            failed = 1;
            break;
        }
        atomic_store_explicit(&race.gate, 1, memory_order_release);
        sp5_result revoked = sp5_session_begin_end(session, SP5_END_REVOKED, SP5_REASON_NONE);
        sp5_session_view during = sp5_session_read(session);
        while (atomic_load_explicit(&race.gate, memory_order_acquire) != 0) {
        }
        sp5_result ended = sp5_session_end(session, SP5_END_REVOKED);
        sp5_session_view view = sp5_session_read(session);
        size_t count = sp5_authority_end_count(authority);
        sp5_session_end_record latest = {0}, previous = {0};
        bool recorded =
            count > 0 && sp5_authority_end_at(authority, count - 1, &latest).status == SP5_OK &&
            (count == 1 || sp5_authority_end_at(authority, count - 2, &previous).status == SP5_OK);
        bool one_reason =
            during.has_end_reason && view.has_end_reason &&
            (view.end_reason == SP5_END_REVOKED || view.end_reason == SP5_END_CLIENT_LOST) &&
            view.end_reason == during.end_reason;
        bool one_record = recorded && latest.session_id == id && latest.reason == view.end_reason &&
                          (count == 1 || previous.session_id == id - 1);
        if (revoked.status != SP5_OK || ended.status != SP5_OK || view.state != SP5_SESSION_ENDED ||
            !one_reason || !one_record ||
            atomic_load_explicit(&race.released, memory_order_relaxed) != id)
            failed = 1;
        sp5_control_destroy(race.control);
        race.control = NULL;
    }
    atomic_store_explicit(&race.gate, -1, memory_order_release);
    CHECK(pthread_join(owner, NULL) == 0);
    CHECK(failed == 0);
    sp5_session_destroy(session);
    sp5_authority_destroy(authority);
    sp5_devices_destroy(devices);
    return 0;
}
static size_t crypto_allocations;
static void *count_allocate(size_t size, const char *file, int line)
{
    (void)file;
    (void)line;
    ++crypto_allocations;
    return malloc(size);
}
static void *count_reallocate(void *p, size_t size, const char *file, int line)
{
    (void)file;
    (void)line;
    ++crypto_allocations;
    return realloc(p, size);
}
static void count_free(void *p, const char *file, int line)
{
    (void)file;
    (void)line;
    free(p);
}
int main(void)
{
    CHECK(CRYPTO_set_mem_functions(count_allocate, count_reallocate, count_free) == 1);
    files f = {0};
    sp5_storage_ops ops = {&f, read_file, write_file, NULL};
    sp5_devices *devices = sp5_devices_create(&ops);
    CHECK(devices != NULL && sp5_devices_load(devices).status == SP5_OK);
    sp5_authority *authority = sp5_authority_create(devices);
    sp5_session *session = sp5_session_create(authority);
    CHECK(session != NULL);
    f.session = session;
    sp5_bytes cert = bytes("synthetic-control-client");
    CHECK(sp5_authority_pair(authority, cert).status == SP5_OK &&
          sp5_session_admit(session, cert, 1).status == SP5_OK);
    sp5_host host = {0};
    host.supported_mode_count = 1;
    host.supported_modes[0] = (sp5_host_mode){1280, 720, 60, "h264", SP5_SDR, 100, 1000};
    CHECK(sp5_session_configure(session, &host,
                                &(sp5_session_parameters){.mode_index = 0, .requested_kbps = 500})
                  .status == SP5_OK &&
          sp5_session_activate(session).status == SP5_OK);
    sp5_control_config config = {.session = session,
                                 .verified_certificate = cert,
                                 .peer = "10.1.2.3",
                                 .started_us = 100,
                                 .end_context = &f,
                                 .release_input = release,
                                 .launch = {.session_id = 1, .control_connect_data = 42},
                                 .negotiated = {.encryption_flags = 1, .feature_flags = 3}};
    memset(config.launch.key, 0x42, 16);
    sp5_control_config media_config = config;
    media_config.negotiated.encryption_flags = 9;
    CHECK(sp5_control_create(&media_config) == NULL);
    media_config.negotiated.encryption_flags = 7;
    sp5_control *c = sp5_control_create(&media_config);
    CHECK(c != NULL);
    uint8_t data[SP5_CONTROL_WIRE_LIMIT];
    sp5_buffer wire = {data, sizeof(data), 0};
    const uint8_t ping[] = {4, 0, 0, 0, 0, 0, 0, 0};
    CHECK(client_packet(0, 0x0200, (sp5_bytes){ping, 8}, &wire).status == SP5_OK);
    CHECK(sp5_control_receive(c, "10.1.2.3", false, 0, SP5_CONTROL_RELIABLE,
                              (sp5_bytes){data, wire.size}, 100)
              .status == SP5_ERROR);
    CHECK(sp5_control_accept(c, "10.1.2.3", false, 41, 100).status == SP5_ERROR);
    CHECK(sp5_control_accept(c, "8.8.8.8", false, 42, 100).status == SP5_ERROR);
    CHECK(sp5_control_accept(c, "10.1.2.3", false, 42, 100).status == SP5_OK);
    CHECK(sp5_control_receive(c, "10.1.2.4", false, 0, SP5_CONTROL_RELIABLE,
                              (sp5_bytes){data, wire.size}, 100)
              .status == SP5_ERROR);
    size_t allocations_before = crypto_allocations;
    CHECK(sp5_control_receive(c, "10.1.2.3", false, 0, SP5_CONTROL_RELIABLE,
                              (sp5_bytes){data, wire.size}, 200)
              .status == SP5_OK);
    CHECK(crypto_allocations == allocations_before);
    CHECK(sp5_control_receive(c, "10.1.2.3", false, 0, SP5_CONTROL_RELIABLE,
                              (sp5_bytes){data, wire.size}, 300)
              .status == SP5_ERROR);
    const uint8_t input[] = {0, 0, 0, 4, 0x18, 0, 0, 0};
    CHECK(client_packet(2, 0x0206, (sp5_bytes){input, 8}, &wire).status == SP5_OK);
    CHECK(sp5_control_receive(c, "10.1.2.3", false, 16, SP5_CONTROL_RELIABLE,
                              (sp5_bytes){data, wire.size}, 300)
              .status == SP5_OK);
    const uint8_t idr[] = {0, 0};
    CHECK(client_packet(1, 0x0302, (sp5_bytes){idr, 2}, &wire).status == SP5_OK);
    CHECK(sp5_control_receive(c, "10.1.2.3", false, 1, SP5_CONTROL_RELIABLE,
                              (sp5_bytes){data, wire.size}, 400)
              .status == SP5_OK); /* reordered channels */
    sp5_control_event event;
    CHECK(sp5_control_take(c, &event).status == SP5_OK && event.type == 0x0206 &&
          event.sequence == 2 && event.payload_size == 8);
    CHECK(sp5_control_take(c, &event).status == SP5_OK && event.type == 0x0302 &&
          event.sequence == 1);
    CHECK(sp5_control_take(c, &event).status == SP5_ERROR);
    /* mcc 41cedb8 Limelight-internal.h:56-65: gamepads 0x10..0x1f,
     * sensors 0x20..0x2f, channel count 0x30. Four LIVE pads are a
     * controller-manager limit, not a restriction to the first four wire IDs. */
    CHECK(client_packet(4, 0x0206, (sp5_bytes){input, 8}, &wire).status == SP5_OK);
    CHECK(sp5_control_receive(c, "10.1.2.3", false, 15, SP5_CONTROL_RELIABLE,
                              (sp5_bytes){data, wire.size}, 400)
              .status == SP5_ERROR);
    CHECK(sp5_control_receive(c, "10.1.2.3", false, 31, SP5_CONTROL_RELIABLE,
                              (sp5_bytes){data, wire.size}, 400)
              .status == SP5_OK);
    CHECK(sp5_control_take(c, &event).status == SP5_OK && event.channel == 31);
    CHECK(client_packet(5, 0x0206, (sp5_bytes){input, 8}, &wire).status == SP5_OK);
    CHECK(sp5_control_receive(c, "10.1.2.3", false, 48, SP5_CONTROL_UNRELIABLE,
                              (sp5_bytes){data, wire.size}, 400)
              .status == SP5_ERROR);
    CHECK(sp5_control_receive(c, "10.1.2.3", false, 47, SP5_CONTROL_UNRELIABLE,
                              (sp5_bytes){data, wire.size}, 400)
              .status == SP5_OK);
    CHECK(sp5_control_take(c, &event).status == SP5_OK && event.channel == 47);
    uint8_t fec[21] = {0};
    fec[3] = 7;
    fec[11] = 10;
    fec[13] = 2;
    fec[15] = 9;
    fec[17] = 1;
    fec[18] = 20;
    fec[20] = 1;
    CHECK(client_packet(3, 0x5502, (sp5_bytes){fec, sizeof(fec)}, &wire).status == SP5_OK);
    CHECK(sp5_control_receive(c, "10.1.2.3", false, 0, SP5_CONTROL_UNSEQUENCED,
                              (sp5_bytes){data, wire.size}, 500)
              .status == SP5_OK);
    CHECK(sp5_control_take(c, &event).status == SP5_OK);
    sp5_control_fec feedback;
    CHECK(sp5_control_fec_read(&event, &feedback).status == SP5_OK && feedback.frame_index == 7 &&
          feedback.data_total == 10 && feedback.data_received == 9 &&
          feedback.parity_received == 1);
    const uint8_t rumble[10] = {0xee, 0xff, 0xc0, 0, 0, 0, 10, 0, 20, 0};
    allocations_before = crypto_allocations;
    CHECK(sp5_control_send(c, 0x010b, (sp5_bytes){rumble, 10}).status == SP5_OK);
    CHECK(crypto_allocations == allocations_before);
    wire.size = 0;
    CHECK(sp5_control_take_wire(c, &wire).status == SP5_OK && wire.size == 38 && data[0] == 1);
    CHECK(sp5_control_tick(c, 10000499).status == SP5_OK);
    CHECK(sp5_control_tick(c, 10000500).status == SP5_ERROR);
    CHECK(f.admission_closed && f.released == 1 && f.reason == SP5_END_CLIENT_LOST &&
          sp5_session_read(session).end_reason == SP5_END_CLIENT_LOST);
    CHECK(!sp5_session_authorized(session, cert));
    CHECK(sp5_control_end(c, SP5_END_CLIENT_QUIT).status == SP5_OK && f.released == 1);
    sp5_control_destroy(c);
    CHECK(sp5_session_end(session, SP5_END_CLIENT_LOST).status == SP5_OK &&
          sp5_session_admit(session, cert, 2).status == SP5_OK);
    CHECK(sp5_session_configure(session, &host,
                                &(sp5_session_parameters){.mode_index = 0, .requested_kbps = 500})
                  .status == SP5_OK &&
          sp5_session_activate(session).status == SP5_OK);
    config.launch.session_id = 2;
    c = sp5_control_create(&config);
    CHECK(c != NULL);
    CHECK(sp5_control_accept(c, "10.1.2.3", false, 42, 100).status == SP5_OK);
    CHECK(sp5_authority_revoke(authority, cert).status == SP5_OK);
    CHECK(sp5_control_tick(c, 101).status == SP5_ERROR && f.released == 2 &&
          f.reason == SP5_END_REVOKED);
    CHECK(sp5_authority_pair(authority, cert).status == SP5_OK);
    CHECK(sp5_control_receive(c, "10.1.2.3", false, 0, SP5_CONTROL_RELIABLE,
                              (sp5_bytes){data, wire.size}, 102)
              .status == SP5_ERROR);
    sp5_control_destroy(c);
    CHECK(error_cases(authority, session, &host, config, &f) == 0);
    sp5_session_destroy(session);
    sp5_authority_destroy(authority);
    sp5_devices_destroy(devices);
    CHECK(input_owner_claim() == 0);
    CHECK(concurrent_end_owners() == 0);
    sp5_control_runtime *runtime = sp5_control_runtime_create();
    CHECK(runtime != NULL && sp5_control_runtime_create() == NULL);
    CHECK(sp5_control_pool_read(runtime).used_blocks == 0);
    /* Pinned ENet uses only fixed blocks, including exhaustion; its allocator
     * reports failure instead of aborting or falling back to malloc. */
    void *blocks[520];
    for (size_t i = 0; i < 520; ++i) {
        blocks[i] = enet_malloc(2048);
        CHECK(blocks[i] != NULL);
    }
    CHECK(enet_malloc(2048) == NULL);
    CHECK(sp5_control_pool_read(runtime).exhausted == 1 &&
          sp5_control_pool_read(runtime).high_water == 520);
    CHECK(sp5_control_runtime_destroy(runtime).status == SP5_ERROR);
    for (size_t i = 0; i < 520; ++i)
        enet_free(blocks[i]);
    CHECK(sp5_control_pool_read(runtime).used_blocks == 0);

    CHECK(sp5_control_runtime_destroy(runtime).status == SP5_OK);
    return 0;
}
