/* Synthetic portable contract tests; no console or interoperability evidence. */
#if defined(SP5_PENDING_CORE) && defined(__APPLE__)
#define SP5_CORE_API __attribute__((weak_import))
#elif defined(SP5_PENDING_CORE)
#define SP5_CORE_API __attribute__((weak))
#endif

#include "protocol.h"
#include "session/session.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            if (fputs(__FILE__ ": " #condition "\n", stderr) == EOF) {                             \
                return 2;                                                                          \
            }                                                                                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
#ifdef SP5_PENDING_CORE
#define NEED(api)                                                                                  \
    do {                                                                                           \
        if ((api) == NULL) {                                                                       \
            if (fputs("TDD missing implementation: " #api "\n", stderr) == EOF) {                  \
                return 2;                                                                          \
            }                                                                                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
#else
#define NEED(api) ((void)0)
#endif

static sp5_bytes bytes(const char *text)
{
    return (sp5_bytes){(const uint8_t *)text, strlen(text)};
}

/* T052: FR-040 boundaries use transport peers, independent of local subnet. */
static int addresses(void)
{
    NEED(sp5_peer_permitted);
    const char *allowed[] = {"10.0.0.0",        "10.255.255.255", "172.16.0.0",
                             "172.31.255.255",  "192.168.0.0",    "192.168.255.255",
                             "10.200.3.9",      "fc00::",         "fdff:ffff::1",
                             "::ffff:10.0.0.1", "::ffff:ac10:1",  "::ffff:192.168.255.255"};
    const char *denied[] = {"9.255.255.255",
                            "11.0.0.0",
                            "172.15.255.255",
                            "172.32.0.0",
                            "192.167.255.255",
                            "192.169.0.0",
                            "0.0.0.0",
                            "127.0.0.1",
                            "169.254.1.1",
                            "100.64.0.1",
                            "224.0.0.1",
                            "255.255.255.255",
                            "8.8.8.8",
                            "::",
                            "::1",
                            "fbff:ffff::1",
                            "fe00::1",
                            "2001:db8::1",
                            "ff02::1",
                            "::ffff:8.8.8.8",
                            "::ffff:172.32.0.1",
                            "::192.168.0.1",
                            "10.0.0.999",
                            "10.0.0.1.evil",
                            ""};
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
        CHECK(sp5_peer_permitted(allowed[i], false));
    }
    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); ++i) {
        CHECK(!sp5_peer_permitted(denied[i], true));
    }
    CHECK(sp5_peer_permitted("fe80::1", true));
    CHECK(sp5_peer_permitted("febf:ffff::1", true));
    CHECK(!sp5_peer_permitted("fe80::1", false));
    CHECK(!sp5_peer_permitted("fec0::1", true));
    CHECK(!sp5_peer_permitted(NULL, true));
    return 0;
}

/* T055: serverinfo/launch spellings from protocol-notes.md 1/2. A forwarded
 * private address cannot convert a public transport peer into an eligible one. */
static int http_parser(void)
{
    NEED(sp5_http_parse);
    NEED(sp5_peer_permitted);
    const char valid[] =
        "GET /serverinfo?uniqueid=untrusted-name HTTP/1.1\r\nHost: 10.0.0.2\r\n\r\n";
    sp5_request request;
    CHECK(sp5_http_parse(bytes(valid), sizeof(valid), &request).status == SP5_OK);
    CHECK(request.method.size == 3 && memcmp(request.method.data, "GET", 3) == 0);
    CHECK(request.target.size == strlen("/serverinfo?uniqueid=untrusted-name"));
    CHECK(request.body.size == 0);
    const char *invalid[] = {
        "GET / HTTP/1.1\r\nHost: x\r\n",
        "GET / HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
        "POST /pair HTTP/1.1\r\nContent-Length: 2\r\n\r\nx",
        "POST /pair HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nx",
        "GET / HTTP/1.1\r\nContent-Length: 18446744073709551616\r\n\r\n",
        "GET / HTTP/1.1\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\nx"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        CHECK(sp5_http_parse(bytes(invalid[i]), 4096, &request).status == SP5_ERROR);
    }
    CHECK(sp5_http_parse(bytes(valid), 8, &request).status == SP5_ERROR);
    const uint8_t nul[] = {'G', 'E', 'T', ' ', '/', '\0', 'x',  ' ',  'H',  'T',
                           'T', 'P', '/', '1', '.', '1',  '\r', '\n', '\r', '\n'};
    CHECK(sp5_http_parse((sp5_bytes){nul, sizeof(nul)}, 4096, &request).status == SP5_ERROR);
    CHECK(sp5_http_parse(bytes("GET /serverinfo HTTP/1.1\r\nX-Forwarded-For: 10.1.2.3\r\n\r\n"),
                         4096, &request)
              .status == SP5_OK);
    CHECK(!sp5_peer_permitted("203.0.113.2", false));
    return 0;
}

/* T058: CSeq and X-GS-ClientVersion from protocol-notes.md section 3. */
static int rtsp_parser(void)
{
    NEED(sp5_rtsp_parse);
    sp5_request request;
    const char valid[] =
        "OPTIONS rtsp://10.0.0.2:48010 RTSP/1.0\r\nCSeq: 1\r\nX-GS-ClientVersion: 14\r\n\r\n";
    CHECK(sp5_rtsp_parse(bytes(valid), 4096, &request).status == SP5_OK);
    CHECK(request.cseq == 1 && request.method.size == 7);
    const char *invalid[] = {"OPTIONS x RTSP/1.0\r\nCSeq: -1\r\n\r\n",
                             "OPTIONS x RTSP/1.0\r\nCSeq: 4294967296\r\n\r\n",
                             "OPTIONS x RTSP/1.0\r\nCSeq: 1\r\nCSeq: 2\r\n\r\n",
                             "ANNOUNCE x RTSP/1.0\r\nCSeq: 1\r\nContent-Length: 3\r\n\r\nx"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        CHECK(sp5_rtsp_parse(bytes(invalid[i]), 4096, &request).status == SP5_ERROR);
    }
    CHECK(sp5_rtsp_parse(bytes(valid), 1, &request).status == SP5_ERROR);
    return 0;
}

/* T058: configuredBitrateKbps from notes section 4; the 100/1000 interval
 * belongs solely to this synthetic fixture, not to a PS5 supported mode. */
static int bitrate(void)
{
    NEED(sp5_negotiate_bitrate);
    const uint32_t requested[] = {1, 99, 100, 101, 999, 1000, 1001, UINT32_MAX};
    const uint32_t chosen[] = {100, 100, 100, 101, 999, 1000, 1000, 1000};
    const sp5_bitrate_adjustment adjustment[] = {
        SP5_BITRATE_RAISED, SP5_BITRATE_RAISED, SP5_BITRATE_NONE,    SP5_BITRATE_NONE,
        SP5_BITRATE_NONE,   SP5_BITRATE_NONE,   SP5_BITRATE_LOWERED, SP5_BITRATE_LOWERED};
    sp5_bitrate result;
    for (size_t i = 0; i < sizeof(requested) / sizeof(requested[0]); ++i) {
        char sdp[96];
        int length = snprintf(sdp, sizeof(sdp), "v=0\r\na=x-ml-video.configuredBitrateKbps:%u\r\n",
                              requested[i]);
        CHECK(length > 0 && (size_t)length < sizeof(sdp));
        CHECK(sp5_negotiate_bitrate(bytes(sdp), 100, 1000, &result).status == SP5_OK);
        CHECK(result.requested_kbps == requested[i] && result.chosen_kbps == chosen[i]);
        CHECK(result.adjustment == adjustment[i]);
    }
    const char *bad[] = {"",   "0",          "-1",
                         "1x", "4294967296", "1\r\na=x-ml-video.configuredBitrateKbps:2"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        char sdp[160];
        int length =
            snprintf(sdp, sizeof(sdp), "v=0\r\na=x-ml-video.configuredBitrateKbps:%s\r\n", bad[i]);
        CHECK(length > 0 && (size_t)length < sizeof(sdp));
        CHECK(sp5_negotiate_bitrate(bytes(sdp), 100, 1000, &result).status == SP5_ERROR);
    }
    CHECK(sp5_negotiate_bitrate(bytes("v=0\r\n"), 100, 1000, &result).status == SP5_ERROR);
    CHECK(sp5_negotiate_bitrate(bytes("a=x-ml-video.configuredBitrateKbps:100\r\n"), 1000, 100,
                                &result)
              .status == SP5_ERROR);
    return 0;
}

/* Bounded synthetic persistence for the shared real registry implementation. */
typedef struct {
    uint8_t data[40000];
    size_t size;
} authority_files;
static sp5_result authority_read(void *context, sp5_store_file file, sp5_buffer *output)
{
    authority_files *files = context;
    if (file != SP5_STORE_DEVICES || files->size == 0) {
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    }
    return sp5_buffer_append(output, (sp5_bytes){files->data, files->size});
}
static sp5_result authority_write(void *context, sp5_store_file file, sp5_bytes data)
{
    authority_files *files = context;
    if (file != SP5_STORE_DEVICES || data.size > sizeof(files->data)) {
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    memcpy(files->data, data.data, data.size);
    files->size = data.size;
    return sp5_success();
}

/* T051/T053/T059: explicit interleavings exercise revoke between admission and
 * activation, revoke between packets, and certificate identity vs display IDs. */
static int authorization(void)
{
    NEED(sp5_authority_create);
    NEED(sp5_authority_destroy);
    NEED(sp5_authority_pair);
    NEED(sp5_authority_revoke);
    NEED(sp5_session_create);
    NEED(sp5_session_destroy);
    NEED(sp5_session_admit);
    NEED(sp5_session_activate);
    NEED(sp5_session_authorized);
    NEED(sp5_session_end);
    NEED(sp5_session_read);
    authority_files files = {0};
    sp5_storage_ops ops = {&files, authority_read, authority_write, NULL};
    sp5_devices *registry = sp5_devices_create(&ops);
    CHECK(registry != NULL && sp5_devices_load(registry).status == SP5_OK);
    sp5_authority *authority = sp5_authority_create(registry);
    CHECK(authority != NULL);
    sp5_session *session = sp5_session_create(authority);
    CHECK(session != NULL);
    sp5_bytes certificate = bytes("synthetic-certificate-A");
    CHECK(sp5_session_admit(session, certificate, 1).status == SP5_ERROR);
    CHECK(sp5_authority_pair(authority, certificate).status == SP5_OK);
    CHECK(sp5_session_admit(session, certificate, 1).status == SP5_OK);
    CHECK(!sp5_session_authorized(session, bytes("claimed-device-A")));
    CHECK(sp5_authority_revoke(authority, certificate).status == SP5_OK);
    CHECK(sp5_session_activate(session).status == SP5_ERROR);
    CHECK(!sp5_session_authorized(session, certificate));
    CHECK(sp5_session_end(session, SP5_END_REVOKED).status == SP5_OK);
    CHECK(sp5_authority_pair(authority, certificate).status == SP5_OK);
    CHECK(sp5_session_admit(session, certificate, 2).status == SP5_OK);
    sp5_host host = {0};
    host.supported_mode_count = 1;
    host.supported_modes[0] = (sp5_host_mode){1280, 720, 60, "fixture", SP5_SDR, 100, 1000};
    CHECK(sp5_session_configure(
              session, &host,
              &(sp5_session_parameters){
                  .mode_index = 0, .requested_kbps = 100, .started_us = 1, .wall_unix_us = {0}})
              .status == SP5_OK);
    CHECK(sp5_session_activate(session).status == SP5_OK);
    CHECK(sp5_session_authorized(session, certificate));
    sp5_session *competitor = sp5_session_create(authority);
    CHECK(competitor != NULL);
    CHECK(sp5_authority_pair(authority, bytes("synthetic-certificate-B")).status == SP5_OK);
    CHECK(sp5_session_admit(competitor, bytes("synthetic-certificate-B"), 3).status == SP5_ERROR);
    CHECK(sp5_session_read(session).id == 2 &&
          sp5_session_read(session).state == SP5_SESSION_ACTIVE);
    sp5_session_destroy(competitor);
    CHECK(sp5_session_admit(session, certificate, 3).status == SP5_ERROR);
    CHECK(sp5_session_read(session).id == 2 &&
          sp5_session_read(session).state == SP5_SESSION_ACTIVE);
    CHECK(sp5_authority_revoke(authority, certificate).status == SP5_OK);
    CHECK(!sp5_session_authorized(session, certificate));
    CHECK(sp5_authority_pair(authority, certificate).status == SP5_OK);
    CHECK(!sp5_session_authorized(session, certificate)); /* no resurrection */
    CHECK(sp5_session_end(session, SP5_END_REVOKED).status == SP5_OK);
    CHECK(sp5_session_end(session, SP5_END_CLIENT_QUIT).status == SP5_OK);
    CHECK(sp5_session_read(session).end_reason == SP5_END_REVOKED);
    sp5_session_destroy(session);
    sp5_authority_destroy(authority);
    sp5_devices_destroy(registry);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        return 2;
    }
    if (strcmp(argv[1], "addresses") == 0) {
        return addresses();
    }
    if (strcmp(argv[1], "http") == 0) {
        return http_parser();
    }
    if (strcmp(argv[1], "rtsp") == 0) {
        return rtsp_parser();
    }
    if (strcmp(argv[1], "bitrate") == 0) {
        return bitrate();
    }
    if (strcmp(argv[1], "authorization") == 0) {
        return authorization();
    }
    return 2;
}
