/* Synthetic portable contract tests; no console or interoperability evidence. */
#if defined(SP5_PENDING_CORE) && defined(__APPLE__)
#define SP5_CORE_API __attribute__((weak_import))
#elif defined(SP5_PENDING_CORE)
#define SP5_CORE_API __attribute__((weak))
#endif

#include "../fakes/platform.h"
#include "lifecycle.h"
#include "store.h"

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

enum { FILE_CAPACITY = 4096 };
typedef struct {
    uint8_t data[SP5_STORE_FILE_COUNT][FILE_CAPACITY];
    size_t size[SP5_STORE_FILE_COUNT];
    bool exists[SP5_STORE_FILE_COUNT];
    bool fail_replace;
    unsigned generations;
    unsigned replacements;
} fake_files;

static sp5_result file_read(void *context, sp5_store_file file, sp5_buffer *output)
{
    fake_files *files = context;
    if (!files->exists[file]) {
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    }
    return sp5_buffer_append(output, (sp5_bytes){files->data[file], files->size[file]});
}

static sp5_result file_replace(void *context, sp5_store_file file, sp5_bytes contents)
{
    fake_files *files = context;
    files->replacements++;
    if (files->fail_replace) {
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    if (contents.size > FILE_CAPACITY) {
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    memcpy(files->data[file], contents.data, contents.size);
    files->size[file] = contents.size;
    files->exists[file] = true;
    return sp5_success();
}

static sp5_result identity_generate(void *context, sp5_buffer *output)
{
    fake_files *files = context;
    files->generations++;
    /* Opaque synthetic identity material, never a real key/certificate. */
    uint8_t identity[32];
    memset(identity, (int)files->generations, sizeof(identity));
    return sp5_buffer_append(output, (sp5_bytes){identity, sizeof(identity)});
}

/* Current-era Unix microseconds (2025-10-30), never time(NULL) seconds. */
#define FIXTURE_PAIRED_AT_US UINT64_C(1761800000000000)

static sp5_storage_ops storage(fake_files *files)
{
    return (sp5_storage_ops){files, file_read, file_replace, identity_generate};
}

static int durability(void)
{
    NEED(sp5_store_create);
    NEED(sp5_store_destroy);
    NEED(sp5_store_load);
    NEED(sp5_store_identity);
    NEED(sp5_store_settings);
    NEED(sp5_store_save_settings);
    NEED(sp5_store_pair);
    NEED(sp5_store_authorized);
    fake_files files = {0};
    sp5_storage_ops ops = storage(&files);
    sp5_store *store = sp5_store_create(&ops);
    CHECK(store != NULL && sp5_store_load(store).status == SP5_OK);
    CHECK(files.generations == 1);
    uint8_t identity[FILE_CAPACITY];
    sp5_bytes generated = sp5_store_identity(store);
    CHECK(generated.size > 0 && generated.size <= sizeof(identity));
    size_t identity_size = generated.size;
    memcpy(identity, generated.data, identity_size);
    sp5_settings defaults = sp5_store_settings(store);
    CHECK(strcmp(defaults.host_name, "SunshinePS5") == 0 && !defaults.metrics_enabled);
    sp5_settings changed = {.host_name = "Living room", .metrics_enabled = true};
    CHECK(sp5_store_save_settings(store, &changed).status == SP5_OK);
    CHECK(
        sp5_store_pair(store, bytes("synthetic-paired-certificate"), FIXTURE_PAIRED_AT_US).status ==
        SP5_OK);
    sp5_store_destroy(store);
    store = sp5_store_create(&ops);
    CHECK(store != NULL && sp5_store_load(store).status == SP5_OK);
    CHECK(files.generations == 1);
    CHECK(sp5_store_identity(store).size == identity_size);
    CHECK(memcmp(sp5_store_identity(store).data, identity, identity_size) == 0);
    CHECK(strcmp(sp5_store_settings(store).host_name, "Living room") == 0);
    CHECK(sp5_store_settings(store).metrics_enabled);
    CHECK(sp5_store_authorized(store, bytes("synthetic-paired-certificate")));
    CHECK(sp5_store_read(store).paired_count == 1);
    files.fail_replace = true;
    CHECK(sp5_store_pair(store, bytes("must-not-authorize-on-save-failure"), FIXTURE_PAIRED_AT_US)
              .status == SP5_ERROR);
    CHECK(!sp5_store_authorized(store, bytes("must-not-authorize-on-save-failure")));
    changed.metrics_enabled = false;
    CHECK(sp5_store_save_settings(store, &changed).status == SP5_ERROR);
    CHECK(sp5_store_settings(store).metrics_enabled);
    sp5_store_destroy(store);
    files.fail_replace = false;
    store = sp5_store_create(&ops);
    CHECK(store != NULL && sp5_store_load(store).status == SP5_OK);
    CHECK(sp5_store_settings(store).metrics_enabled);
    changed.host_name[0] = '\n';
    unsigned replaces = files.replacements;
    CHECK(sp5_store_save_settings(store, &changed).status == SP5_ERROR);
    CHECK(files.replacements == replaces);
    sp5_store_destroy(store);
    return 0;
}

/* In-memory file snapshots model truncated/corrupt persisted bytes. Atomic
 * adapter sync/power-loss guarantees require separate real storage tests. */
static int recovery(void)
{
    NEED(sp5_store_create);
    NEED(sp5_store_destroy);
    NEED(sp5_store_load);
    NEED(sp5_store_read);
    NEED(sp5_store_settings);
    NEED(sp5_store_identity);
    NEED(sp5_store_pair);
    NEED(sp5_store_authorized);
    for (int file = SP5_STORE_IDENTITY; file < SP5_STORE_FILE_COUNT; ++file) {
        for (unsigned partial = 0; partial < 2; ++partial) {
            fake_files files = {0};
            sp5_storage_ops ops = storage(&files);
            sp5_store *store = sp5_store_create(&ops);
            CHECK(store != NULL && sp5_store_load(store).status == SP5_OK);
            CHECK(
                sp5_store_pair(store, bytes("certificate-to-lose"), FIXTURE_PAIRED_AT_US).status ==
                SP5_OK);
            sp5_store_destroy(store);
            files.exists[file] = true;
            if (partial != 0) {
                files.size[file] /= 2;
            } else {
                memcpy(files.data[file], "corrupt", 7);
                files.size[file] = 7;
            }
            unsigned generations = files.generations;
            unsigned replacements = files.replacements;
            store = sp5_store_create(&ops);
            CHECK(store != NULL);
            sp5_result loaded = sp5_store_load(store);
            sp5_store_view view = sp5_store_read(store);
            CHECK(view.diagnostic != SP5_REASON_NONE);
            if (file == SP5_STORE_IDENTITY) {
                CHECK(loaded.status == SP5_ERROR && !view.identity_valid);
                CHECK(sp5_store_identity(store).size == 0);
                CHECK(!sp5_store_authorized(store, bytes("certificate-to-lose")));
                CHECK(files.generations == generations && files.replacements == replacements);
            } else if (file == SP5_STORE_SETTINGS) {
                CHECK(view.settings_recovered);
                CHECK(strcmp(sp5_store_settings(store).host_name, "SunshinePS5") == 0);
                CHECK(!sp5_store_settings(store).metrics_enabled);
            } else {
                CHECK(view.repairing_required && view.paired_count == 0);
                CHECK(!sp5_store_authorized(store, bytes("certificate-to-lose")));
            }
            sp5_store_destroy(store);
        }
    }
    return 0;
}

static sp5_lifecycle_ops adapters(sp5_fake_platform *fake)
{
    return (sp5_lifecycle_ops){fake->capture, fake->encoder, fake->audio, fake->input,
                               fake->system};
}

static sp5_media_config mode(void)
{
    return (sp5_media_config){2, 2, 60, 1, "synthetic/fake-only", SP5_SDR, 100};
}

static int clean(const sp5_fake_platform *fake)
{
    CHECK(!fake->capture_open && !fake->encoder_open && !fake->audio_open);
    CHECK(!fake->video_leased && !fake->audio_leased && !fake->prepared);
    for (size_t i = 0; i < SP5_MAX_CONTROLLERS; ++i) {
        CHECK(!fake->controllers[i].live);
        CHECK(fake->controllers[i].state.buttons == 0);
        CHECK(fake->controllers[i].state.triggers[0] == 0 &&
              fake->controllers[i].state.triggers[1] == 0);
        for (size_t axis = 0; axis < 4; ++axis) {
            CHECK(fake->controllers[i].state.axes[axis] == 0);
        }
    }
    return 0;
}

static int unwind(void)
{
    NEED(sp5_lifecycle_create);
    NEED(sp5_lifecycle_destroy);
    NEED(sp5_lifecycle_start);
    NEED(sp5_lifecycle_read);
    const sp5_fake_operation failures[] = {SP5_FAKE_SYSTEM_PREPARE, SP5_FAKE_CAPTURE_OPEN,
                                           SP5_FAKE_ENCODER_OPEN, SP5_FAKE_AUDIO_OPEN};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        sp5_fake_platform fake;
        sp5_fake_init(&fake);
        sp5_system_state foreground_services = fake.system_state;
        fake.fail_next[failures[i]] = SP5_REASON_PLATFORM;
        sp5_lifecycle_ops ops = adapters(&fake);
        sp5_lifecycle *lifecycle = sp5_lifecycle_create(81, &ops);
        sp5_media_config config = mode();
        CHECK(lifecycle != NULL);
        CHECK(sp5_lifecycle_start(lifecycle, &config, 0).status == SP5_ERROR);
        CHECK(clean(&fake) == 0);
        CHECK(foreground_services.remote_play == fake.system_state.remote_play);
        CHECK(foreground_services.share_play == fake.system_state.share_play);
        CHECK(foreground_services.stopping_or_rest == fake.system_state.stopping_or_rest);
        CHECK(foreground_services.validity == fake.system_state.validity);
        CHECK(foreground_services.reason == fake.system_state.reason);
        CHECK(sp5_lifecycle_read(lifecycle).has_end_reason);
        CHECK(sp5_lifecycle_read(lifecycle).end_reason == SP5_END_ERROR);
        sp5_lifecycle_destroy(lifecycle);
    }
    return 0;
}

static int teardown(void)
{
    NEED(sp5_lifecycle_create);
    NEED(sp5_lifecycle_destroy);
    NEED(sp5_lifecycle_start);
    NEED(sp5_lifecycle_end);
    NEED(sp5_lifecycle_read);
    const sp5_end_reason reasons[] = {SP5_END_CLIENT_QUIT,      SP5_END_CLIENT_LOST,
                                      SP5_END_REVOKED,          SP5_END_HOST_STOPPING,
                                      SP5_END_CONSOLE_CONFLICT, SP5_END_ERROR};
    for (size_t end = 0; end < sizeof(reasons) / sizeof(reasons[0]); ++end) {
        sp5_fake_platform fake;
        sp5_fake_init(&fake);
        sp5_lifecycle_ops ops = adapters(&fake);
        sp5_lifecycle *lifecycle = sp5_lifecycle_create(81, &ops);
        sp5_media_config config = mode();
        CHECK(lifecycle != NULL && sp5_lifecycle_start(lifecycle, &config, 0).status == SP5_OK);
        for (uint32_t id = 0; id < SP5_MAX_CONTROLLERS; ++id) {
            CHECK(fake.input.add(&fake, 81, id).status == SP5_OK);
            sp5_controller_state state = {
                .buttons = UINT32_MAX, .axes = {32767, -32768, 1, -1}, .triggers = {65535, 32768}};
            sp5_input_evidence event = {.session_id = 81, .controller_id = id};
            CHECK(fake.input.deliver(&fake, &state, false, &event).status == SP5_OK);
        }
        CHECK(sp5_lifecycle_end(lifecycle, reasons[end]).status == SP5_OK);
        CHECK(clean(&fake) == 0);
        CHECK(fake.last_input_release_sequence > 0 &&
              fake.last_input_release_sequence < fake.last_media_close_sequence);
        CHECK(fake.rollbacks == 1);
        CHECK(!fake.system_state.remote_play && !fake.system_state.share_play &&
              !fake.system_state.stopping_or_rest);
        CHECK(sp5_lifecycle_read(lifecycle).state == SP5_SESSION_ENDED);
        CHECK(sp5_lifecycle_read(lifecycle).end_reason == reasons[end]);
        uint64_t closes = fake.capture_closes + fake.encoder_closes + fake.audio_closes;
        CHECK(sp5_lifecycle_end(lifecycle, SP5_END_ERROR).status == SP5_OK);
        CHECK(sp5_lifecycle_read(lifecycle).end_reason == reasons[end]);
        CHECK(fake.capture_closes + fake.encoder_closes + fake.audio_closes == closes &&
              fake.rollbacks == 1);
        sp5_lifecycle_destroy(lifecycle);
    }
    return 0;
}

/* A failed close remains visible and can be retried; input has already been
 * neutralized. Other safe cleanup must proceed rather than leaking everything. */
static int cleanup_error(void)
{
    NEED(sp5_lifecycle_create);
    NEED(sp5_lifecycle_destroy);
    NEED(sp5_lifecycle_start);
    NEED(sp5_lifecycle_end);
    NEED(sp5_lifecycle_read);
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_lifecycle_ops ops = adapters(&fake);
    sp5_lifecycle *lifecycle = sp5_lifecycle_create(81, &ops);
    sp5_media_config config = mode();
    CHECK(lifecycle != NULL && sp5_lifecycle_start(lifecycle, &config, 0).status == SP5_OK);
    CHECK(fake.input.add(&fake, 81, 0).status == SP5_OK);
    sp5_controller_state state = {.buttons = 0x1000, .triggers = {65535, 0}};
    sp5_input_evidence event = {.session_id = 81, .controller_id = 0};
    CHECK(fake.input.deliver(&fake, &state, false, &event).status == SP5_OK);
    fake.fail_next[SP5_FAKE_AUDIO_CLOSE] = SP5_REASON_PLATFORM;
    CHECK(sp5_lifecycle_end(lifecycle, SP5_END_CLIENT_QUIT).status == SP5_ERROR);
    CHECK(!fake.controllers[0].live && fake.controllers[0].state.buttons == 0);
    CHECK(fake.audio_open && !fake.capture_open && !fake.encoder_open);
    CHECK(sp5_lifecycle_read(lifecycle).end_reason == SP5_END_CLIENT_QUIT);
    CHECK(sp5_lifecycle_end(lifecycle, SP5_END_ERROR).status == SP5_OK);
    CHECK(clean(&fake) == 0);
    CHECK(sp5_lifecycle_read(lifecycle).end_reason == SP5_END_CLIENT_QUIT);
    sp5_lifecycle_destroy(lifecycle);
    return 0;
}

static int conflicts_and_loss(void)
{
    NEED(sp5_lifecycle_create);
    NEED(sp5_lifecycle_destroy);
    NEED(sp5_lifecycle_start);
    NEED(sp5_lifecycle_tick);
    NEED(sp5_lifecycle_contact);
    NEED(sp5_lifecycle_read);
    for (unsigned conflict = 0; conflict < 2; ++conflict) {
        for (unsigned active = 0; active < 2; ++active) {
            sp5_fake_platform fake;
            sp5_fake_init(&fake);
            if (!active) {
                fake.system_state.remote_play = conflict == 0;
                fake.system_state.share_play = conflict == 1;
            }
            sp5_lifecycle_ops ops = adapters(&fake);
            sp5_lifecycle *lifecycle = sp5_lifecycle_create(81, &ops);
            sp5_media_config config = mode();
            CHECK(lifecycle != NULL);
            if (active) {
                CHECK(sp5_lifecycle_start(lifecycle, &config, 0).status == SP5_OK);
                fake.system_state.remote_play = conflict == 0;
                fake.system_state.share_play = conflict == 1;
                CHECK(sp5_lifecycle_tick(lifecycle, 1, false).status == SP5_OK);
                CHECK(sp5_lifecycle_read(lifecycle).end_reason == SP5_END_CONSOLE_CONFLICT);
            } else {
                CHECK(sp5_lifecycle_start(lifecycle, &config, 0).reason == SP5_REASON_CONFLICT);
                CHECK(fake.calls[SP5_FAKE_CAPTURE_OPEN] == 0);
            }
            CHECK(clean(&fake) == 0);
            CHECK(fake.system_state.remote_play == (conflict == 0));
            CHECK(fake.system_state.share_play == (conflict == 1));
            sp5_lifecycle_destroy(lifecycle);
        }
    }
    for (unsigned detected = 0; detected < 2; ++detected) {
        sp5_fake_platform fake;
        sp5_fake_init(&fake);
        sp5_lifecycle_ops ops = adapters(&fake);
        sp5_lifecycle *lifecycle = sp5_lifecycle_create(81, &ops);
        sp5_media_config config = mode();
        CHECK(lifecycle != NULL && sp5_lifecycle_start(lifecycle, &config, 0).status == SP5_OK);
        CHECK(sp5_lifecycle_contact(lifecycle, 1).status == SP5_OK);
        CHECK(sp5_lifecycle_tick(lifecycle, UINT64_C(10000000), false).status == SP5_OK);
        CHECK(sp5_lifecycle_read(lifecycle).state == SP5_SESSION_ACTIVE);
        CHECK(sp5_lifecycle_tick(lifecycle, detected ? 10000000 : 10000001, detected != 0).status ==
              SP5_OK);
        CHECK(sp5_lifecycle_read(lifecycle).end_reason == SP5_END_CLIENT_LOST);
        CHECK(clean(&fake) == 0);
        sp5_lifecycle_destroy(lifecycle);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        return 2;
    }
    if (strcmp(argv[1], "durability") == 0) {
        return durability();
    }
    if (strcmp(argv[1], "recovery") == 0) {
        return recovery();
    }
    if (strcmp(argv[1], "unwind") == 0) {
        return unwind();
    }
    if (strcmp(argv[1], "teardown") == 0) {
        return teardown();
    }
    if (strcmp(argv[1], "cleanup_error") == 0) {
        return cleanup_error();
    }
    if (strcmp(argv[1], "conflicts_loss") == 0) {
        return conflicts_and_loss();
    }
    return 2;
}
