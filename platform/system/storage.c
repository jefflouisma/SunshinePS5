#include "settings/host.h"
#include "settings/storage_diagnostic.h"
#include "storage_syscalls.h"
#include "storage_test.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

struct sp5_posix_storage {
    int directory;
#ifdef SP5_STORAGE_TEST
    sp5_posix_replace_test_ops replace_ops;
#endif
};

#ifdef SP5_STORAGE_TEST
void sp5_posix_storage_test_replace_ops(sp5_posix_storage *storage,
                                        const sp5_posix_replace_test_ops *ops)
{
    storage->replace_ops = ops != NULL ? *ops : (sp5_posix_replace_test_ops){0};
}
#endif

static ssize_t replace_write(sp5_posix_storage *storage, int fd, const void *bytes, size_t size)
{
#ifdef SP5_STORAGE_TEST
    if (storage->replace_ops.write_bytes != NULL) {
        return storage->replace_ops.write_bytes(storage->replace_ops.context, fd, bytes, size);
    }
#else
    (void)storage;
#endif
    return write(fd, bytes, size);
}

static int replace_sync(sp5_posix_storage *storage, int fd, bool directory)
{
#ifdef SP5_STORAGE_TEST
    if (storage->replace_ops.sync_file != NULL) {
        return storage->replace_ops.sync_file(storage->replace_ops.context, fd, directory);
    }
#else
    (void)storage;
    (void)directory;
#endif
    return fsync(fd);
}

static int replace_close(sp5_posix_storage *storage, int fd)
{
#ifdef SP5_STORAGE_TEST
    if (storage->replace_ops.close_file != NULL) {
        return storage->replace_ops.close_file(storage->replace_ops.context, fd);
    }
#else
    (void)storage;
#endif
    return close(fd);
}

static int replace_rename(sp5_posix_storage *storage, const char *from, const char *to)
{
#ifdef SP5_STORAGE_TEST
    if (storage->replace_ops.rename_file != NULL) {
        return storage->replace_ops.rename_file(storage->replace_ops.context, storage->directory,
                                                from, to);
    }
#endif
    return sp5_storage_renameat(storage->directory, from, storage->directory, to);
}

static const char *file_name(sp5_store_file file)
{
    switch (file) {
    case SP5_STORE_IDENTITY:
        return "identity.bin";
    case SP5_STORE_SETTINGS:
        return "settings.bin";
    case SP5_STORE_DEVICES:
        return "devices.bin";
    default:
        return NULL;
    }
}

sp5_posix_storage *sp5_posix_storage_open(const char *directory)
{
    if (directory == NULL) {
        return NULL;
    }
    int fd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }
    struct stat info;
    if (fstat(fd, &info) != 0 || info.st_uid != geteuid() || (info.st_mode & 0777) != 0700 ||
        flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (close(fd) != 0) {
            return NULL;
        }
        return NULL;
    }
    sp5_posix_storage *storage = calloc(1, sizeof(*storage));
    if (storage == NULL) {
        if (close(fd) != 0) {
            return NULL;
        }
        return NULL;
    }
    storage->directory = fd;
    return storage;
}

sp5_result sp5_posix_storage_close(sp5_posix_storage *storage)
{
    if (storage == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    int result = close(storage->directory);
    free(storage);
    return result == 0 ? sp5_success() : sp5_failure(SP5_REASON_PLATFORM);
}

static bool secure_file(const struct stat *info)
{
    return S_ISREG(info->st_mode) && info->st_uid == geteuid() && (info->st_mode & 0777) == 0600 &&
           info->st_nlink == 1;
}

static sp5_result read_record(void *context, sp5_store_file file, sp5_buffer *output)
{
    sp5_posix_storage *storage = context;
    const char *name = file_name(file);
    if (storage == NULL || name == NULL || output == NULL || output->data == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    output->size = 0;
    SP5_STORAGE_BEGIN("read.open", file);
    int fd = SP5_STORAGE_OP("read.open",
                            sp5_storage_openat(storage->directory, name,
                                               O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0),
                            -1);
    SP5_STORAGE_END("read.open", file, fd,
                    fd >= 0           ? SP5_REASON_NONE
                    : errno == ENOENT ? SP5_REASON_WOULD_BLOCK
                                      : SP5_REASON_PLATFORM,
                    0);
    if (fd < 0) {
        return sp5_failure(errno == ENOENT ? SP5_REASON_WOULD_BLOCK : SP5_REASON_PLATFORM);
    }
    struct stat info;
    sp5_result result = sp5_success();
    SP5_STORAGE_BEGIN("read.fstat", file);
    int stat_result = SP5_STORAGE_OP("read.fstat", fstat(fd, &info), -1);
    SP5_STORAGE_END("read.fstat", file, stat_result,
                    stat_result == 0 ? SP5_REASON_NONE : SP5_REASON_OWNERSHIP, 0);
    if (stat_result == 0) {
        SP5_STORAGE_END("read.type", file, info.st_mode & S_IFMT, SP5_REASON_NONE, 0);
        SP5_STORAGE_END("read.mode", file, info.st_mode & 0777, SP5_REASON_NONE, 0);
        SP5_STORAGE_END("read.secure", file, (int64_t)secure_file(&info),
                        secure_file(&info) ? SP5_REASON_NONE : SP5_REASON_OWNERSHIP, 0);
    }
    if (stat_result != 0 || !secure_file(&info) || info.st_size < 0) {
        result = sp5_failure(SP5_REASON_OWNERSHIP);
    } else if ((uintmax_t)info.st_size > output->capacity) {
        result = sp5_failure(SP5_REASON_CAPACITY);
    } else {
        size_t expected = (size_t)info.st_size;
        while (output->size < expected) {
            SP5_STORAGE_BEGIN("read.data", file);
            ssize_t count = SP5_STORAGE_OP(
                "read.data", read(fd, output->data + output->size, expected - output->size), -1);
            SP5_STORAGE_END("read.data", file, count,
                            count > 0 ? SP5_REASON_NONE : SP5_REASON_PLATFORM, 0);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                result = sp5_failure(SP5_REASON_PLATFORM);
                break;
            }
            output->size += (size_t)count;
        }
        uint8_t extra;
        ssize_t count;
        do {
            SP5_STORAGE_BEGIN("read.tail", file);
            count = SP5_STORAGE_OP("read.tail", read(fd, &extra, 1), -1);
            SP5_STORAGE_END("read.tail", file, count,
                            count == 0 ? SP5_REASON_NONE : SP5_REASON_PLATFORM, 0);
        } while (count < 0 && errno == EINTR);
        if (count != 0) {
            result = sp5_failure(SP5_REASON_PLATFORM);
        }
    }
    SP5_STORAGE_BEGIN("read.close", file);
    int closed_result = close(fd); /* Consume once, including diagnostic fault tests. */
    int closed = SP5_STORAGE_OP("read.close", closed_result, -1);
    SP5_STORAGE_END("read.close", file, closed, closed == 0 ? SP5_REASON_NONE : SP5_REASON_PLATFORM,
                    0);
    if (closed != 0) {
        result = sp5_failure(SP5_REASON_PLATFORM);
    }
    if (result.status != SP5_OK) {
        output->size = 0;
    }
    SP5_STORAGE_END("read.complete", file, result.status, result.reason, 0);
    return result;
}

static sp5_result replace_record(void *context, sp5_store_file file, sp5_bytes contents)
{
    sp5_posix_storage *storage = context;
    const char *name = file_name(file);
    if (storage == NULL || name == NULL || contents.data == NULL || contents.size == 0 ||
        contents.size > 65536) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    struct stat previous;
    SP5_STORAGE_BEGIN("replace.previous", file);
    int found = sp5_storage_fstatat(storage->directory, name, &previous, AT_SYMLINK_NOFOLLOW);
    SP5_STORAGE_END("replace.previous", file, found,
                    found == 0        ? SP5_REASON_NONE
                    : errno == ENOENT ? SP5_REASON_WOULD_BLOCK
                                      : SP5_REASON_OWNERSHIP,
                    0);
    if ((found == 0 && !secure_file(&previous)) || (found != 0 && errno != ENOENT)) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    uint8_t random[16];
    SP5_STORAGE_BEGIN("replace.random", file);
    int random_ok = SP5_STORAGE_OP("replace.random", RAND_bytes(random, sizeof(random)), 0);
    SP5_STORAGE_END("replace.random", file, random_ok,
                    random_ok == 1 ? SP5_REASON_NONE : SP5_REASON_PLATFORM, ERR_peek_last_error());
    if (random_ok != 1) {
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    static const char hex[] = "0123456789abcdef";
    char temporary[38] = ".new-";
    for (size_t i = 0; i < sizeof(random); ++i) {
        temporary[5 + 2 * i] = hex[random[i] >> 4];
        temporary[6 + 2 * i] = hex[random[i] & 15];
    }
    temporary[37] = 0;
    SP5_STORAGE_BEGIN("replace.temp_create", file);
    int fd = SP5_STORAGE_OP("replace.temp_create",
                            sp5_storage_openat(storage->directory, temporary,
                                               O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                                               0600),
                            -1);
    SP5_STORAGE_END("replace.temp_create", file, fd >= 0 ? 0 : -1,
                    fd >= 0 ? SP5_REASON_NONE : SP5_REASON_PLATFORM, 0);
    if (fd < 0) {
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    sp5_result result = sp5_success();
    size_t written = 0;
    SP5_STORAGE_BEGIN("replace.write", file);
    while (written < contents.size) {
        ssize_t count = SP5_STORAGE_OP(
            "replace.write",
            replace_write(storage, fd, contents.data + written, contents.size - written), -1);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            result = sp5_failure(SP5_REASON_PLATFORM);
            break;
        }
        written += (size_t)count;
    }
    SP5_STORAGE_END("replace.write", file, result.status, result.reason, 0);
    if (result.status == SP5_OK) {
        SP5_STORAGE_BEGIN("replace.file_fsync", file);
        int synced = SP5_STORAGE_OP("replace.file_fsync", replace_sync(storage, fd, false), -1);
        SP5_STORAGE_END("replace.file_fsync", file, synced,
                        synced == 0 ? SP5_REASON_NONE : SP5_REASON_PLATFORM, 0);
        if (synced != 0)
            result = sp5_failure(SP5_REASON_PLATFORM);
    }
    SP5_STORAGE_BEGIN("replace.close", file);
    int close_result = replace_close(storage, fd); /* Consume once even in fault tests. */
    int closed = SP5_STORAGE_OP("replace.close", close_result, -1);
    SP5_STORAGE_END("replace.close", file, closed,
                    closed == 0 ? SP5_REASON_NONE : SP5_REASON_PLATFORM, 0);
    if (closed != 0) {
        result = sp5_failure(SP5_REASON_PLATFORM);
    }
    bool renamed = false;
    if (result.status == SP5_OK) {
        SP5_STORAGE_BEGIN("replace.rename", file);
        int renamed_result =
            SP5_STORAGE_OP("replace.rename", replace_rename(storage, temporary, name), -1);
        SP5_STORAGE_END("replace.rename", file, renamed_result,
                        renamed_result == 0 ? SP5_REASON_NONE : SP5_REASON_PLATFORM, 0);
        if (renamed_result == 0) {
            renamed = true;
            /* Rename is already visible. A directory-sync failure means durability
             * is unknown; return failure without undoing the replacement. */
            SP5_STORAGE_BEGIN("replace.directory_fsync", file);
            int synced = SP5_STORAGE_OP("replace.directory_fsync",
                                        replace_sync(storage, storage->directory, true), -1);
            SP5_STORAGE_END("replace.directory_fsync", file, synced,
                            synced == 0 ? SP5_REASON_NONE : SP5_REASON_PLATFORM, 0);
            if (synced != 0) {
                result = sp5_failure(SP5_REASON_PLATFORM);
            }
        } else {
            result = sp5_failure(SP5_REASON_PLATFORM);
        }
    }
    if (!renamed && sp5_storage_unlinkat(storage->directory, temporary, 0) != 0) {
        result = sp5_failure(SP5_REASON_PLATFORM);
    }
    SP5_STORAGE_END("replace.complete", file, result.status, result.reason, 0);
    return result;
}

static sp5_result generate_identity(void *context, sp5_buffer *output)
{
    if (context == NULL || output == NULL || output->data == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    output->size = 0;
    SP5_STORAGE_BEGIN("identity.generate", SP5_STORE_IDENTITY);
    /* Protocol notes 13.2/13.5: RSA identity and SHA-256 signatures. All entropy,
     * key generation, certificate construction and PEM encoding use OpenSSL. */
    SP5_STORAGE_BEGIN("identity.keygen", SP5_STORE_IDENTITY);
    EVP_PKEY *key =
        SP5_STORAGE_OP("identity.keygen", EVP_PKEY_Q_keygen(NULL, NULL, "RSA", (size_t)2048), NULL);
    SP5_STORAGE_END("identity.keygen", SP5_STORE_IDENTITY, key != NULL,
                    key != NULL ? SP5_REASON_NONE : SP5_REASON_PLATFORM, ERR_peek_last_error());
    SP5_STORAGE_BEGIN("identity.x509_new", SP5_STORE_IDENTITY);
    X509 *certificate = SP5_STORAGE_OP("identity.x509_new", X509_new(), NULL);
    SP5_STORAGE_END("identity.x509_new", SP5_STORE_IDENTITY, certificate != NULL,
                    certificate != NULL ? SP5_REASON_NONE : SP5_REASON_PLATFORM,
                    ERR_peek_last_error());
    SP5_STORAGE_BEGIN("identity.bio_new", SP5_STORE_IDENTITY);
    BIO *pem = SP5_STORAGE_OP("identity.bio_new", BIO_new(BIO_s_mem()), NULL);
    SP5_STORAGE_END("identity.bio_new", SP5_STORE_IDENTITY, pem != NULL,
                    pem != NULL ? SP5_REASON_NONE : SP5_REASON_PLATFORM, ERR_peek_last_error());
    uint8_t serial_bytes[16];
    BIGNUM *serial = NULL;
    sp5_result result = sp5_failure(SP5_REASON_PLATFORM);
    if (key == NULL || certificate == NULL || pem == NULL) {
        goto cleanup;
    }
#define IDENTITY_STEP(stage, expression)                                                           \
    do {                                                                                           \
        SP5_STORAGE_BEGIN(stage, SP5_STORE_IDENTITY);                                              \
        int succeeded = SP5_STORAGE_OP(stage, (expression), 0);                                    \
        SP5_STORAGE_END(stage, SP5_STORE_IDENTITY, succeeded,                                      \
                        succeeded ? SP5_REASON_NONE : SP5_REASON_PLATFORM, ERR_peek_last_error()); \
        if (!succeeded)                                                                            \
            goto cleanup;                                                                          \
    } while (0)
    IDENTITY_STEP("identity.serial_random", RAND_bytes(serial_bytes, sizeof(serial_bytes)) == 1);
    IDENTITY_STEP("identity.serial_bn",
                  (serial = BN_bin2bn(serial_bytes, sizeof(serial_bytes), NULL)) != NULL);
    X509_NAME *name = X509_get_subject_name(certificate);
    IDENTITY_STEP("identity.serial_asn1",
                  BN_to_ASN1_INTEGER(serial, X509_get_serialNumber(certificate)) != NULL);
    IDENTITY_STEP("identity.version", X509_set_version(certificate, 2) == 1);
    IDENTITY_STEP("identity.not_before",
                  X509_gmtime_adj(X509_getm_notBefore(certificate), 0) != NULL);
    IDENTITY_STEP("identity.not_after",
                  X509_gmtime_adj(X509_getm_notAfter(certificate), 315360000L) != NULL);
    IDENTITY_STEP("identity.subject",
                  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                             (const unsigned char *)"SunshinePS5", -1, -1, 0) == 1);
    IDENTITY_STEP("identity.issuer", X509_set_issuer_name(certificate, name) == 1);
    IDENTITY_STEP("identity.public_key", X509_set_pubkey(certificate, key) == 1);
    IDENTITY_STEP("identity.sign", X509_sign(certificate, key, EVP_sha256()) > 0);
    IDENTITY_STEP("identity.pem_private",
                  PEM_write_bio_PrivateKey(pem, key, NULL, NULL, 0, NULL, NULL) == 1);
    IDENTITY_STEP("identity.pem_certificate", PEM_write_bio_X509(pem, certificate) == 1);
    char *data = NULL;
    long length = BIO_get_mem_data(pem, &data);
    IDENTITY_STEP("identity.pem_data", length > 0 && data != NULL);
    SP5_STORAGE_BEGIN("identity.append", SP5_STORE_IDENTITY);
    result = SP5_STORAGE_OP(
        "identity.append",
        sp5_buffer_append(output, (sp5_bytes){(const uint8_t *)data, (size_t)length}),
        sp5_failure(SP5_REASON_PLATFORM));
    SP5_STORAGE_END("identity.append", SP5_STORE_IDENTITY, result.status, result.reason, 0);
#undef IDENTITY_STEP
cleanup:
    if (pem != NULL) {
        char *secret = NULL;
        long length = BIO_get_mem_data(pem, &secret);
        if (length > 0 && secret != NULL) {
            OPENSSL_cleanse(secret, (size_t)length);
        }
    }
    BN_free(serial);
    BIO_free(pem);
    X509_free(certificate);
    EVP_PKEY_free(key);
    OPENSSL_cleanse(serial_bytes, sizeof(serial_bytes));
    SP5_STORAGE_END("identity.generate", SP5_STORE_IDENTITY, result.status, result.reason,
                    ERR_peek_last_error());
    return result;
}

sp5_storage_ops sp5_posix_storage_operations(sp5_posix_storage *storage)
{
    return (sp5_storage_ops){storage, read_record, replace_record, generate_identity};
}
