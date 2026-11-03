/* Synthetic management/input interleavings; no console or wire conformance. */
#include "session/session.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            if (fputs(__FILE__ ": " #x "\n", stderr) == EOF) {                                     \
                return 2;                                                                          \
            }                                                                                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

typedef struct {
    uint8_t bytes[40000];
    size_t size;
    bool fail;
    sp5_session *input;
    bool input_accepted_during_write;
} fake_storage;
static sp5_bytes text(const char *s)
{
    return (sp5_bytes){(const uint8_t *)s, strlen(s)};
}
static sp5_result read_file(void *context, sp5_store_file file, sp5_buffer *out)
{
    fake_storage *storage = context;
    if (file != SP5_STORE_DEVICES || storage->size == 0) {
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    }
    return sp5_buffer_append(out, (sp5_bytes){storage->bytes, storage->size});
}
static sp5_result replace_file(void *context, sp5_store_file file, sp5_bytes bytes)
{
    fake_storage *storage = context;
    if (storage->input != NULL) {
        storage->input_accepted_during_write = sp5_session_authorized(storage->input, text("A"));
    }
    if (storage->fail || file != SP5_STORE_DEVICES || bytes.size > sizeof(storage->bytes)) {
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    memcpy(storage->bytes, bytes.data, bytes.size);
    storage->size = bytes.size;
    return sp5_success();
}
static sp5_host fixture_host(void)
{
    /* Explicit fake capability; production host_init still offers zero modes. */
    sp5_host host = {0};
    host.supported_mode_count = 2;
    host.supported_modes[0] = (sp5_host_mode){1280, 720, 60, "fixture-codec", SP5_SDR, 100, 1000};
    host.supported_modes[1] = host.supported_modes[0];
    host.supported_modes[1].dynamic_range = SP5_HDR_PQ_BT2020;
    return host;
}
static int admission(void)
{
    fake_storage storage = {0};
    sp5_storage_ops ops = {&storage, read_file, replace_file, NULL};
    sp5_devices *devices = sp5_devices_create(&ops);
    CHECK(devices != NULL && sp5_devices_load(devices).status == SP5_OK);
    CHECK(sp5_authority_create(NULL) == NULL);
    sp5_authority *authority = sp5_authority_create(devices);
    CHECK(authority != NULL);
    sp5_session *session = sp5_session_create(authority);
    sp5_session *competitor = sp5_session_create(authority);
    CHECK(session != NULL && competitor != NULL);
    CHECK(sp5_session_admit(session, text("A"), 1).status == SP5_ERROR);
    CHECK(sp5_devices_pair(devices, text("A"), text("Tablet"), 10).status == SP5_OK);
    CHECK(sp5_devices_pair(devices, text("B"), text("Phone"), 11).status == SP5_OK);
    CHECK(sp5_session_admit(session, text("A"), 0).status == SP5_ERROR);
    sp5_host host = fixture_host();
    CHECK(sp5_session_admit(session, text("A"), 1).status == SP5_OK);
    CHECK(sp5_session_activate(session).status == SP5_ERROR); /* no negotiated mode */
    CHECK(sp5_session_configure(session, &host,
                                &(sp5_session_parameters){.mode_index = 0,
                                                          .requested_kbps = 99,
                                                          .started_us = 500,
                                                          .wall_unix_us = {true, 10000}})
              .status == SP5_OK);
    CHECK(sp5_session_admit(competitor, text("B"), 2).status == SP5_ERROR);
    sp5_session_view view = sp5_session_read(session);
    CHECK(view.id == 1 && view.state == SP5_SESSION_NEGOTIATING && view.mode_configured);
    CHECK(view.started_us == 500 && view.started_wall_unix_us.present &&
          view.started_wall_unix_us.value == 10000);
    CHECK(strcmp(view.device_name, "Tablet") == 0 && strlen(view.device_id) == 64);
    CHECK(view.width == 1280 && view.height == 720 && view.fps == 60 &&
          strcmp(view.codec, "fixture-codec") == 0);
    CHECK(view.requested_bitrate_kbps == 99 && view.chosen_bitrate_kbps == 100 &&
          view.bitrate_adjustment == SP5_BITRATE_RAISED);
    CHECK(!view.measured_bitrate_bps.present && view.controller_count == 0 && !view.has_end_reason);
    CHECK(sp5_session_activate(session).status == SP5_OK);
    CHECK(!sp5_session_authorized(session, text("claimed-id")));
    CHECK(sp5_session_authorized(session, text("A")));
    CHECK(sp5_session_configure(
              session, &host,
              &(sp5_session_parameters){
                  .mode_index = 0, .requested_kbps = 100, .started_us = 0, .wall_unix_us = {0}})
              .status == SP5_ERROR);
    CHECK(sp5_session_set_dynamic_range(
              session, (sp5_session_range_change){.range = SP5_HDR_PQ_BT2020, .observed_us = 501})
              .status == SP5_OK);
    view = sp5_session_read(session);
    CHECK(view.id == 1 && view.state == SP5_SESSION_ACTIVE &&
          view.dynamic_range == SP5_HDR_PQ_BT2020);
    CHECK(view.dynamic_range_changed_us == 501 && view.dynamic_range_sequence == 1);
    CHECK(sp5_session_set_dynamic_range(
              session, (sp5_session_range_change){.range = SP5_SDR, .observed_us = 500})
              .reason == SP5_REASON_CLOCK);
    CHECK(sp5_session_set_dynamic_range(
              session, (sp5_session_range_change){.range = SP5_SDR, .observed_us = 502})
              .status == SP5_OK);
    sp5_session_range_change change;
    CHECK(sp5_session_take_range_change(session, &change).status == SP5_OK);
    CHECK(change.range == SP5_HDR_PQ_BT2020 && change.observed_us == 501 && change.sequence == 1);
    CHECK(sp5_session_take_range_change(session, &change).status == SP5_OK);
    CHECK(change.range == SP5_SDR && change.observed_us == 502 && change.sequence == 2);
    CHECK(sp5_session_take_range_change(session, &change).reason == SP5_REASON_WOULD_BLOCK);
    for (unsigned i = 0; i < SP5_SESSION_RANGE_EVENT_CAPACITY; ++i) {
        CHECK(sp5_session_set_dynamic_range(
                  session,
                  (sp5_session_range_change){.range = (i % 2 == 0) ? SP5_HDR_PQ_BT2020 : SP5_SDR,
                                             .observed_us = 503 + (uint64_t)i})
                  .status == SP5_OK);
    }
    CHECK(sp5_session_set_dynamic_range(
              session, (sp5_session_range_change){.range = SP5_HDR_PQ_BT2020, .observed_us = 507})
              .reason == SP5_REASON_WOULD_BLOCK);
    CHECK(sp5_session_read(session).id == 1 &&
          sp5_session_read(session).state == SP5_SESSION_ACTIVE);
    CHECK(sp5_session_read(session).dynamic_range == SP5_SDR);
    CHECK(sp5_session_begin_end(session, SP5_END_CLIENT_QUIT, SP5_REASON_NONE).status == SP5_OK);
    CHECK(!sp5_session_authorized(session, text("A")) &&
          sp5_session_read(session).state == SP5_SESSION_ENDING);
    CHECK(sp5_session_admit(competitor, text("B"), 2).status == SP5_ERROR);
    CHECK(sp5_session_end(session, SP5_END_ERROR).status == SP5_OK);
    CHECK(sp5_session_read(session).end_reason == SP5_END_CLIENT_QUIT);
    CHECK(sp5_authority_end_count(authority) == 1);
    sp5_session_end_record ended;
    CHECK(sp5_authority_end_at(authority, 0, &ended).status == SP5_OK);
    CHECK(ended.session_id == 1 && ended.reason == SP5_END_CLIENT_QUIT);
    CHECK(sp5_session_end(session, SP5_END_REVOKED).status == SP5_OK);
    CHECK(sp5_session_admit(session, text("A"), 1).status == SP5_ERROR);
    CHECK(sp5_session_admit(competitor, text("B"), 2).status == SP5_OK);
    CHECK(sp5_session_fail(competitor, SP5_REASON_PLATFORM).status == SP5_OK);
    CHECK(sp5_session_read(competitor).diagnostic == SP5_REASON_PLATFORM);
    CHECK(sp5_session_read(competitor).end_reason == SP5_END_ERROR);
    CHECK(sp5_authority_end_count(authority) == 2);
    CHECK(sp5_authority_end_at(authority, 0, &ended).status == SP5_OK && ended.session_id == 1);
    sp5_session_destroy(competitor);
    sp5_session_destroy(session);
    sp5_authority_destroy(authority);
    sp5_devices_destroy(devices);
    return 0;
}
static int bounds_and_leases(void)
{
    fake_storage storage = {0};
    sp5_storage_ops ops = {&storage, read_file, replace_file, NULL};
    sp5_devices *devices = sp5_devices_create(&ops);
    CHECK(devices != NULL && sp5_devices_load(devices).status == SP5_OK);
    sp5_authority *authority = sp5_authority_create(devices);
    CHECK(authority != NULL && sp5_authority_pair(authority, text("A")).status == SP5_OK);
    CHECK(sp5_authority_pair(authority, text("B")).status == SP5_OK);
    sp5_session *session = sp5_session_create(authority);
    CHECK(session != NULL);
    sp5_host host = fixture_host();
    const uint32_t requested[] = {1, 100, 101, 1000, 1001, UINT32_MAX};
    const uint32_t chosen[] = {100, 100, 101, 1000, 1000, 1000};
    const sp5_bitrate_adjustment adjustments[] = {SP5_BITRATE_RAISED,  SP5_BITRATE_NONE,
                                                  SP5_BITRATE_NONE,    SP5_BITRATE_NONE,
                                                  SP5_BITRATE_LOWERED, SP5_BITRATE_LOWERED};
    for (size_t i = 0; i < sizeof(requested) / sizeof(requested[0]); ++i) {
        CHECK(sp5_session_admit(session, text("A"), i + 1).status == SP5_OK);
        CHECK(sp5_session_configure(
                  session, &host,
                  &(sp5_session_parameters){
                      .mode_index = 2, .requested_kbps = 1, .started_us = 50, .wall_unix_us = {0}})
                  .status == SP5_ERROR);
        CHECK(sp5_session_configure(
                  session, &host,
                  &(sp5_session_parameters){
                      .mode_index = 0, .requested_kbps = 0, .started_us = 50, .wall_unix_us = {0}})
                  .status == SP5_ERROR);
        CHECK(sp5_session_configure(session, &host,
                                    &(sp5_session_parameters){.mode_index = 0,
                                                              .requested_kbps = requested[i],
                                                              .started_us = 50,
                                                              .wall_unix_us = {0}})
                  .status == SP5_OK);
        sp5_session_view view = sp5_session_read(session);
        CHECK(view.requested_bitrate_kbps == requested[i] &&
              view.chosen_bitrate_kbps == chosen[i] && view.bitrate_adjustment == adjustments[i]);
        CHECK(sp5_session_activate(session).status == SP5_OK);
        CHECK(sp5_session_end(session, SP5_END_CLIENT_QUIT).status == SP5_OK);
    }
    CHECK(sp5_session_admit(session, text("A"), 10).status == SP5_OK);
    CHECK(sp5_session_configure(
              session, &host,
              &(sp5_session_parameters){
                  .mode_index = 0, .requested_kbps = 100, .started_us = 50, .wall_unix_us = {0}})
              .status == SP5_OK);
    sp5_device_lease stale = sp5_devices_lease(devices, text("A"));
    sp5_device_lease other = sp5_devices_lease(devices, text("B"));
    CHECK(sp5_devices_rename(devices, (sp5_device_name){text("B"), text("Other")}).status ==
          SP5_OK);
    CHECK(sp5_device_lease_valid(stale) && sp5_device_lease_valid(other));
    CHECK(sp5_session_activate(session).status == SP5_OK);
    storage.input = session;
    storage.fail = true;
    CHECK(sp5_authority_revoke(authority, text("A")).status == SP5_ERROR);
    CHECK(!storage.input_accepted_during_write && !sp5_device_lease_valid(stale));
    CHECK(sp5_device_lease_valid(other));
    CHECK(sp5_session_activate(session).status == SP5_ERROR);
    CHECK(sp5_session_end(session, SP5_END_REVOKED).status == SP5_OK);
    storage.fail = false;
    CHECK(sp5_authority_pair(authority, text("A")).status == SP5_OK);
    CHECK(!sp5_device_lease_valid(stale)); /* re-pair cannot revive queued input */
    CHECK(sp5_device_lease_valid(other));  /* registry compaction cannot invalidate B */
    CHECK(sp5_devices_load(devices).status == SP5_OK);
    CHECK(!sp5_device_lease_valid(other)); /* reload invalidates every old lease */
    sp5_session *handles[SP5_SESSION_HANDLE_CAPACITY - 1];
    for (size_t i = 0; i < SP5_SESSION_HANDLE_CAPACITY - 1; ++i) {
        handles[i] = sp5_session_create(authority);
        CHECK(handles[i] != NULL);
    }
    CHECK(sp5_session_create(authority) == NULL);
    for (size_t i = 0; i < SP5_SESSION_HANDLE_CAPACITY - 1; ++i) {
        sp5_session_destroy(handles[i]);
    }
    sp5_session_destroy(session);
    sp5_authority_destroy(authority);
    sp5_devices_destroy(devices);
    return 0;
}
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool ready, revoked, accepted, error;
    const sp5_session *session;
} input_race;

static void *queued_input(void *context)
{
    input_race *race = context;
    if (pthread_mutex_lock(&race->mutex) != 0) {
        race->error = true;
        return NULL;
    }
    race->ready = true;
    if (pthread_cond_signal(&race->condition) != 0) {
        race->error = true;
    }
    while (!race->revoked && !race->error) {
        if (pthread_cond_wait(&race->condition, &race->mutex) != 0) {
            race->error = true;
        }
    }
    if (pthread_mutex_unlock(&race->mutex) != 0) {
        race->error = true;
    }
    /* Queued before revoke, consumed after re-pair, concurrently with unrelated
     * registry mutations: an old session must never regain input authority. */
    for (unsigned i = 0; i < 10000; ++i) {
        if (sp5_session_authorized(race->session, text("A"))) {
            race->accepted = true;
        }
    }
    return NULL;
}

static int threaded_repair(void)
{
    fake_storage storage = {0};
    sp5_storage_ops ops = {&storage, read_file, replace_file, NULL};
    sp5_devices *devices = sp5_devices_create(&ops);
    CHECK(devices != NULL && sp5_devices_load(devices).status == SP5_OK);
    sp5_authority *authority = sp5_authority_create(devices);
    CHECK(authority != NULL && sp5_authority_pair(authority, text("A")).status == SP5_OK);
    CHECK(sp5_authority_pair(authority, text("B")).status == SP5_OK);
    sp5_session *session = sp5_session_create(authority);
    CHECK(session != NULL && sp5_session_admit(session, text("A"), 1).status == SP5_OK);
    sp5_host host = fixture_host();
    CHECK(sp5_session_configure(
              session, &host,
              &(sp5_session_parameters){
                  .mode_index = 0, .requested_kbps = 100, .started_us = 0, .wall_unix_us = {0}})
              .status == SP5_OK);
    CHECK(sp5_session_activate(session).status == SP5_OK);
    input_race race = {.mutex = PTHREAD_MUTEX_INITIALIZER,
                       .condition = PTHREAD_COND_INITIALIZER,
                       .session = session};
    pthread_t reader;
    CHECK(pthread_create(&reader, NULL, queued_input, &race) == 0);
    CHECK(pthread_mutex_lock(&race.mutex) == 0);
    while (!race.ready) {
        CHECK(pthread_cond_wait(&race.condition, &race.mutex) == 0);
    }
    CHECK(pthread_mutex_unlock(&race.mutex) == 0);
    CHECK(sp5_session_authorized(session, text("A")));
    CHECK(sp5_authority_revoke(authority, text("A")).status == SP5_OK);
    CHECK(sp5_authority_pair(authority, text("A")).status == SP5_OK);
    CHECK(pthread_mutex_lock(&race.mutex) == 0);
    race.revoked = true;
    CHECK(pthread_cond_signal(&race.condition) == 0);
    CHECK(pthread_mutex_unlock(&race.mutex) == 0);
    for (unsigned i = 0; i < 16; ++i) {
        CHECK(sp5_devices_rename(devices, (sp5_device_name){text("B"), text("Other")}).status ==
              SP5_OK);
    }
    CHECK(pthread_join(reader, NULL) == 0 && !race.accepted && !race.error);
    CHECK(pthread_cond_destroy(&race.condition) == 0 && pthread_mutex_destroy(&race.mutex) == 0);
    CHECK(sp5_session_end(session, SP5_END_REVOKED).status == SP5_OK);
    /* Bounded recent diagnostics survive replacement; every terminal reason is
     * assigned once, including failed negotiations and repeated end calls. */
    for (uint64_t id = 2; id <= 22; ++id) {
        CHECK(sp5_session_admit(session, text("A"), id).status == SP5_OK);
        CHECK(sp5_session_end(session, (sp5_end_reason)(id % 6)).status == SP5_OK);
    }
    CHECK(sp5_authority_end_count(authority) == SP5_SESSION_END_HISTORY_CAPACITY);
    sp5_session_end_record record;
    CHECK(sp5_authority_end_at(authority, 0, &record).status == SP5_OK && record.session_id == 7);
    CHECK(sp5_authority_end_at(authority, 15, &record).status == SP5_OK && record.session_id == 22);
    CHECK(sp5_authority_end_at(authority, 16, &record).status == SP5_ERROR);
    sp5_session_destroy(session);
    sp5_authority_destroy(authority);
    sp5_devices_destroy(devices);
    return 0;
}

int main(void)
{
    int result = admission();
    if (result == 0) {
        result = bounds_and_leases();
    }
    return result == 0 ? threaded_repair() : result;
}
