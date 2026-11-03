#include "../fakes/platform.h"
#include "session/lifecycle.h"
#include <stdio.h>
#include <string.h>
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            if (fputs(#c "\n", stderr) == EOF)                                                     \
                return 2;                                                                          \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static sp5_lifecycle_ops ops(sp5_fake_platform *f)
{
    return (sp5_lifecycle_ops){f->capture, f->encoder, f->audio, f->input, f->system};
}
static sp5_lifecycle_policy policy(sp5_fake_platform *f)
{
    sp5_lifecycle_policy p = {.mode_count = 1,
                              .media_bytes = 4096,
                              .core_bytes = 4096,
                              .streaming_cpu_us_per_10s = 100,
                              .idle_cpu_us_per_10s = 1,
                              .validity = SP5_VALIDITY_VALID,
                              .synthetic = true,
                              .evidence_ref = "unit:explicit-synthetic-budget",
                              .clock = f->clock,
                              .metrics_capacity = {16, 16, 8}};
    p.modes[0] = (sp5_host_mode){2, 2, 60, "synthetic/fake-only", SP5_SDR, 100, 200};
    p.metrics_context.synthetic = true;
    return p;
}
static int admission(void)
{
    sp5_fake_platform f;
    sp5_fake_init(&f);
    sp5_lifecycle_ops a = ops(&f);
    sp5_lifecycle *l = sp5_lifecycle_create(1, &a);
    CHECK(l != NULL);
    sp5_lifecycle_policy p = policy(&f);
    p.media_bytes = (size_t)129 * 1024 * 1024;
    CHECK(sp5_lifecycle_configure(l, &p).reason == SP5_REASON_CAPACITY);
    p = policy(&f);
    p.streaming_cpu_us_per_10s = 10000001;
    CHECK(sp5_lifecycle_configure(l, &p).reason == SP5_REASON_CAPACITY);
    p = policy(&f);
    p.validity = SP5_VALIDITY_UNAVAILABLE;
    CHECK(sp5_lifecycle_configure(l, &p).reason == SP5_REASON_UNVERIFIED);
    p = policy(&f);
    CHECK(sp5_lifecycle_configure(l, &p).status == SP5_OK);
    sp5_media_config m = {4, 2, 60, 1, "synthetic/fake-only", SP5_SDR, 100};
    CHECK(sp5_lifecycle_start(l, &m, 0).reason == SP5_REASON_UNSUPPORTED);
    CHECK(f.calls[SP5_FAKE_SYSTEM_PREPARE] == 0);
    sp5_lifecycle_destroy(l);
    l = sp5_lifecycle_create(2, &a);
    CHECK(l != NULL && sp5_lifecycle_configure(l, &p).status == SP5_OK);
    m.width = 2;
    CHECK(sp5_lifecycle_start(l, &m, 0).status == SP5_OK);
    CHECK(sp5_lifecycle_pool(l).size == 4096);
    sp5_buffer media;
    CHECK(sp5_lifecycle_media_buffer(l, 0, &media, 4097).reason == SP5_REASON_CAPACITY);
    CHECK(sp5_lifecycle_media_buffer(l, SIZE_MAX, &media, 1).reason == SP5_REASON_CAPACITY);
    CHECK(sp5_lifecycle_media_buffer(l, 4095, &media, 1).status == SP5_OK);
    CHECK(sp5_buffer_append(&media, (sp5_bytes){(const uint8_t *)"x", 1}).status == SP5_OK);
    CHECK(sp5_lifecycle_pool(l).data[4095] == 'x');
    CHECK(sp5_lifecycle_metrics(l) != NULL);
    sp5_metrics_view v;
    CHECK(sp5_metrics_snapshot(sp5_lifecycle_metrics(l), &v).status == SP5_OK);
    CHECK(v.session_id == 2 && !v.metrics_enabled && v.context.synthetic);
    CHECK(sp5_lifecycle_start(l, &m, 1).reason == SP5_REASON_CONFLICT);
    CHECK(sp5_lifecycle_contact(l, 2).status == SP5_OK);
    CHECK(sp5_lifecycle_tick(l, 1, false).reason == SP5_REASON_CLOCK);
    CHECK(sp5_lifecycle_read(l).end_reason == SP5_END_ERROR);
    CHECK(sp5_lifecycle_pool(l).size == 0);
    CHECK(strstr(sp5_lifecycle_explanation(l), "clock") != NULL);
    sp5_lifecycle_destroy(l);
    return 0;
}
static int independent(void)
{
    sp5_fake_platform f;
    sp5_fake_init(&f);
    sp5_lifecycle_ops a = ops(&f);
    sp5_lifecycle *l = sp5_lifecycle_create(3, &a);
    sp5_lifecycle_policy p = policy(&f);
    p.metrics_enabled = true;
    CHECK(l != NULL && sp5_lifecycle_configure(l, &p).status == SP5_OK);
    sp5_media_config m = {2, 2, 60, 1, "synthetic/fake-only", SP5_SDR, 100};
    CHECK(sp5_lifecycle_start(l, &m, 0).status == SP5_OK);
    /* Hold a media lease: direct input owner must still deliver and neutralize. */
    sp5_video_frame frame;
    CHECK(f.capture.acquire(&f, f.now, false, &frame).status == SP5_OK);
    sp5_control_event packet = {
        .type = 0x0206, .channel = 0x10, .delivery = SP5_CONTROL_RELIABLE, .payload_size = 34};
    const uint8_t pad[34] = {0, 0,   0, 30, 12, 0, 0, 0, 0x1a, 0, 0, 0,    1, 0, 0x14, 0,    0,
                             4, 127, 0, 0,  0,  0, 0, 0, 0,    0, 0, 0x9c, 0, 0, 0,    0x55, 0};
    const uint8_t arrival_packet[16] = {0, 0, 0, 12, 4,    0,    0,    0x55,
                                        0, 2, 0, 0,  0xff, 0xf7, 0x3f, 0};
    memcpy(packet.payload, arrival_packet, sizeof(arrival_packet));
    packet.payload_size = 16;
    sp5_input_evidence arrival_event;
    CHECK(sp5_lifecycle_input(l, &packet, f.now, 0, &arrival_event).status == SP5_OK);
    CHECK(!arrival_event.delivery_attempted);
    packet.payload_size = 34;
    memcpy(packet.payload, pad, sizeof(pad));
    sp5_timestamp received;
    CHECK(sp5_metrics_stamp(sp5_lifecycle_metrics(l), &received).status == SP5_OK);
    sp5_input_evidence e;
    CHECK(sp5_lifecycle_input(l, &packet, received, 1, &e).status == SP5_OK);
    CHECK(f.controllers[0].live && f.controllers[0].state.buttons == 0x400);
    CHECK(e.host_received.ticks == received.ticks && e.delivery_attempted);
    CHECK(sp5_lifecycle_stop_input(l, SP5_END_REVOKED).status == SP5_OK);
    CHECK(!sp5_lifecycle_input_open(l) && !f.controllers[0].live && f.capture_open);
    CHECK(sp5_lifecycle_input(l, &packet, received, 2, &e).reason == SP5_REASON_OWNERSHIP);
    CHECK(f.capture.release(&f, frame.lease_id).status == SP5_OK);
    f.fail_next[SP5_FAKE_SYSTEM_ROLLBACK] = SP5_REASON_PLATFORM;
    CHECK(sp5_lifecycle_end(l, SP5_END_ERROR).reason == SP5_REASON_PLATFORM);
    CHECK(sp5_lifecycle_cleanup_diagnostic(l) == SP5_REASON_PLATFORM);
    CHECK(sp5_lifecycle_read(l).end_reason == SP5_END_REVOKED);
    CHECK(sp5_lifecycle_end(l, SP5_END_ERROR).status == SP5_OK);
    CHECK(sp5_lifecycle_cleanup_diagnostic(l) == SP5_REASON_PLATFORM);
    CHECK(sp5_metrics_worker_step(sp5_lifecycle_metrics(l), 0, NULL).status == SP5_OK);
    sp5_metrics_view v;
    CHECK(sp5_metrics_snapshot(sp5_lifecycle_metrics(l), &v).status == SP5_OK);
    CHECK(v.whole_population[SP5_STAGE_INPUT_DELIVERY].seen == 1);
    sp5_lifecycle_destroy(l);
    return 0;
}
static sp5_result missing(void *context, sp5_store_file file, sp5_buffer *output)
{
    (void)context;
    (void)file;
    (void)output;
    return sp5_failure(SP5_REASON_WOULD_BLOCK);
}
static sp5_result saved(void *context, sp5_store_file file, sp5_bytes data)
{
    (void)context;
    (void)file;
    (void)data;
    return sp5_success();
}
static int bound_reservation(void)
{
    sp5_storage_ops storage = {NULL, missing, saved, NULL};
    sp5_devices *devices = sp5_devices_create(&storage);
    CHECK(devices != NULL && sp5_devices_load(devices).status == SP5_OK);
    sp5_authority *authority = sp5_authority_create(devices);
    sp5_session *session = sp5_session_create(authority);
    const uint8_t cert_data[] = "synthetic-unit-cert";
    sp5_bytes cert = {cert_data, sizeof(cert_data) - 1};
    CHECK(sp5_authority_pair(authority, cert).status == SP5_OK);
    CHECK(sp5_session_admit(session, cert, 4).status == SP5_OK);
    CHECK(sp5_session_admitted_id(session, cert, 4));
    CHECK(!sp5_session_admitted_id(session, cert, 3));
    CHECK(!sp5_session_authorized_id(session, cert, 4));
    sp5_fake_platform f;
    sp5_fake_init(&f);
    sp5_lifecycle_ops a = ops(&f);
    sp5_lifecycle *l = sp5_lifecycle_create(4, &a);
    sp5_lifecycle_policy p = policy(&f);
    CHECK(l != NULL && sp5_lifecycle_configure(l, &p).status == SP5_OK);
    CHECK(sp5_lifecycle_bind(l, session, cert).status == SP5_OK);
    sp5_host host;
    CHECK(sp5_host_init(&host, "unit", "13.60", "192.168.1.1").status == SP5_OK);
    host.supported_mode_count = 1;
    host.supported_modes[0] = p.modes[0];
    sp5_session_parameters parameters = {.mode_index = 0, .requested_kbps = 100};
    CHECK(sp5_session_configure(session, &host, &parameters).status == SP5_OK);
    sp5_rtsp_config rtsp_config = {.host = &host,
                                   .devices = devices,
                                   .session = session,
                                   .verified_certificate = cert,
                                   .peer = "192.168.1.1",
                                   .audio_port = 49000,
                                   .video_port = 48998,
                                   .control_port = 48999,
                                   .launch = {.session_id = 4, .width = 2, .height = 2, .fps = 60}};
    sp5_rtsp *rtsp = sp5_rtsp_create(&rtsp_config);
    CHECK(rtsp != NULL && !sp5_rtsp_playing(rtsp) && !sp5_rtsp_playing(NULL));
    uint8_t reply[SP5_RTSP_LIMIT];
    sp5_buffer response = {reply, sizeof(reply), 0};
    const char invalid_play[] = "PLAY / RTSP/1.0\r\nCSeq: 1\r\nSession: DEADBEEFCAFE\r\n\r\n";
    sp5_result rejected =
        sp5_rtsp_exchange(rtsp, "192.168.1.1", false,
                          (sp5_bytes){(const uint8_t *)invalid_play, sizeof(invalid_play) - 1}, 0,
                          (sp5_optional_u64){0}, &response);
    CHECK(rejected.status == SP5_ERROR && rejected.reason == SP5_REASON_INVALID_ARGUMENT);
    CHECK(response.size >= 12 && memcmp(reply, "RTSP/1.0 455", 12) == 0 && !sp5_rtsp_playing(rtsp));
    sp5_rtsp_destroy(rtsp);
    sp5_media_config m = {2, 2, 60, 1, "synthetic/fake-only", SP5_SDR, 100};
    CHECK(sp5_lifecycle_start(l, &m, 0).status == SP5_OK);
    CHECK(sp5_session_read(session).state == SP5_SESSION_NEGOTIATING);
    CHECK(sp5_lifecycle_metrics(l) != NULL && sp5_lifecycle_pool(l).size == 4096);
    CHECK(sp5_session_activate(session).status == SP5_OK);
    CHECK(sp5_authority_revoke(authority, cert).status == SP5_OK);
    CHECK(!sp5_session_admitted_id(session, cert, 4));
    CHECK(sp5_lifecycle_tick(l, 1, false).status == SP5_OK);
    CHECK(sp5_lifecycle_read(l).end_reason == SP5_END_REVOKED);
    sp5_lifecycle_destroy(l);
    sp5_session_destroy(session);
    sp5_authority_destroy(authority);
    sp5_devices_destroy(devices);
    return 0;
}
static int encryption_capability(void)
{
    sp5_host host;
    CHECK(sp5_host_init(&host, "unit", "13.60", "192.168.1.1").status == SP5_OK);
    host.encryption_supported = 5;
    uint8_t data[512] = {0};
    sp5_buffer sdp = {data, sizeof(data) - 1, 0};
    CHECK(sp5_sdp_describe(&host, &sdp).status == SP5_OK);
    CHECK(strstr((const char *)data, "encryptionSupported:5\r\n") != NULL);
    host.supported_mode_count = 1;
    host.supported_modes[0] = (sp5_host_mode){2, 2, 60, "h264", SP5_SDR, 100, 200};
    char request[] =
        "v=0\r\na=x-nv-video[0].clientViewportWd:2\r\n"
        "a=x-nv-video[0].clientViewportHt:2\r\na=x-nv-video[0].maxFPS:60\r\n"
        "a=x-ml-video.configuredBitrateKbps:100\r\na=x-nv-general.useReliableUdp:13\r\n"
        "a=x-ss-general.encryptionEnabled:7\r\na=x-nv-video[0].packetSize:1392\r\n"
        "a=x-nv-audio.surround.numChannels:2\r\na=x-nv-audio.surround.channelMask:3\r\n";
    sp5_negotiated chosen;
    sp5_bytes wire = {(const uint8_t *)request, sizeof(request) - 1};
    host.encryption_supported = 0;
    CHECK(sp5_sdp_negotiate(wire, &host, &chosen).status == SP5_OK);
    host.encryption_supported = 5;
    CHECK(sp5_sdp_negotiate(wire, &host, &chosen).reason == SP5_REASON_UNSUPPORTED);
    char *flags = strstr(request, "encryptionEnabled:7");
    CHECK(flags != NULL);
    flags[strlen("encryptionEnabled:")] = '5';
    CHECK(sp5_sdp_negotiate(wire, &host, &chosen).status == SP5_OK && chosen.encryption_flags == 5);

    host.encryption_supported = 8;
    sdp.size = 0;
    CHECK(sp5_sdp_describe(&host, &sdp).reason == SP5_REASON_INVALID_ARGUMENT);
    return 0;
}
static sp5_result advancing_now(void *context, sp5_timestamp *timestamp)
{
    sp5_fake_platform *f = context;
    f->now.ticks += 10;
    *timestamp = f->now;
    return sp5_success();
}
static int setup_clock(void)
{
    sp5_fake_platform f;
    sp5_fake_init(&f);
    sp5_lifecycle_ops a = ops(&f);
    sp5_lifecycle *l = sp5_lifecycle_create(5, &a);
    sp5_lifecycle_policy p = policy(&f);
    p.metrics_enabled = true;
    p.clock.now = advancing_now;
    CHECK(l != NULL && sp5_lifecycle_configure(l, &p).status == SP5_OK);
    sp5_media_config m = {2, 2, 60, 1, "synthetic/fake-only", SP5_SDR, 100};
    CHECK(sp5_lifecycle_start(l, &m, 0).status == SP5_OK);
    CHECK(sp5_lifecycle_end(l, SP5_END_ERROR).status == SP5_OK);
    CHECK(sp5_lifecycle_cleanup_diagnostic(l) == SP5_REASON_NONE);
    sp5_lifecycle_destroy(l);
    return 0;
}
/* A management-owned lifecycle end records its one reason in session history
 * even while adapter cleanup fails and must be retried (FR-039). */
static int bound_cleanup_history(void)
{
    sp5_storage_ops storage = {NULL, missing, saved, NULL};
    sp5_devices *devices = sp5_devices_create(&storage);
    CHECK(devices != NULL && sp5_devices_load(devices).status == SP5_OK);
    sp5_authority *authority = sp5_authority_create(devices);
    sp5_session *session = sp5_session_create(authority);
    const uint8_t cert_data[] = "synthetic-unit-cert";
    sp5_bytes cert = {cert_data, sizeof(cert_data) - 1};
    CHECK(session != NULL && sp5_authority_pair(authority, cert).status == SP5_OK);
    CHECK(sp5_session_admit(session, cert, 6).status == SP5_OK);
    sp5_fake_platform f;
    sp5_fake_init(&f);
    sp5_lifecycle_ops a = ops(&f);
    sp5_lifecycle *l = sp5_lifecycle_create(6, &a);
    sp5_lifecycle_policy p = policy(&f);
    CHECK(l != NULL && sp5_lifecycle_configure(l, &p).status == SP5_OK);
    CHECK(sp5_lifecycle_bind(l, session, cert).status == SP5_OK);
    sp5_host host;
    CHECK(sp5_host_init(&host, "unit", "13.60", "192.168.1.1").status == SP5_OK);
    host.supported_mode_count = 1;
    host.supported_modes[0] = p.modes[0];
    sp5_session_parameters parameters = {.mode_index = 0, .requested_kbps = 100};
    CHECK(sp5_session_configure(session, &host, &parameters).status == SP5_OK);
    sp5_media_config m = {2, 2, 60, 1, "synthetic/fake-only", SP5_SDR, 100};
    CHECK(sp5_lifecycle_start(l, &m, 0).status == SP5_OK);
    CHECK(sp5_session_activate(session).status == SP5_OK);
    f.fail_next[SP5_FAKE_AUDIO_CLOSE] = SP5_REASON_PLATFORM;
    CHECK(sp5_lifecycle_tick(l, 1, true).status == SP5_ERROR);
    CHECK(sp5_session_read(session).end_reason == SP5_END_CLIENT_LOST);
    sp5_session_end_record record;
    CHECK(sp5_authority_end_count(authority) == 1 &&
          sp5_authority_end_at(authority, 0, &record).status == SP5_OK && record.session_id == 6 &&
          record.reason == SP5_END_CLIENT_LOST);
    /* Retried cleanup completes without a second record or a different reason. */
    CHECK(sp5_lifecycle_end(l, SP5_END_HOST_STOPPING).status == SP5_OK);
    CHECK(sp5_session_read(session).state == SP5_SESSION_ENDED &&
          sp5_session_read(session).end_reason == SP5_END_CLIENT_LOST);
    CHECK(sp5_authority_end_count(authority) == 1);
    sp5_lifecycle_destroy(l);
    sp5_session_destroy(session);
    sp5_authority_destroy(authority);
    sp5_devices_destroy(devices);
    return 0;
}
int main(void)
{
    return admission() || independent() || bound_reservation() || encryption_capability() ||
           setup_clock() || bound_cleanup_history();
}
