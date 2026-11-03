#define _POSIX_C_SOURCE 200809L
#include "../fakes/platform.h"
#include "controller.h"
#include "protocol/audio.h"
#include "protocol/video.h"
#include <arpa/inet.h>
#include <enet/enet.h>
#include <ifaddrs.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            if (fputs("dependency allocations: " #c "\n", stderr) == EOF)                          \
                return 2;                                                                          \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

enum { ITERATIONS = 256 };
typedef struct {
    size_t allocations, reallocations, frees;
} counts;
static counts measured;
static bool measuring;
static void *allocate(size_t size, const char *file, int line)
{
    (void)file;
    (void)line;
    if (measuring)
        ++measured.allocations;
    return malloc(size);
}
static void *reallocate(void *pointer, size_t size, const char *file, int line)
{
    (void)file;
    (void)line;
    if (measuring)
        ++measured.reallocations;
    return realloc(pointer, size);
}
static void release(void *pointer, const char *file, int line)
{
    (void)file;
    (void)line;
    if (measuring && pointer != NULL)
        ++measured.frees;
    free(pointer);
}
static void begin(void)
{
    measured = (counts){0};
    measuring = true;
}
static int end(const char *scenario)
{
    measuring = false;
    CHECK(printf("%s openssl_malloc=%zu openssl_realloc=%zu openssl_nonnull_free=%zu\n", scenario,
                 measured.allocations, measured.reallocations, measured.frees) > 0);
    CHECK(measured.allocations == 0 && measured.reallocations == 0 && measured.frees == 0);
    return 0;
}
static int calibration(void)
{
    begin();
    void *p = OPENSSL_malloc(8);
    CHECK(p != NULL);
    p = OPENSSL_realloc(p, 16);
    CHECK(p != NULL);
    OPENSSL_free(p);
    measuring = false;
    CHECK(measured.allocations == 1 && measured.reallocations == 1 && measured.frees == 1);
    CHECK(puts("hook_calibration malloc=1 realloc=1 nonnull_free=1") >= 0);
    return 0;
}
static int media(bool inject)
{
    sp5_video_config video_config = {
        .packet_size = 64, .fec_percent = 20, .minimum_parity = 2, .encrypted = true};
    sp5_audio_config audio_config = {
        .sample_rate = 48000, .channels = 2, .duration_ms = 5, .opus_size = 16, .encrypted = true};
    sp5_video *video = sp5_video_create(&video_config);
    sp5_audio *audio = sp5_audio_create(&audio_config);
    CHECK(video != NULL && audio != NULL);
    uint8_t source[100] = {1}, wire_data[SP5_VIDEO_WIRE_LIMIT];
    sp5_buffer wire = {wire_data, sizeof(wire_data), 0};
    size_t video_packets = 0, audio_packets = 0;
    begin();
    for (uint32_t i = 0; i < ITERATIONS; ++i) {
        sp5_video_frame_input frame = {
            .bitstream = {source, sizeof(source)}, .frame_index = i, .rtp_timestamp = i * 1500};
        CHECK(sp5_video_begin(video, &frame).status == SP5_OK);
        CHECK(sp5_video_begin(video, &frame).reason == SP5_REASON_OWNERSHIP);
        wire.capacity = 1;
        CHECK(sp5_video_next(video, &wire).reason == SP5_REASON_CAPACITY);
        wire.capacity = sizeof(wire_data);
        unsigned packets = 0;
        sp5_result result;
        while ((result = sp5_video_next(video, &wire)).status == SP5_OK) {
            CHECK(++packets <= 8 && wire.size > 32);
            ++video_packets;
        }
        CHECK(result.reason == SP5_REASON_WOULD_BLOCK && packets > 0);
        CHECK(sp5_audio_submit(audio, (sp5_bytes){source, 15}).status == SP5_ERROR);
        CHECK(sp5_audio_submit(audio, (sp5_bytes){source, 16}).status == SP5_OK);
        CHECK(sp5_audio_submit(audio, (sp5_bytes){source, 16}).reason == SP5_REASON_WOULD_BLOCK);
        wire.capacity = 1;
        CHECK(sp5_audio_next(audio, &wire).reason == SP5_REASON_CAPACITY);
        wire.capacity = sizeof(wire_data);
        packets = 0;
        while ((result = sp5_audio_next(audio, &wire)).status == SP5_OK) {
            CHECK(++packets <= 3 && wire.size >= 44);
            ++audio_packets;
        }
        CHECK(result.reason == SP5_REASON_WOULD_BLOCK && packets == (i % 4 == 3 ? 3U : 1U));
    }
    /* An opt-in negative control proves that the same zero-call assertion fires. */
    if (inject) {
        void *p = OPENSSL_malloc(8);
        CHECK(p != NULL);
        OPENSSL_free(p);
    }
    CHECK(end("encrypted_media_with_retries") == 0);
    CHECK(printf("media frames=%u video_packets=%zu audio_packets=%zu\n", ITERATIONS, video_packets,
                 audio_packets) > 0);
    sp5_audio_destroy(audio);
    sp5_video_destroy(video);
    return 0;
}
static sp5_result read_file(void *context, sp5_store_file file, sp5_buffer *out)
{
    (void)context;
    (void)file;
    (void)out;
    return sp5_failure(SP5_REASON_WOULD_BLOCK);
}
static sp5_result write_file(void *context, sp5_store_file file, sp5_bytes bytes)
{
    (void)context;
    (void)file;
    (void)bytes;
    return sp5_success();
}
static sp5_result release_input(void *context, sp5_end_reason reason)
{
    (void)reason;
    return sp5_controller_close(context);
}
/* Independent fixture envelope: protocol-notes section 9 CONTROL_V2; generated
 * before measurement so client-side setup cannot contaminate host counters. */
static int prepare(uint8_t wire[ITERATIONS + 1][62])
{
    EVP_CIPHER_CTX *cipher = EVP_CIPHER_CTX_new();
    CHECK(cipher != NULL);
    uint8_t key[16] = {0};
    for (uint32_t i = 0; i <= ITERATIONS; ++i) {
        uint8_t iv[12] = {0}, plain[38] = {6, 2, 34, 0};
        uint8_t *pad = plain + 4;
        pad[3] = 30;
        pad[4] = 12;
        pad[8] = 0x1a;
        pad[12] = 1;
        pad[14] = 0x14;
        pad[16] = (uint8_t)(i & 1U);
        pad[18] = (uint8_t)i;
        pad[28] = 0x9c;
        pad[30] = 0x10;
        pad[32] = 0x55;
        wire[i][0] = 1;
        wire[i][2] = 58;
        for (unsigned j = 0; j < 4; ++j)
            iv[j] = wire[i][4 + j] = (uint8_t)(i >> (8 * j));
        iv[10] = iv[11] = 'C';
        int n = 0, tail = 0;
        CHECK(EVP_EncryptInit_ex(cipher, EVP_aes_128_gcm(), NULL, key, iv) == 1 &&
              EVP_EncryptUpdate(cipher, wire[i] + 24, &n, plain, sizeof(plain)) == 1 &&
              EVP_EncryptFinal_ex(cipher, wire[i] + 24 + n, &tail) == 1 &&
              n + tail == (int)sizeof(plain) &&
              EVP_CIPHER_CTX_ctrl(cipher, EVP_CTRL_GCM_GET_TAG, 16, wire[i] + 8) == 1);
    }
    EVP_CIPHER_CTX_free(cipher);
    return 0;
}
static int control_input(void)
{
    sp5_storage_ops ops = {NULL, read_file, write_file, NULL};
    sp5_devices *devices = sp5_devices_create(&ops);
    CHECK(devices != NULL && sp5_devices_load(devices).status == SP5_OK);
    sp5_authority *authority = sp5_authority_create(devices);
    sp5_session *session = sp5_session_create(authority);
    const uint8_t cert_data[] = {1, 2, 3};
    sp5_bytes cert = {cert_data, sizeof(cert_data)};
    CHECK(session != NULL && sp5_authority_pair(authority, cert).status == SP5_OK &&
          sp5_session_admit(session, cert, 91).status == SP5_OK);
    sp5_host host = {.supported_mode_count = 1};
    host.supported_modes[0] = (sp5_host_mode){1280, 720, 60, "h264", SP5_SDR, 100, 1000};
    CHECK(sp5_session_configure(session, &host, &(sp5_session_parameters){.requested_kbps = 500})
                  .status == SP5_OK &&
          sp5_session_activate(session).status == SP5_OK);
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_controller_input *input = sp5_controller_create_bound(session, cert, &fake.input);
    CHECK(input != NULL);
    sp5_control_config config = {.session = session,
                                 .verified_certificate = cert,
                                 .peer = "10.1.2.3",
                                 .release_input = release_input,
                                 .end_context = input,
                                 .launch = {.session_id = 91, .control_connect_data = 42},
                                 .negotiated = {.encryption_flags = 7}};
    sp5_control *control = sp5_control_create(&config);
    CHECK(control != NULL &&
          sp5_control_accept(control, config.peer, false, 42, 0).status == SP5_OK);
    uint8_t packets[ITERATIONS + 1][62] = {{0}}, wire_data[SP5_CONTROL_WIRE_LIMIT];
    CHECK(prepare(packets) == 0);
    sp5_buffer wire = {wire_data, sizeof(wire_data), 0};
    const uint8_t rumble[10] = {0};
    begin();
    for (unsigned i = 0; i < ITERATIONS; ++i) {
        CHECK(sp5_control_receive(control, config.peer, false, 16, SP5_CONTROL_RELIABLE,
                                  (sp5_bytes){packets[i], sizeof(packets[i])}, i + 1)
                  .status == SP5_OK);
        sp5_control_event event;
        CHECK(sp5_control_take(control, &event).status == SP5_OK);
        sp5_input_evidence evidence = {.session_id = 91, .host_received = fake.now};
        CHECK(sp5_controller_receive_control(input, &event, false, &evidence).status == SP5_OK);
        CHECK(fake.calls[SP5_FAKE_INPUT_DELIVER] == i + 1);
        CHECK(sp5_control_send(control, 0x010b, (sp5_bytes){rumble, sizeof(rumble)}).status ==
              SP5_OK);
        wire.size = 0;
        wire.capacity = 1;
        CHECK(sp5_control_take_wire(control, &wire).reason == SP5_REASON_CAPACITY);
        wire.capacity = sizeof(wire_data);
        CHECK(sp5_control_take_wire(control, &wire).status == SP5_OK && wire.size == 38);
    }
    CHECK(end("authenticated_control_and_fake_input") == 0);
    CHECK(printf("control received=%u fake_deliveries=%llu encrypted_feedback=%u\n", ITERATIONS,
                 (unsigned long long)fake.calls[SP5_FAKE_INPUT_DELIVER], ITERATIONS) > 0);
    packets[ITERATIONS][8] ^= 1;
    begin();
    CHECK(sp5_control_receive(control, config.peer, false, 16, SP5_CONTROL_RELIABLE,
                              (sp5_bytes){packets[ITERATIONS], sizeof(packets[ITERATIONS])}, 1000)
              .status == SP5_ERROR);
    CHECK(sp5_control_diagnostic(control) == SP5_REASON_OWNERSHIP && !fake.controllers[0].live);
    CHECK(end("invalid_tag_input_release") == 0);
    sp5_control_destroy(control);
    sp5_controller_destroy(input);
    sp5_session_destroy(session);
    sp5_authority_destroy(authority);
    sp5_devices_destroy(devices);
    return 0;
}
static int enet_packets(void)
{
    sp5_control_runtime *runtime = sp5_control_runtime_create();
    CHECK(runtime != NULL);
    uint8_t source[4096] = {0x42};
    begin();
    for (unsigned i = 0; i < ITERATIONS; ++i) {
        ENetPacket *small = enet_packet_create(source, 1024, ENET_PACKET_FLAG_RELIABLE);
        ENetPacket *large = enet_packet_create(source, sizeof(source), ENET_PACKET_FLAG_RELIABLE);
        CHECK(small != NULL && large != NULL && memcmp(small->data, source, 1024) == 0 &&
              memcmp(large->data, source, sizeof(source)) == 0);
        CHECK(sp5_control_pool_read(runtime).used_blocks == 4);
        enet_packet_destroy(small);
        enet_packet_destroy(large);
        CHECK(sp5_control_pool_read(runtime).used_blocks == 0);
    }
    ENetPacket *packets[260];
    for (size_t i = 0; i < 260; ++i) {
        packets[i] = enet_packet_create(source, 1024, ENET_PACKET_FLAG_RELIABLE);
        CHECK(packets[i] != NULL);
    }
    CHECK(enet_packet_create(source, 1024, ENET_PACKET_FLAG_RELIABLE) == NULL);
    CHECK(sp5_control_pool_read(runtime).used_blocks == 520 &&
          sp5_control_pool_read(runtime).exhausted == 1);
    for (size_t i = 0; i < 260; ++i)
        enet_packet_destroy(packets[i]);
    CHECK(sp5_control_pool_read(runtime).used_blocks == 0);
    ENetPacket *recovered = enet_packet_create(source, sizeof(source), ENET_PACKET_FLAG_RELIABLE);
    CHECK(recovered != NULL && sp5_control_pool_read(runtime).used_blocks == 2);
    enet_packet_destroy(recovered);
    CHECK(end("enet_packet_pool_lifecycle") == 0);
    sp5_control_pool_view view = sp5_control_pool_read(runtime);
    CHECK(view.used_blocks == 0 && view.high_water == 520 && view.exhausted == 1);
    CHECK(printf("enet packet_cycles=%u saturation_packets=260 recovered=1 used=%zu peak=%zu "
                 "exhausted=%llu\n",
                 ITERATIONS, view.used_blocks, view.high_water,
                 (unsigned long long)view.exhausted) > 0);
    CHECK(sp5_control_runtime_destroy(runtime).status == SP5_OK);
    return 0;
}
/* Production admission excludes loopback addresses. Both sockets bind the same
 * enumerated private local address, so these datagrams use kernel-local routing.
 * A runner without a private IPv4 interface fails explicitly, never skips. */
static int local_address(ENetAddress *address, char text[INET_ADDRSTRLEN])
{
    struct ifaddrs *interfaces = NULL;
    CHECK(getifaddrs(&interfaces) == 0);
    bool found = false;
    for (const struct ifaddrs *item = interfaces; item != NULL; item = item->ifa_next) {
        if (item->ifa_addr == NULL || item->ifa_addr->sa_family != AF_INET)
            continue;
        struct sockaddr_in local = *(const struct sockaddr_in *)item->ifa_addr;
        if (inet_ntop(AF_INET, &local.sin_addr, text, INET_ADDRSTRLEN) == NULL ||
            !sp5_peer_permitted(text, true))
            continue;
        local.sin_port = 0;
        found = enet_address_set_address(address, (struct sockaddr *)&local, sizeof(local)) == 0;
        CHECK(printf("transport local_interface=%s local_address=%s\n", item->ifa_name, text) > 0);
        break;
    }
    freeifaddrs(interfaces);
    CHECK(found);
    return 0;
}
static uint64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return UINT64_MAX;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}
static bool before(uint64_t deadline)
{
    return monotonic_ms() < deadline;
}
static unsigned intercepted;
static EVP_CIPHER_CTX *feedback_cipher;
static int drop_ack(ENetHost *host, ENetEvent *event)
{
    (void)event;
    if (intercepted != 0 || host->receivedDataLength < 2)
        return 0;
    uint16_t peer_id;
    memcpy(&peer_id, host->receivedData, sizeof(peer_id));
    size_t header = (ntohs(peer_id) & ENET_PROTOCOL_HEADER_FLAG_SENT_TIME) != 0 ? 4U : 2U;
    if (host->receivedDataLength < header + sizeof(ENetProtocolAcknowledge) ||
        (host->receivedData[header] & ENET_PROTOCOL_COMMAND_MASK) !=
            ENET_PROTOCOL_COMMAND_ACKNOWLEDGE ||
        host->receivedData[header + 1] != 16)
        return 0;
    ++intercepted;
    return 1;
}
static int service_client(ENetHost *client, unsigned *feedback)
{
    ENetEvent event;
    int result = enet_host_service(client, &event, 1);
    CHECK(result >= 0);
    if (result > 0) {
        CHECK(event.type == ENET_EVENT_TYPE_RECEIVE && event.channelID == 0 &&
              (event.packet->flags & ENET_PACKET_FLAG_RELIABLE) != 0);
        CHECK(event.packet->dataLength == 38 && event.packet->data[0] == 1);
        for (unsigned byte = 0; byte < 4; ++byte)
            CHECK(event.packet->data[4 + byte] == (uint8_t)(*feedback >> (8 * byte)));
        uint8_t iv[12] = {0}, plain[14];
        memcpy(iv, event.packet->data + 4, 4);
        iv[10] = 'H';
        iv[11] = 'C';
        int size = 0, tail = 0;
        CHECK(EVP_DecryptInit_ex(feedback_cipher, NULL, NULL, NULL, iv) == 1 &&
              EVP_DecryptUpdate(feedback_cipher, plain, &size, event.packet->data + 24, 14) == 1 &&
              EVP_CIPHER_CTX_ctrl(feedback_cipher, EVP_CTRL_GCM_SET_TAG, 16,
                                  event.packet->data + 8) == 1 &&
              EVP_DecryptFinal_ex(feedback_cipher, plain + size, &tail) == 1);
        const uint8_t expected[14] = {0x0b, 1, 10};
        CHECK(size + tail == 14 && memcmp(plain, expected, sizeof(expected)) == 0);
        ++*feedback;
        enet_packet_destroy(event.packet);
    }
    return 0;
}
static int settle(sp5_control_transport *server, ENetHost *client, ENetPeer *peer,
                  sp5_control_runtime *runtime, size_t baseline, unsigned *feedback)
{
    uint64_t deadline = monotonic_ms() + 5000;
    do {
        CHECK(sp5_control_transport_step(server, 1000).status == SP5_OK);
        CHECK(service_client(client, feedback) == 0);
        CHECK(before(deadline));
    } while (!enet_list_empty(&peer->sentReliableCommands) ||
             !enet_list_empty(&peer->outgoingSendReliableCommands) ||
             sp5_control_pool_read(runtime).used_blocks != baseline);
    return 0;
}
static int transport(bool inject, bool exhaust)
{
    sp5_storage_ops ops = {NULL, read_file, write_file, NULL};
    sp5_devices *devices = sp5_devices_create(&ops);
    CHECK(devices != NULL && sp5_devices_load(devices).status == SP5_OK);
    sp5_authority *authority = sp5_authority_create(devices);
    sp5_session *session = sp5_session_create(authority);
    const uint8_t cert_data[] = {1, 2, 3};
    sp5_bytes cert = {cert_data, sizeof(cert_data)};
    CHECK(session != NULL && sp5_authority_pair(authority, cert).status == SP5_OK &&
          sp5_session_admit(session, cert, 92).status == SP5_OK);
    sp5_host host = {.supported_mode_count = 1};
    host.supported_modes[0] = (sp5_host_mode){1280, 720, 60, "h264", SP5_SDR, 100, 1000};
    CHECK(sp5_session_configure(session, &host, &(sp5_session_parameters){.requested_kbps = 500})
                  .status == SP5_OK &&
          sp5_session_activate(session).status == SP5_OK);
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_controller_input *input = sp5_controller_create_bound(session, cert, &fake.input);
    CHECK(input != NULL);
    ENetAddress bind = {0};
    char address_text[INET_ADDRSTRLEN];
    CHECK(local_address(&bind, address_text) == 0);
    sp5_control_config config = {.session = session,
                                 .verified_certificate = cert,
                                 .peer = address_text,
                                 .on_link = true,
                                 .release_input = release_input,
                                 .end_context = input,
                                 .launch = {.session_id = 92, .control_connect_data = 42},
                                 .negotiated = {.encryption_flags = 7, .feature_flags = 2}};
    sp5_control *control = sp5_control_create(&config);
    sp5_control_runtime *runtime = sp5_control_runtime_create();
    CHECK(control != NULL && runtime != NULL);
    sp5_control_transport *server = sp5_control_transport_open(runtime, control, address_text, 0);
    ENetHost *client = enet_host_create(AF_INET, &bind, 1, SP5_CONTROL_CHANNEL_COUNT, 0, 0);
    CHECK(server != NULL && client != NULL);
    ((struct sockaddr_in *)&bind.address)->sin_port = htons(sp5_control_transport_port(server));
    ENetPeer *peer = enet_host_connect(client, &bind, SP5_CONTROL_CHANNEL_COUNT, 42);
    CHECK(peer != NULL);
    uint64_t deadline = monotonic_ms() + 5000;
    bool connected = false;
    while (!connected) {
        CHECK(sp5_control_transport_step(server, 1000).status == SP5_OK);
        ENetEvent event;
        int result = enet_host_service(client, &event, 1);
        CHECK(result >= 0 && before(deadline));
        if (result > 0) {
            CHECK(event.type == ENET_EVENT_TYPE_CONNECT);
            connected = true;
        }
    }
    enet_host_flush(client);
    CHECK(sp5_control_transport_step(server, 1000).status == SP5_OK);
    unsigned feedback = 0;
    /* Drain handshake ACKs before recording persistent host/peer pool ownership. */
    deadline = monotonic_ms() + 5000;
    while (!enet_list_empty(&peer->sentReliableCommands) ||
           !enet_list_empty(&peer->outgoingSendReliableCommands)) {
        CHECK(sp5_control_transport_step(server, 1000).status == SP5_OK);
        CHECK(service_client(client, &feedback) == 0 && before(deadline));
    }
    size_t baseline = sp5_control_pool_read(runtime).used_blocks;
    CHECK(baseline > 0);
    uint8_t packets[ITERATIONS + 1][62] = {{0}};
    CHECK(prepare(packets) == 0);
    uint8_t fragment[SP5_CONTROL_WIRE_LIMIT] = {1, 0, 0x18, 4}, key[16] = {0};
    uint8_t plain[SP5_CONTROL_PAYLOAD_LIMIT + 4] = {6, 2, 0, 4};
    uint8_t iv[12] = {1, 1};
    memcpy(fragment + 4, iv, 4);
    iv[10] = iv[11] = 'C';
    EVP_CIPHER_CTX *encoder = EVP_CIPHER_CTX_new();
    CHECK(encoder != NULL);
    int size = 0, tail = 0;
    CHECK(EVP_EncryptInit_ex(encoder, EVP_aes_128_gcm(), NULL, key, iv) == 1 &&
          EVP_EncryptUpdate(encoder, fragment + 24, &size, plain, sizeof(plain)) == 1 &&
          EVP_EncryptFinal_ex(encoder, fragment + 24 + size, &tail) == 1 &&
          size + tail == (int)sizeof(plain) &&
          EVP_CIPHER_CTX_ctrl(encoder, EVP_CTRL_GCM_GET_TAG, 16, fragment + 8) == 1);
    EVP_CIPHER_CTX_free(encoder);
    feedback_cipher = EVP_CIPHER_CTX_new();
    CHECK(feedback_cipher != NULL &&
          EVP_DecryptInit_ex(feedback_cipher, EVP_aes_128_gcm(), NULL, key, NULL) == 1);
    const uint8_t rumble[10] = {0};
    begin();
    for (unsigned i = 0; i < ITERATIONS; ++i) {
        ENetPacket *packet =
            enet_packet_create(packets[i], sizeof(packets[i]), ENET_PACKET_FLAG_RELIABLE);
        CHECK(packet != NULL && enet_peer_send(peer, 16, packet) == 0);
        enet_host_flush(client);
        sp5_control_event event;
        deadline = monotonic_ms() + 5000;
        while (sp5_control_take(control, &event).status != SP5_OK) {
            CHECK(sp5_control_transport_step(server, 1000).status == SP5_OK);
            CHECK(service_client(client, &feedback) == 0 && before(deadline));
        }
        CHECK(event.sequence == i && event.type == 0x0206 && event.payload_size == 34);
        sp5_input_evidence evidence = {.session_id = 92, .host_received = fake.now};
        CHECK(sp5_controller_receive_control(input, &event, false, &evidence).status == SP5_OK);
        CHECK(sp5_control_send(control, 0x010b, (sp5_bytes){rumble, sizeof(rumble)}).status ==
              SP5_OK);
        CHECK(settle(server, client, peer, runtime, baseline, &feedback) == 0);
        CHECK(feedback == i + 1 && fake.calls[SP5_FAKE_INPUT_DELIVER] == i + 1);
    }
    /* Drop the genuine ACK, then advance ENet's public test clock to the actual
     * queued command's deadline. The real service path retransmits and deduplicates. */
    client->intercept = drop_ack;
    intercepted = 0;
    /* Keep the first retransmit outside the 1-ms observation poll; the test
     * advances to this deadline explicitly after retaining the dropped ACK. */
    peer->roundTripTime = 1000;
    peer->roundTripTimeVariance = 0;
    ENetPacket *packet = enet_packet_create(packets[ITERATIONS], 62, ENET_PACKET_FLAG_RELIABLE);
    CHECK(packet != NULL && enet_peer_send(peer, 16, packet) == 0);
    enet_host_flush(client);
    sp5_control_event received = {0};
    bool taken = false;
    deadline = monotonic_ms() + 5000;
    while (!taken || intercepted == 0) {
        CHECK(sp5_control_transport_step(server, 1000).status == SP5_OK);
        CHECK(service_client(client, &feedback) == 0 && before(deadline));
        if (!taken)
            taken = sp5_control_take(control, &received).status == SP5_OK;
    }
    CHECK(received.sequence == ITERATIONS && intercepted > 0 &&
          !enet_list_empty(&peer->sentReliableCommands));
    ENetOutgoingCommand *pending =
        (ENetOutgoingCommand *)enet_list_front(&peer->sentReliableCommands);
    enet_uint16 attempts = pending->sendAttempts;
    enet_time_set(pending->sentTime + pending->roundTripTimeout + 1);
    CHECK(service_client(client, &feedback) == 0 && pending->sendAttempts > attempts);
    client->intercept = NULL;
    CHECK(settle(server, client, peer, runtime, baseline, &feedback) == 0);
    CHECK(sp5_control_take(control, &received).reason == SP5_REASON_WOULD_BLOCK);
    /* A valid maximum-sized control event exceeds ENet's minimum 576-byte MTU.
     * Inspect the queued fragment descriptors, then verify exact reassembly. */
    peer->mtu = ENET_PROTOCOL_MINIMUM_MTU;
    packet = enet_packet_create(fragment, sizeof(fragment), ENET_PACKET_FLAG_RELIABLE);
    CHECK(packet != NULL && enet_peer_send(peer, 16, packet) == 0);
    unsigned fragments = 0;
    for (ENetListIterator item = enet_list_begin(&peer->outgoingSendReliableCommands);
         item != enet_list_end(&peer->outgoingSendReliableCommands); item = enet_list_next(item)) {
        const ENetOutgoingCommand *command = (const ENetOutgoingCommand *)item;
        CHECK((command->command.header.command & ENET_PROTOCOL_COMMAND_MASK) ==
              ENET_PROTOCOL_COMMAND_SEND_FRAGMENT);
        ++fragments;
    }
    CHECK(fragments >= 2);
    enet_host_flush(client);
    deadline = monotonic_ms() + 5000;
    while (sp5_control_take(control, &received).status != SP5_OK) {
        CHECK(sp5_control_transport_step(server, 1000).status == SP5_OK);
        CHECK(service_client(client, &feedback) == 0 && before(deadline));
    }
    CHECK(received.sequence == ITERATIONS + 1 &&
          received.payload_size == SP5_CONTROL_PAYLOAD_LIMIT &&
          memcmp(received.payload, plain + 4, SP5_CONTROL_PAYLOAD_LIMIT) == 0);
    CHECK(settle(server, client, peer, runtime, baseline, &feedback) == 0);
    uint8_t malformed[4] = {0};
    packet = enet_packet_create(malformed, sizeof(malformed), ENET_PACKET_FLAG_RELIABLE);
    CHECK(packet != NULL && enet_peer_send(peer, 16, packet) == 0);
    enet_host_flush(client);
    CHECK(settle(server, client, peer, runtime, baseline, &feedback) == 0);
    CHECK(sp5_control_take(control, &received).reason == SP5_REASON_WOULD_BLOCK &&
          sp5_session_read(session).state == SP5_SESSION_ACTIVE);
    if (inject) {
        void *allocation = OPENSSL_malloc(8);
        CHECK(allocation != NULL);
        OPENSSL_free(allocation);
    }
    CHECK(end("enet_service_authenticated_exchange") == 0);
    CHECK(printf("transport cycles=%u feedback=%u baseline=%zu peak=%zu ack_drops=%u "
                 "retransmission=1 duplicate_delivery=0 fragments=%u malformed_refused=1\n",
                 ITERATIONS, feedback, baseline, sp5_control_pool_read(runtime).high_water,
                 intercepted, fragments) > 0);
    EVP_CIPHER_CTX_free(feedback_cipher);
    feedback_cipher = NULL;
    if (exhaust) {
        ENetPacket *held[260] = {0};
        size_t count = 0;
        begin();
        while (count < 260) {
            held[count] = enet_packet_create(rumble, sizeof(rumble), ENET_PACKET_FLAG_RELIABLE);
            if (held[count] == NULL)
                break;
            ++count;
        }
        CHECK(count < 260 && sp5_control_pool_read(runtime).exhausted == 1);
        CHECK(sp5_control_send(control, 0x010b, (sp5_bytes){rumble, sizeof(rumble)}).status ==
              SP5_OK);
        CHECK(sp5_control_transport_step(server, 1000).reason == SP5_REASON_CAPACITY &&
              sp5_control_pool_read(runtime).exhausted == 2 && !fake.controllers[0].live);
        for (size_t i = 0; i < count; ++i)
            enet_packet_destroy(held[i]);
        CHECK(end("enet_service_pool_exhaustion") == 0);
        CHECK(sp5_session_read(session).end_reason == SP5_END_ERROR);
        CHECK(
            printf("transport saturated_packets=%zu service_capacity_refusal=1 input_released=1\n",
                   count) > 0);
    } else {
        enet_peer_disconnect(peer, 0);
        enet_host_flush(client);
        deadline = monotonic_ms() + 5000;
        while (sp5_session_read(session).state == SP5_SESSION_ACTIVE) {
            CHECK(sp5_control_transport_step(server, 1000).status == SP5_OK && before(deadline));
        }
        CHECK(!fake.controllers[0].live &&
              sp5_session_read(session).end_reason == SP5_END_CLIENT_QUIT);
    }
    sp5_control_transport_close(server);
    enet_host_destroy(client);
    CHECK(sp5_control_pool_read(runtime).used_blocks == 0 &&
          sp5_control_pool_read(runtime).exhausted == (exhaust ? 2U : 0U));
    packet = enet_packet_create(rumble, sizeof(rumble), ENET_PACKET_FLAG_RELIABLE);
    CHECK(packet != NULL);
    enet_packet_destroy(packet);
    CHECK(sp5_control_pool_read(runtime).used_blocks == 0);
    CHECK(sp5_control_runtime_destroy(runtime).status == SP5_OK);
    sp5_control_destroy(control);
    sp5_controller_destroy(input);
    sp5_session_destroy(session);
    sp5_authority_destroy(authority);
    sp5_devices_destroy(devices);
    CHECK(printf("transport terminal=%s input_released=1 final_pool_used=0 recovered=1\n",
                 exhaust ? "capacity" : "graceful_disconnect") > 0);
    return 0;
}
int main(int argc, char **argv)
{
    CHECK(argc == 1 || (argc == 2 && (strcmp(argv[1], "--inject-allocation") == 0 ||
                                      strcmp(argv[1], "--inject-transport-allocation") == 0)));
    CHECK(CRYPTO_set_mem_functions(allocate, reallocate, release) == 1);
    CHECK(calibration() == 0);
    CHECK(media(argc == 2 && strcmp(argv[1], "--inject-allocation") == 0) == 0);
    CHECK(control_input() == 0);
    CHECK(enet_packets() == 0);
    CHECK(transport(argc == 2 && strcmp(argv[1], "--inject-transport-allocation") == 0, false) ==
          0);
    CHECK(transport(false, true) == 0);
    CHECK(puts("T114 dependency allocation scenarios passed; hardware evidence absent") >= 0);
    return 0;
}
