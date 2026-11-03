#include "../../platform/system/storage_test.h"
#include "settings/devices.h"
#include "settings/host.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/pem.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

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
static sp5_bytes text(const char *value)
{
    return (sp5_bytes){(const uint8_t *)value, strlen(value)};
}

static int host_records(void)
{
    CHECK(!sp5_name_valid(text("")));
    CHECK(!sp5_name_valid(text("line\nbreak")));
    CHECK(!sp5_name_valid(text("bad\x7f")));
    CHECK(!sp5_name_valid(text("bad\xc2\x85")));
    CHECK(!sp5_name_valid(text("\xc0\xaf")));
    CHECK(!sp5_name_valid(text("\xed\xa0\x80")));
    CHECK(!sp5_name_valid(text("\xf4\x90\x80\x80")));
    CHECK(!sp5_name_valid(text("\xe2\x82")));
    CHECK(sp5_name_valid(text("Living room \xf0\x9f\x8e\xae")));
    char limit[65];
    memset(limit, 'x', 64);
    limit[64] = 0;
    CHECK(!sp5_name_valid(text(limit)));
    limit[63] = 0;
    CHECK(sp5_name_valid(text(limit)));
    CHECK(strstr(sp5_name_requirement(), "too long") != NULL);
    uint8_t multibyte[64];
    for (size_t i = 0; i < 64; i += 2) {
        multibyte[i] = 0xc3;
        multibyte[i + 1] = 0xa9;
    }
    CHECK(!sp5_name_valid((sp5_bytes){multibyte, 64})); /* 32 scalars, 64 octets */
    CHECK(sp5_name_valid((sp5_bytes){multibyte, 62}));
    multibyte[62] = 'x';
    CHECK(sp5_name_valid((sp5_bytes){multibyte, 63}));
    sp5_settings settings = sp5_settings_defaults();
    CHECK(!settings.metrics_enabled && strcmp(settings.host_name, "SunshinePS5") == 0);
    memset(settings.host_name, 'x', sizeof(settings.host_name));
    CHECK(!sp5_settings_valid(&settings));
    sp5_host host;
    CHECK(sp5_host_init(&host, "test-build", "13.60", "192.168.2.2").status == SP5_OK);
    CHECK(host.state == SP5_HOST_STARTING && host.supported_mode_count == 0);
    CHECK(strcmp(host.version, "test-build") == 0);
    CHECK(strcmp(host.address, "192.168.2.2") == 0);
    CHECK(sp5_host_settings(&host, &settings).status == SP5_ERROR);
    CHECK(strcmp(host.settings.host_name, "SunshinePS5") == 0);
    settings = (sp5_settings){.host_name = "Den", .metrics_enabled = true};
    CHECK(sp5_host_settings(&host, &settings).status == SP5_OK);
    CHECK(strcmp(host.settings.host_name, "Den") == 0 && host.settings.metrics_enabled);
    CHECK(sp5_host_transition(&host, SP5_HOST_RUNNING).status == SP5_OK);
    CHECK(sp5_host_transition(&host, SP5_HOST_STARTING).status == SP5_ERROR);
    CHECK(sp5_host_transition(&host, SP5_HOST_STOPPING).status == SP5_OK);
    CHECK(sp5_host_transition(&host, SP5_HOST_RUNNING).status == SP5_ERROR);
    CHECK(sp5_host_init(&host, "test-build", "12.00", "192.168.2.2").reason ==
          SP5_REASON_UNSUPPORTED);
    CHECK(sp5_host_transition(&host, SP5_HOST_RUNNING).reason == SP5_REASON_UNSUPPORTED);
    return 0;
}

static int disk_storage(void)
{
    const char *temporary = getenv("TMPDIR");
    CHECK(temporary != NULL);
    char directory[1024];
    CHECK(snprintf(directory, sizeof(directory), "%s/settings-XXXXXX", temporary) > 0);
    CHECK(mkdtemp(directory) != NULL);
    sp5_posix_storage *disk = sp5_posix_storage_open(directory);
    CHECK(disk != NULL);
    CHECK(sp5_posix_storage_open(directory) == NULL);
    sp5_storage_ops ops = sp5_posix_storage_operations(disk);
    sp5_store *store = sp5_store_create(&ops);
    CHECK(store != NULL && sp5_store_load(store).status == SP5_OK);
    sp5_host host;
    CHECK(sp5_host_init(&host, "test-build", "13.60", "192.168.2.2").status == SP5_OK);
    CHECK(sp5_host_refresh(&host, store).status == SP5_OK && host.identity.size > 0);
    sp5_bytes identity = sp5_store_identity(store);
    CHECK(identity.size > 0 && identity.size <= SP5_IDENTITY_CAPACITY);
    uint8_t saved[SP5_IDENTITY_CAPACITY];
    size_t saved_size = identity.size;
    memcpy(saved, identity.data, identity.size);
    BIO *pem = BIO_new_mem_buf(identity.data, (int)identity.size);
    CHECK(pem != NULL);
    EVP_PKEY *key = PEM_read_bio_PrivateKey(pem, NULL, NULL, NULL);
    X509 *cert = PEM_read_bio_X509(pem, NULL, NULL, NULL);
    CHECK(key != NULL && cert != NULL && X509_check_private_key(cert, key) == 1);
    CHECK(EVP_PKEY_get_bits(key) == 2048 && X509_verify(cert, key) == 1);
    X509_free(cert);
    EVP_PKEY_free(key);
    CHECK(BIO_free(pem) == 1);
    sp5_settings changed = {.host_name = "Den", .metrics_enabled = true};
    CHECK(sp5_store_save_settings(store, &changed).status == SP5_OK);
    sp5_devices *devices = sp5_store_devices(store);
    CHECK(devices != NULL && sp5_devices_count(devices) == 0);
    CHECK(sp5_devices_pair(devices, text("verified-fixture-certificate"), text("Tablet"), 100)
              .status == SP5_OK);
    CHECK(sp5_devices_pair(devices, text("verified-fixture-certificate"), text("Ignore duplicate"),
                           200)
              .status == SP5_OK);
    const sp5_device *device = sp5_devices_at(devices, 0);
    CHECK(device != NULL && device->paired_at == 100 && !device->last_session_at.present);
    CHECK(strlen(device->id) == 64 && strcmp(device->name, "Tablet") == 0);
    char id[65];
    memcpy(id, device->id, sizeof(id));
    CHECK(sp5_devices_rename(devices,
                             (sp5_device_name){text("verified-fixture-certificate"), text("\n")})
              .status == SP5_ERROR);
    CHECK(sp5_devices_rename(devices,
                             (sp5_device_name){text("verified-fixture-certificate"), text("Phone")})
              .status == SP5_OK);
    CHECK(sp5_devices_session_ended(devices, text("verified-fixture-certificate"), 150).status ==
          SP5_OK);
    CHECK(!sp5_devices_authorized(devices, text(id)));
    sp5_store_destroy(store);
    store = sp5_store_create(&ops);
    CHECK(store != NULL && sp5_store_load(store).status == SP5_OK);
    CHECK(sp5_store_identity(store).size == saved_size);
    CHECK(memcmp(saved, sp5_store_identity(store).data, saved_size) == 0);
    CHECK(sp5_store_settings(store).metrics_enabled);
    devices = sp5_store_devices(store);
    device = sp5_devices_at(devices, 0);
    CHECK(device != NULL && strcmp(device->id, id) == 0 && strcmp(device->name, "Phone") == 0);
    CHECK(device->paired_at == 100 && device->last_session_at.present &&
          device->last_session_at.value == 150);
    CHECK(sp5_devices_revoke(devices, text("verified-fixture-certificate")).status == SP5_OK);
    CHECK(!sp5_store_authorized(store, text("verified-fixture-certificate")));
    CHECK(sp5_devices_revoke(devices, text("verified-fixture-certificate")).status == SP5_OK);
    sp5_store_destroy(store);
    store = sp5_store_create(&ops);
    CHECK(store != NULL && sp5_store_load(store).status == SP5_OK);
    CHECK(sp5_store_read(store).paired_count == 0);
    sp5_store_destroy(store);
    CHECK(sp5_posix_storage_close(disk).status == SP5_OK);
    char identity_path[1200], backup_path[1200];
    CHECK(snprintf(identity_path, sizeof(identity_path), "%s/identity.bin", directory) > 0);
    CHECK(snprintf(backup_path, sizeof(backup_path), "%s/identity.backup", directory) > 0);
    CHECK(chmod(identity_path, 0644) == 0);
    disk = sp5_posix_storage_open(directory);
    CHECK(disk != NULL);
    ops = sp5_posix_storage_operations(disk);
    store = sp5_store_create(&ops);
    CHECK(store != NULL && sp5_store_load(store).status == SP5_ERROR);
    CHECK(sp5_store_identity(store).size == 0);
    sp5_store_destroy(store);
    CHECK(chmod(identity_path, 0600) == 0);
    CHECK(rename(identity_path, backup_path) == 0);
    CHECK(symlink("identity.backup", identity_path) == 0);
    store = sp5_store_create(&ops);
    CHECK(store != NULL && sp5_store_load(store).status == SP5_ERROR);
    CHECK(ops.replace(ops.context, SP5_STORE_IDENTITY, text("must-not-follow-link")).reason ==
          SP5_REASON_OWNERSHIP);
    sp5_store_destroy(store);
    CHECK(unlink(identity_path) == 0 && rename(backup_path, identity_path) == 0);
    CHECK(sp5_posix_storage_close(disk).status == SP5_OK);
    CHECK(chmod(directory, 0755) == 0 && sp5_posix_storage_open(directory) == NULL);
    CHECK(chmod(directory, 0700) == 0);
    const char *names[] = {"identity.bin", "settings.bin", "devices.bin"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char path[1200];
        CHECK(snprintf(path, sizeof(path), "%s/%s", directory, names[i]) > 0);
        struct stat info;
        CHECK(stat(path, &info) == 0 && (info.st_mode & 0777) == 0600);
        CHECK(unlink(path) == 0);
    }
    CHECK(rmdir(directory) == 0);
    return 0;
}

typedef enum {
    REPLACE_WRITE,
    REPLACE_FILE_SYNC,
    REPLACE_CLOSE,
    REPLACE_RENAME,
    REPLACE_DIRECTORY_SYNC,
    REPLACE_SHORT_INTERRUPTED_WRITE
} replace_fault;

typedef struct {
    replace_fault fault;
    unsigned writes, file_syncs, closes, renames, directory_syncs;
    int written_fd;
    bool injected;
} replace_fault_state;

static ssize_t replace_write(void *context, int fd, const void *bytes, size_t size)
{
    replace_fault_state *state = context;
    state->written_fd = fd;
    ++state->writes;
    if (state->fault == REPLACE_WRITE && state->writes == 2) {
        state->injected = true;
        errno = EIO;
        return -1;
    }
    if (state->fault == REPLACE_SHORT_INTERRUPTED_WRITE && state->writes == 1) {
        state->injected = true;
        errno = EINTR;
        return -1;
    }
    if ((state->fault == REPLACE_WRITE && state->writes == 1) ||
        (state->fault == REPLACE_SHORT_INTERRUPTED_WRITE && state->writes == 2)) {
        return write(fd, bytes, size > 3 ? 3 : size);
    }
    return write(fd, bytes, size);
}

static int replace_sync(void *context, int fd, bool directory)
{
    replace_fault_state *state = context;
    if (directory) {
        ++state->directory_syncs;
    } else {
        ++state->file_syncs;
    }
    if ((directory && state->fault == REPLACE_DIRECTORY_SYNC) ||
        (!directory && state->fault == REPLACE_FILE_SYNC)) {
        state->injected = true;
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}

static int replace_close(void *context, int fd)
{
    replace_fault_state *state = context;
    ++state->closes;
    int result = close(fd);
    if (result == 0 && state->fault == REPLACE_CLOSE) {
        /* Model a delayed close error after the descriptor has been consumed. */
        state->injected = true;
        errno = EIO;
        return -1;
    }
    return result;
}

static int replace_rename(void *context, int directory, const char *from, const char *to)
{
    replace_fault_state *state = context;
    ++state->renames;
    if (state->fault == REPLACE_RENAME) {
        state->injected = true;
        errno = EIO;
        return -1;
    }
    return renameat(directory, from, directory, to);
}

static int storage_directory_clean(const char *directory)
{
    DIR *entries = opendir(directory);
    CHECK(entries != NULL);
    unsigned records = 0;
    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(entries)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        CHECK(strcmp(entry->d_name, "identity.bin") == 0 ||
              strcmp(entry->d_name, "settings.bin") == 0);
        ++records;
    }
    CHECK(errno == 0 && records == 2);
    CHECK(closedir(entries) == 0);
    return 0;
}

static int disk_replace_failures(void)
{
    const char *temporary = getenv("TMPDIR");
    CHECK(temporary != NULL);
    char directory[1024];
    CHECK(snprintf(directory, sizeof(directory), "%s/replace-faults-XXXXXX", temporary) > 0);
    CHECK(mkdtemp(directory) != NULL);
    sp5_posix_storage *disk = sp5_posix_storage_open(directory);
    CHECK(disk != NULL);
    sp5_storage_ops ops = sp5_posix_storage_operations(disk);
    sp5_store *store = sp5_store_create(&ops);
    CHECK(store != NULL && sp5_store_load(store).status == SP5_OK);
    const sp5_settings before = {.host_name = "Before", .metrics_enabled = false};
    const sp5_settings after = {.host_name = "After", .metrics_enabled = true};
    const char *names[] = {"partial-write-error",  "file-sync-error",
                           "consumed-close-error", "rename-error",
                           "directory-sync-error", "interrupted-short-write"};
    for (unsigned fault = REPLACE_WRITE; fault <= REPLACE_SHORT_INTERRUPTED_WRITE; ++fault) {
        CHECK(sp5_store_save_settings(store, &before).status == SP5_OK);
        replace_fault_state state = {.fault = (replace_fault)fault, .written_fd = -1};
        sp5_posix_replace_test_ops injected = {&state, replace_write, replace_sync, replace_close,
                                               replace_rename};
        sp5_posix_storage_test_replace_ops(disk, &injected);
        sp5_result result = sp5_store_save_settings(store, &after);
        bool succeeded = fault == REPLACE_SHORT_INTERRUPTED_WRITE;
        bool replaced = succeeded || fault == REPLACE_DIRECTORY_SYNC;
        CHECK(result.status == (succeeded ? SP5_OK : SP5_ERROR));
        CHECK(succeeded || result.reason == SP5_REASON_PLATFORM);
        CHECK(state.injected && state.closes == 1 && state.written_fd >= 0);
        errno = 0;
        CHECK(fcntl(state.written_fd, F_GETFD) == -1 && errno == EBADF);
        CHECK(state.writes == (fault == REPLACE_WRITE ? 2U : succeeded ? 3U : 1U));
        CHECK(state.file_syncs == (fault == REPLACE_WRITE ? 0U : 1U));
        CHECK(state.renames == (fault >= REPLACE_RENAME ? 1U : 0U));
        CHECK(state.directory_syncs == (replaced ? 1U : 0U));
        CHECK(storage_directory_clean(directory) == 0);
        sp5_settings current = sp5_store_settings(store);
        CHECK(strcmp(current.host_name, succeeded ? "After" : "Before") == 0);
        CHECK(current.metrics_enabled == succeeded);
        CHECK(succeeded || sp5_store_read(store).diagnostic == SP5_REASON_PLATFORM);

        /* Read the actual replacement through the secured disk adapter. A
         * directory-sync error is failure even though rename is already visible. */
        uint8_t raw[128];
        sp5_buffer record = {raw, sizeof(raw), 0};
        sp5_bytes payload;
        CHECK(ops.read(ops.context, SP5_STORE_SETTINGS, &record).status == SP5_OK);
        CHECK(
            sp5_record_unpack(SP5_STORE_SETTINGS, (sp5_bytes){raw, record.size}, &payload).status ==
            SP5_OK);
        const char *expected = replaced ? "After" : "Before";
        CHECK(payload.size == strlen(expected) + 1 && payload.data[0] == (replaced ? 1 : 0));
        CHECK(memcmp(payload.data + 1, expected, strlen(expected)) == 0);
        sp5_posix_storage_test_replace_ops(disk, NULL);
        sp5_store_destroy(store);
        CHECK(sp5_posix_storage_close(disk).status == SP5_OK);
        disk = sp5_posix_storage_open(directory);
        CHECK(disk != NULL);
        ops = sp5_posix_storage_operations(disk);
        store = sp5_store_create(&ops);
        CHECK(store != NULL && sp5_store_load(store).status == SP5_OK);
        current = sp5_store_settings(store);
        CHECK(strcmp(current.host_name, expected) == 0 && current.metrics_enabled == replaced);
        /* A subsequent full replacement can establish a fresh successful save. */
        CHECK(sp5_store_save_settings(store, &after).status == SP5_OK);
        CHECK(storage_directory_clean(directory) == 0);
        CHECK(printf("posix_replace case=%s result=%s memory=%s disk=%s reload=%s "
                     "temporary_files=0 descriptor=closed retry=success\n",
                     names[fault], succeeded ? "success" : "failure",
                     succeeded ? "After" : "Before", expected, expected) > 0);
    }
    sp5_store_destroy(store);
    CHECK(sp5_posix_storage_close(disk).status == SP5_OK);
    const char *files[] = {"identity.bin", "settings.bin"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        char path[1200];
        CHECK(snprintf(path, sizeof(path), "%s/%s", directory, files[i]) > 0);
        CHECK(unlink(path) == 0);
    }
    CHECK(rmdir(directory) == 0);
    return 0;
}

/* Deliberately synthetic persistence with injected failures. It is not a
 * console filesystem or power-loss simulator. */
typedef struct {
    uint8_t raw[SP5_STORE_FILE_COUNT][40000];
    size_t size[SP5_STORE_FILE_COUNT];
    bool exists[SP5_STORE_FILE_COUNT];
    bool fail_replace, fail_read, fail_generation;
    unsigned generations;
    sp5_devices *observed;
    bool observed_authorization;
    sp5_device_lease observed_lease;
    bool observed_lease_valid;
} fault_files;

static sp5_result fault_read(void *context, sp5_store_file file, sp5_buffer *output)
{
    fault_files *files = context;
    if (files->fail_read) {
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    if (!files->exists[file]) {
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    }
    return sp5_buffer_append(output, (sp5_bytes){files->raw[file], files->size[file]});
}

static sp5_result fault_replace(void *context, sp5_store_file file, sp5_bytes contents)
{
    fault_files *files = context;
    if (files->observed != NULL) {
        files->observed_authorization = sp5_devices_authorized(files->observed, text("first"));
        files->observed_lease_valid = sp5_device_lease_valid(files->observed_lease);
    }
    if (files->fail_replace || contents.size > sizeof(files->raw[file])) {
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    memcpy(files->raw[file], contents.data, contents.size);
    files->size[file] = contents.size;
    files->exists[file] = true;
    return sp5_success();
}

static sp5_result fault_generate(void *context, sp5_buffer *output)
{
    fault_files *files = context;
    ++files->generations;
    return files->fail_generation ? sp5_failure(SP5_REASON_PLATFORM)
                                  : sp5_buffer_append(output, text("synthetic-identity"));
}

static int registry_failures(void)
{
    fault_files files = {0};
    sp5_storage_ops ops = {&files, fault_read, fault_replace, fault_generate};
    sp5_devices *devices = sp5_devices_create(&ops);
    CHECK(devices != NULL && sp5_devices_load(devices).status == SP5_OK);
    files.observed = devices;
    uint8_t name[64];
    for (size_t i = 0; i < sizeof(name); i += 2) {
        name[i] = 0xc3;
        name[i + 1] = 0xa9;
    }
    CHECK(sp5_devices_pair(devices, text("name-boundary"), (sp5_bytes){name, 64}, 1).status ==
          SP5_ERROR);
    CHECK(sp5_devices_count(devices) == 0);
    name[62] = 'x';
    CHECK(sp5_devices_pair(devices, text("name-boundary"), (sp5_bytes){name, 63}, 1).status ==
          SP5_OK);
    name[62] = 0xc3;
    CHECK(
        sp5_devices_rename(devices, (sp5_device_name){text("name-boundary"), {name, 64}}).status ==
        SP5_ERROR);
    CHECK(strlen(sp5_devices_at(devices, 0)->name) == 63);
    CHECK(sp5_devices_revoke(devices, text("name-boundary")).status == SP5_OK);
    uint8_t oversized[SP5_CERTIFICATE_CAPACITY + 1] = {0};
    CHECK(sp5_devices_pair(devices, (sp5_bytes){oversized, sizeof(oversized)}, text("One"), 10)
              .status == SP5_ERROR);
    CHECK(sp5_devices_pair(devices, text("first"), text(""), 10).status == SP5_ERROR);
    CHECK(sp5_devices_pair(devices, text("first"), text("One"), 10).status == SP5_OK);
    CHECK(!files.observed_authorization);
    CHECK(sp5_devices_authorized(devices, text("first")));
    files.observed_lease = sp5_devices_lease(devices, text("first"));
    CHECK(sp5_device_lease_valid(files.observed_lease));
    CHECK(!sp5_device_lease_valid(sp5_devices_lease(devices, text("claimed-first"))));
    files.fail_replace = true;
    CHECK(sp5_devices_pair(devices, text("second"), text("Two"), 10).status == SP5_ERROR);
    CHECK(!sp5_devices_authorized(devices, text("second")));
    CHECK(sp5_devices_rename(devices, (sp5_device_name){text("first"), text("Changed")}).status ==
          SP5_ERROR);
    CHECK(strcmp(sp5_devices_at(devices, 0)->name, "One") == 0);
    CHECK(sp5_devices_session_ended(devices, text("first"), 20).status == SP5_ERROR);
    CHECK(!sp5_devices_at(devices, 0)->last_session_at.present);
    CHECK(sp5_devices_revoke(devices, text("first")).status == SP5_ERROR);
    CHECK(!files.observed_authorization && !sp5_devices_authorized(devices, text("first")));
    CHECK(!files.observed_lease_valid && !sp5_device_lease_valid(files.observed_lease));
    CHECK(sp5_devices_load(devices).status == SP5_ERROR);
    CHECK(!sp5_devices_authorized(devices, text("first")));
    files.fail_replace = false;
    CHECK(sp5_devices_revoke(devices, text("first")).status == SP5_OK);
    CHECK(sp5_devices_load(devices).status == SP5_OK && sp5_devices_count(devices) == 0);
    for (unsigned i = 0; i < SP5_DEVICE_CAPACITY; ++i) {
        uint8_t certificate = (uint8_t)i;
        CHECK(sp5_devices_pair(devices, (sp5_bytes){&certificate, 1}, text("Device"), i).status ==
              SP5_OK);
    }
    CHECK(sp5_devices_pair(devices, text("overflow"), text("Device"), 20).reason ==
          SP5_REASON_CAPACITY);
    CHECK(sp5_devices_load(devices).status == SP5_OK &&
          sp5_devices_count(devices) == SP5_DEVICE_CAPACITY);
    size_t original_size = files.size[SP5_STORE_DEVICES];
    for (size_t length = 0; length < original_size; ++length) {
        files.size[SP5_STORE_DEVICES] = length;
        CHECK(sp5_devices_load(devices).status == SP5_ERROR && sp5_devices_count(devices) == 0);
        CHECK(sp5_devices_repairing_required(devices));
        CHECK(strstr(sp5_devices_explanation(devices), "Pair again") != NULL);
    }
    files.size[SP5_STORE_DEVICES] = original_size;
    CHECK(sp5_devices_load(devices).status == SP5_OK);
    /* Recompute the integrity digest: semantic validation must reject even
     * self-consistent oversized counts and duplicate certificate identities. */
    sp5_buffer malformed = {files.raw[SP5_STORE_DEVICES], sizeof(files.raw[SP5_STORE_DEVICES]), 0};
    uint8_t *payload = malformed.data + 12;
    payload[0] = SP5_DEVICE_CAPACITY + 1;
    CHECK(sp5_record_pack(SP5_STORE_DEVICES,
                          (sp5_bytes){payload, original_size - SP5_RECORD_OVERHEAD}, &malformed)
              .status == SP5_OK);
    CHECK(sp5_devices_load(devices).status == SP5_ERROR && sp5_devices_count(devices) == 0);
    payload[0] = 2;
    memcpy(payload + 28, payload + 1, 27);
    CHECK(sp5_record_pack(SP5_STORE_DEVICES, (sp5_bytes){payload, 55}, &malformed).status ==
          SP5_OK);
    files.size[SP5_STORE_DEVICES] = malformed.size;
    CHECK(sp5_devices_load(devices).status == SP5_ERROR && sp5_devices_count(devices) == 0);
    files.fail_read = true;
    CHECK(sp5_devices_load(devices).status == SP5_ERROR && sp5_devices_count(devices) == 0);
    sp5_devices_destroy(devices);
    return 0;
}

static int identity_failures(void)
{
    fault_files files = {0};
    sp5_storage_ops ops = {&files, fault_read, fault_replace, fault_generate};
    sp5_store *store = sp5_store_create(&ops);
    CHECK(store != NULL);
    files.fail_generation = true;
    CHECK(sp5_store_load(store).status == SP5_ERROR && !sp5_store_read(store).identity_valid);
    files.fail_generation = false;
    files.fail_replace = true;
    CHECK(sp5_store_load(store).status == SP5_ERROR && sp5_store_identity(store).size == 0);
    files.fail_replace = false;
    CHECK(sp5_store_load(store).status == SP5_OK);
    /* Unix microseconds like every other device timestamp (web divides by 1000). */
    const uint64_t paired_at = UINT64_C(1761800000123456);
    CHECK(sp5_store_pair(store, text("first"), 0).status == SP5_ERROR);
    CHECK(sp5_store_pair(store, text("first"), paired_at).status == SP5_OK);
    CHECK(sp5_devices_at(sp5_store_devices(store), 0)->paired_at == paired_at);
    sp5_settings settings = {.host_name = "Changed", .metrics_enabled = true};
    CHECK(sp5_store_save_settings(store, &settings).status == SP5_OK);
    unsigned generations = files.generations;
    files.exists[SP5_STORE_IDENTITY] = false;
    CHECK(sp5_store_load(store).status == SP5_ERROR);
    CHECK(files.generations == generations && !sp5_store_authorized(store, text("first")));
    CHECK(sp5_store_read(store).repairing_required);
    files.exists[SP5_STORE_IDENTITY] = true;
    CHECK(sp5_store_load(store).status == SP5_OK);
    files.fail_read = true;
    CHECK(sp5_store_load(store).status == SP5_ERROR && sp5_store_identity(store).size == 0);
    CHECK(!sp5_store_authorized(store, text("first")));
    CHECK(strcmp(sp5_store_settings(store).host_name, "SunshinePS5") == 0);
    sp5_store_destroy(store);
    return 0;
}

int main(void)
{
    int result = host_records();
    if (result == 0) {
        result = disk_storage();
    }
    if (result == 0) {
        result = disk_replace_failures();
    }
    if (result == 0) {
        result = registry_failures();
    }
    if (result == 0) {
        result = identity_failures();
    }
    return result;
}
