/* Direct USB approach: talkaboutdesign/elgato-hdr, commit
 * 7799d3c53520bcc3dd049211244bfaf49b905cb0, tools/uvc_capture.c.
 * Independent bounded implementation; see protocol.md for framing and limits. */
#include <errno.h>
#include <fcntl.h>
#include <libusb.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FRAME_BYTES 6220800U
#define INTERVAL 166666U
#define TRANSFERS 16
#define MAX_PAYLOAD 65536U
#define OUTPUT_TIMEOUT_US 2000000U

struct mode {
    uint8_t format, frame, endpoint, control, stream;
};
struct capture {
    struct mode mode;
    libusb_context *context;
    libusb_device_handle *handle;
    struct libusb_transfer *transfers[TRANSFERS];
    bool pending[TRANSFERS], claimed[2], detached[2];
    uint8_t *frame;
    uint32_t fill, payload, emitted, requested;
    uint64_t sequence, bad, deadline, last_frame;
    int fid;
    bool poisoned, finished, failed;
};
static volatile sig_atomic_t interrupted;
static void on_signal(int number)
{
    (void)number;
    interrupted = 1;
}
static void log_message(const char *message)
{
    size_t remaining = strlen(message);
    while (remaining > 0) {
        ssize_t written = write(STDERR_FILENO, message, remaining);
        if (written <= 0)
            return;
        message += written;
        remaining -= (size_t)written;
    }
}
static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned int i = 0; i < 4; ++i)
        p[i] = (uint8_t)(value >> (i * 8));
}
static void put64(uint8_t *p, uint64_t value)
{
    for (unsigned int i = 0; i < 8; ++i)
        p[i] = (uint8_t)(value >> (i * 8));
}
static uint64_t now_us(void)
{
    struct timespec time = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0 || time.tv_sec < 0) {
        interrupted = 1;
        return 0;
    }
    return (uint64_t)time.tv_sec * 1000000U + (uint64_t)time.tv_nsec / 1000U;
}
/* UVC uncompressed/frame-based format/frame layouts; every access length-checked. */
static bool scan_modes(const uint8_t *p, size_t length, struct mode *mode)
{
    uint8_t format = 0;
    bool p010 = false;
    while (length >= 3) {
        size_t size = p[0];
        if (size < 3 || size > length)
            return false;
        if (p[1] == 0x24 && (p[2] == 4 || p[2] == 0x10)) {
            if (size < 27)
                return false;
            format = p[3];
            static const uint8_t guid[16] = {'P',  '0', '1', '0',  0, 0,    0x10, 0,
                                             0x80, 0,   0,   0xaa, 0, 0x38, 0x9b, 0x71};
            p010 = memcmp(p + 5, guid, sizeof(guid)) == 0 && p[21] == 24;
        } else if (p[1] == 0x24 && p[2] == 6) {
            p010 = false;
        } else if (p[1] == 0x24 && (p[2] == 5 || p[2] == 0x11) && p010) {
            if (size < 26)
                return false;
            size_t count_offset = p[2] == 0x11 ? 21 : 25;
            size_t intervals_offset = 26;
            uint8_t count = p[count_offset];
            if (count == 0 || count > (size - intervals_offset) / 4)
                return false;
            for (size_t i = 0; i < count; ++i) {
                if (le16(p + 5) == 1920 && le16(p + 7) == 1080 &&
                    le32(p + intervals_offset + i * 4) == INTERVAL) {
                    mode->format = format;
                    mode->frame = p[3];
                    return true;
                }
            }
        }
        p += size;
        length -= size;
    }
    return false;
}
static bool describe(libusb_device *device, struct mode *mode)
{
    struct libusb_config_descriptor *config = NULL;
    if (libusb_get_active_config_descriptor(device, &config) != 0)
        return false;
    bool found = false, control = false;
    for (uint8_t i = 0; i < config->bNumInterfaces; ++i) {
        const struct libusb_interface *interface = &config->interface[i];
        for (int j = 0; j < interface->num_altsetting; ++j) {
            const struct libusb_interface_descriptor *desc = &interface->altsetting[j];
            if (desc->bInterfaceClass != 0x0e)
                continue;
            if (desc->bInterfaceSubClass == 1) {
                mode->control = desc->bInterfaceNumber;
                control = true;
            }
            if (desc->bInterfaceSubClass != 2 || desc->bAlternateSetting != 0 ||
                desc->extra_length < 0 ||
                !scan_modes(desc->extra, (size_t)desc->extra_length, mode))
                continue;
            for (uint8_t e = 0; e < desc->bNumEndpoints; ++e) {
                const struct libusb_endpoint_descriptor *endpoint = &desc->endpoint[e];
                if ((endpoint->bEndpointAddress & LIBUSB_ENDPOINT_IN) != 0 &&
                    (endpoint->bmAttributes & 3) == LIBUSB_TRANSFER_TYPE_BULK) {
                    mode->endpoint = endpoint->bEndpointAddress;
                    mode->stream = desc->bInterfaceNumber;
                    found = true;
                }
            }
        }
    }
    libusb_free_config_descriptor(config);
    return found && control && mode->control != mode->stream;
}
/* Nonblocking stdout is the designated bounded backpressure point. */
static bool write_all(const uint8_t *bytes, size_t size)
{
    uint64_t deadline = now_us() + OUTPUT_TIMEOUT_US;
    while (size != 0 && !interrupted) {
        ssize_t written = write(STDOUT_FILENO, bytes, size);
        if (written > 0) {
            bytes += written;
            size -= (size_t)written;
            continue;
        }
        if (written == 0 || (errno != EINTR && errno != EAGAIN))
            return false;
        if (now_us() >= deadline)
            return false;
        struct pollfd poll_fd = {.fd = STDOUT_FILENO, .events = POLLOUT};
        if (poll(&poll_fd, 1, 20) < 0 && errno != EINTR)
            return false;
    }
    return size == 0;
}
struct frame_record {
    const char *magic;
    uint64_t stamp, sequence, bad;
    uint32_t size, restored, reason;
};
static bool record_header(const struct frame_record *record)
{
    uint8_t header[48] = {0};
    memcpy(header, record->magic, 4);
    put32(header + 4, record->size);
    put64(header + 8, record->stamp);
    put64(header + 16, record->sequence);
    put64(header + 24, record->bad);
    put32(header + 32, record->restored);
    put32(header + 36, record->reason);
    return write_all(header, sizeof(header));
}
static void end_frame(struct capture *capture, bool eof)
{
    if (capture->fill == 0 && !capture->poisoned)
        return;
    ++capture->sequence;
    if (!eof || capture->poisoned || capture->fill != FRAME_BYTES) {
        ++capture->bad;
    } else {
        uint64_t stamp = now_us();
        if (stamp <= capture->last_frame ||
            !record_header(&(struct frame_record){.magic = "FRAM",
                                                  .stamp = stamp,
                                                  .sequence = capture->sequence,
                                                  .bad = capture->bad,
                                                  .size = FRAME_BYTES}) ||
            !write_all(capture->frame, FRAME_BYTES)) {
            capture->failed = true;
            capture->finished = true;
        } else {
            capture->last_frame = stamp;
            ++capture->emitted;
            capture->finished = capture->emitted == capture->requested;
        }
    }
    capture->fill = 0;
    capture->poisoned = false;
}
static void parse_transfer(struct capture *capture, const uint8_t *buffer, size_t length)
{
    while (length != 0 && !capture->finished) {
        size_t header = buffer[0];
        if (length < 2 || header < 2 || header > 12 || header > length ||
            header > capture->payload) {
            capture->poisoned = true;
            end_frame(capture, false);
            capture->fid = -1;
            return;
        }
        uint8_t flags = buffer[1];
        size_t required = 2U + ((flags & 4U) != 0 ? 4U : 0U) + ((flags & 8U) != 0 ? 6U : 0U);
        if (header < required) {
            capture->poisoned = true;
            end_frame(capture, false);
            return;
        }
        int fid = flags & 1;
        if (capture->fid >= 0 && fid != capture->fid)
            end_frame(capture, false);
        capture->fid = fid;
        size_t payload = length < capture->payload ? length : capture->payload;
        size_t data = payload - header;
        if ((flags & 0x40) != 0 || data > FRAME_BYTES - capture->fill) {
            capture->poisoned = true;
        } else if (!capture->poisoned) {
            memcpy(capture->frame + capture->fill, buffer + header, data);
            capture->fill += (uint32_t)data;
        }
        if ((flags & 2) != 0) {
            end_frame(capture, true);
            capture->fid = -1;
        }
        buffer += payload;
        length -= payload;
    }
}
static void LIBUSB_CALL transfer_callback(struct libusb_transfer *transfer)
{
    struct capture *capture = transfer->user_data;
    int index = 0;
    while (index < TRANSFERS && capture->transfers[index] != transfer)
        ++index;
    if (index == TRANSFERS)
        return;
    capture->pending[index] = false;
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        if (transfer->actual_length < 0 || transfer->actual_length > transfer->length) {
            capture->failed = capture->finished = true;
        } else {
            parse_transfer(capture, transfer->buffer, (size_t)transfer->actual_length);
        }
    } else if (transfer->status != LIBUSB_TRANSFER_CANCELLED) {
        log_message("capture device unavailable or ambiguous\n");
        capture->failed = capture->finished = true;
    }
    if (!capture->finished && !interrupted) {
        int result = libusb_submit_transfer(transfer);
        if (result == 0)
            capture->pending[index] = true;
        else
            capture->failed = capture->finished = true;
    }
}
static bool control(struct capture *capture, bool set, uint8_t selector, uint8_t *probe)
{
    return libusb_control_transfer(capture->handle, set ? 0x21 : 0xa1, set ? 1 : 0x81,
                                   (uint16_t)selector << 8, capture->mode.stream, probe, 26,
                                   1000) == 26;
}
static bool negotiate(struct capture *capture)
{
    uint8_t probe[26] = {1, 0};
    probe[2] = capture->mode.format;
    probe[3] = capture->mode.frame;
    put32(probe + 4, INTERVAL);
    if (libusb_set_interface_alt_setting(capture->handle, capture->mode.stream, 0) != 0 ||
        !control(capture, true, 1, probe) || !control(capture, false, 1, probe))
        return false;
    uint32_t payload = le32(probe + 22);
    if (probe[2] != capture->mode.format || probe[3] != capture->mode.frame ||
        le32(probe + 4) != INTERVAL || le32(probe + 18) != FRAME_BYTES || payload < 12 ||
        payload > MAX_PAYLOAD || !control(capture, true, 2, probe))
        return false;
    /* Some devices do not implement GET_CUR(COMMIT); PROBE response is authoritative. */
    capture->payload = payload;
    log_message("negotiated native P010 1080p60\n");
    return true;
}
static bool pending(const struct capture *capture)
{
    for (int i = 0; i < TRANSFERS; ++i)
        if (capture->pending[i])
            return true;
    return false;
}
struct driver_ops {
    int (*attach)(libusb_device_handle *, int);
    int (*active)(libusb_device_handle *, int);
};
static bool restore_drivers(struct capture *capture, const struct driver_ops *ops)
{
    bool restored = true;
    for (int i = 1; i >= 0; --i) {
        uint8_t interface = i == 0 ? capture->mode.control : capture->mode.stream;
        if (capture->detached[i] && ops->attach(capture->handle, interface) != 0) {
            log_message("driver reattach failed; restoration unverified\n");
            restored = false;
        }
    }
    /* Darwin captures the whole device with a reference count: only the final
     * attach re-enumerates it (libusb v1.0.29 darwin_usb.c:2821-2885).
     * UVC clients may attach asynchronously after USB re-enumeration completes. */
    uint64_t deadline = now_us() + 2000000U;
    for (;;) {
        bool active = true;
        for (int i = 0; i < 2; ++i) {
            uint8_t interface = i == 0 ? capture->mode.control : capture->mode.stream;
            if (capture->detached[i]) {
                int result = ops->active(capture->handle, interface);
                if (result < 0) {
                    log_message("restored interface unavailable\n");
                    return false;
                }
                if (result != 1)
                    active = false;
            }
        }
        if (active)
            return restored;
        if (now_us() >= deadline) {
            log_message("driver reattach verification deadline exceeded\n");
            return false;
        }
        struct timespec delay = {.tv_nsec = 20000000};
        if (nanosleep(&delay, NULL) != 0 && errno != EINTR)
            return false;
    }
}
static bool restore(struct capture *capture)
{
    capture->finished = true;
    bool restored = true;
    for (int i = 0; i < TRANSFERS; ++i) {
        if (capture->pending[i]) {
            int result = libusb_cancel_transfer(capture->transfers[i]);
            if (result != 0 && result != LIBUSB_ERROR_NOT_FOUND) {
                restored = false;
                log_message("USB cancellation request failed; restoration unverified\n");
            }
        }
    }
    /* libusb owns submitted memory until the cancellation callback runs. */
    while (pending(capture)) {
        struct timeval timeout = {0, 100000};
        int result = libusb_handle_events_timeout_completed(capture->context, &timeout, NULL);
        if (result != 0 && result != LIBUSB_ERROR_INTERRUPTED)
            log_message("USB operation failed\n");
    }
    for (int i = 0; i < TRANSFERS; ++i) {
        if (capture->transfers[i] != NULL) {
            free(capture->transfers[i]->buffer);
            libusb_free_transfer(capture->transfers[i]);
        }
    }
    if (capture->handle != NULL) {
        if (capture->claimed[1]) {
            int result = libusb_clear_halt(capture->handle, capture->mode.endpoint);
            if (result != 0) {
                log_message("endpoint stop failed\n");
                restored = false;
            }
            if (libusb_set_interface_alt_setting(capture->handle, capture->mode.stream, 0) != 0) {
                log_message("stream alternate-setting restore failed\n");
                restored = false;
            }
        }
        for (int i = 1; i >= 0; --i) {
            uint8_t interface = i == 0 ? capture->mode.control : capture->mode.stream;
            if (capture->claimed[i] && libusb_release_interface(capture->handle, interface) != 0) {
                log_message("interface release failed\n");
                restored = false;
            }
        }
        const struct driver_ops ops = {.attach = libusb_attach_kernel_driver,
                                       .active = libusb_kernel_driver_active};
        if (!restore_drivers(capture, &ops))
            restored = false;
        libusb_close(capture->handle);
    }
    free(capture->frame);
    return restored;
}
static int run(uint32_t count, const char *clock_id, bool enumerate)
{
    struct capture capture = {.requested = count, .fid = -1};
    int result = libusb_init(&capture.context);
    if (result != 0)
        return 1;
    libusb_device **devices = NULL;
    ssize_t total = libusb_get_device_list(capture.context, &devices);
    libusb_device *selected = NULL;
    for (ssize_t i = 0; i < total; ++i) {
        struct libusb_device_descriptor descriptor;
        struct mode mode = {0};
        if (libusb_get_device_descriptor(devices[i], &descriptor) == 0 &&
            descriptor.idVendor == 0x0fd9 && descriptor.idProduct == 0x009c &&
            describe(devices[i], &mode)) {
            if (selected != NULL) {
                log_message("capture device unavailable or ambiguous\n");
                selected = NULL;
                break;
            }
            selected = devices[i];
            capture.mode = mode;
        }
    }
    bool success = false, restored = true;
    if (selected == NULL) {
        log_message("capture device unavailable or ambiguous\n");
        goto cleanup;
    }
    log_message("descriptor-selected P010 1080p60 device\n");
    if (enumerate) {
        success = true;
        goto cleanup;
    }
    if (libusb_open(selected, &capture.handle) != 0)
        goto cleanup;
    for (int i = 0; i < 2; ++i) {
        uint8_t interface = i == 0 ? capture.mode.control : capture.mode.stream;
        int active = libusb_kernel_driver_active(capture.handle, interface);
        if (active < 0)
            goto cleanup;
        if (active == 1) {
            /* Record the obligation BEFORE detach: macOS capture detaches the entire device. */
            capture.detached[i] = true;
            if (libusb_detach_kernel_driver(capture.handle, interface) != 0)
                goto cleanup;
        }
        if (libusb_claim_interface(capture.handle, interface) != 0)
            goto cleanup;
        capture.claimed[i] = true;
    }
    if (!negotiate(&capture)) {
        log_message("required native mode rejected; no fallback\n");
        goto cleanup;
    }
    capture.frame = malloc(FRAME_BYTES);
    if (capture.frame == NULL)
        goto cleanup;
    uint8_t header[128] = {0};
    memcpy(header, "SP5H", 4);
    put32(header + 4, 1);
    put32(header + 8, FRAME_BYTES);
    put32(header + 12, INTERVAL);
    put32(header + 16, 1920);
    put32(header + 20, 1080);
    memcpy(header + 24, "P010", 4);
    memcpy(header + 32, clock_id, strlen(clock_id));
    if (!write_all(header, sizeof(header)))
        goto cleanup;
    capture.deadline = now_us() + (uint64_t)count * INTERVAL / 10U + 5000000U;
    uint64_t idle = now_us();
    for (int i = 0; i < TRANSFERS; ++i) {
        capture.transfers[i] = libusb_alloc_transfer(0);
        if (capture.transfers[i] == NULL)
            goto cleanup;
        int size = (int)capture.payload * 64;
        uint8_t *buffer = malloc((size_t)size);
        if (buffer == NULL)
            goto cleanup;
        libusb_fill_bulk_transfer(capture.transfers[i], capture.handle, capture.mode.endpoint,
                                  buffer, size, transfer_callback, &capture, 1000);
        if (libusb_submit_transfer(capture.transfers[i]) != 0)
            goto cleanup;
        capture.pending[i] = true;
    }
    while (!capture.finished && !interrupted && now_us() < capture.deadline) {
        struct pollfd control_fd = {.fd = STDIN_FILENO, .events = POLLIN};
        if (poll(&control_fd, 1, 0) > 0 &&
            (control_fd.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            capture.failed = true;
            break;
        }
        struct timeval timeout = {0, 50000};
        result = libusb_handle_events_timeout_completed(capture.context, &timeout, NULL);
        if (result != 0 && result != LIBUSB_ERROR_INTERRUPTED) {
            capture.failed = true;
            break;
        }
        if (now_us() - (capture.last_frame != 0 ? capture.last_frame : idle) > 5000000U) {
            capture.failed = true;
            break;
        }
    }
    success = !capture.failed && !interrupted && capture.emitted == count;
cleanup:
    if (devices != NULL)
        libusb_free_device_list(devices, 1);
    restored = restore(&capture);
    libusb_exit(capture.context);
    log_message(restored ? "ownership restored\n" : "ownership restoration UNVERIFIED\n");
    if (!enumerate && capture.emitted > 0) {
        interrupted = 0; /* Terminal status after safe cleanup, even on requested cancellation. */
        if (!record_header(&(struct frame_record){.magic = "DONE",
                                                  .stamp = now_us(),
                                                  .sequence = capture.sequence,
                                                  .bad = capture.bad,
                                                  .restored = restored ? 1U : 0U,
                                                  .reason = success ? 0U : 1U}))
            success = false;
    }
    return !restored ? 2 : success ? 0 : 1;
}
static bool parse_count(const char *text, uint32_t *count)
{
    size_t length = strnlen(text, 6);
    if (length == 0 || length > 5)
        return false;
    uint32_t value = 0;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9')
            return false;
        value = value * 10U + (uint32_t)(text[i] - '0');
    }
    if (value == 0 || value > 36000)
        return false;
    *count = value;
    return true;
}
static bool valid_clock_id(const char *text)
{
    size_t length = strnlen(text, 96);
    if (length == 0 || length >= 96)
        return false;
    for (size_t i = 0; i < length; ++i)
        if (!((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= '0' && text[i] <= '9') ||
              text[i] == '-'))
            return false;
    return true;
}
static int fake_capture_count;
static int fake_attach(libusb_device_handle *handle, int interface)
{
    (void)handle;
    (void)interface;
    --fake_capture_count;
    return 0;
}
static int fake_active(libusb_device_handle *handle, int interface)
{
    (void)handle;
    (void)interface;
    return fake_capture_count == 0 ? 1 : 0;
}
static int self_test(void)
{
    struct capture detached = {.detached = {true, true}};
    const struct driver_ops ops = {.attach = fake_attach, .active = fake_active};
    fake_capture_count = 2;
    if (!restore_drivers(&detached, &ops))
        return 1;
    uint32_t count = 0;
    if (!parse_count("1", &count) || count != 1 || !parse_count("36000", &count) ||
        count != 36000 || !valid_clock_id("machine-monotonic-0123456789abcdef"))
        return 1;
    const char *invalid[] = {
        "", "0", "+1", "-1", " 1", "1 ", "1.0", "36001", "184467440737095516160", "/dev/disk0"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        if (parse_count(invalid[i], &count))
            return 1;
    if (valid_clock_id("") || valid_clock_id("Upper") || valid_clock_id("../path") ||
        valid_clock_id("a b"))
        return 1;
    uint8_t descriptors[64] = {27, 0x24, 4, 7, 0, 'P', '0', '1', '0'};
    const uint8_t suffix[12] = {0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
    memcpy(descriptors + 9, suffix, sizeof(suffix));
    descriptors[21] = 24;
    descriptors[27] = 30;
    descriptors[28] = 0x24;
    descriptors[29] = 5;
    descriptors[30] = 5;
    descriptors[32] = 0x80;
    descriptors[33] = 7;
    descriptors[34] = 0x38;
    descriptors[35] = 4;
    descriptors[52] = 1;
    put32(descriptors + 53, INTERVAL);
    struct mode mode = {0};
    if (!scan_modes(descriptors, 57, &mode) || mode.format != 7 || mode.frame != 5)
        return 1;
    for (size_t size = 0; size < 57; ++size)
        if (scan_modes(descriptors, size, &mode))
            return 1;
    descriptors[27] = 3;
    if (scan_modes(descriptors, 57, &mode))
        return 1;
    struct capture capture = {.payload = 16, .fid = -1};
    uint8_t malformed[3] = {12, 0, 0};
    parse_transfer(&capture, malformed, sizeof(malformed));
    if (capture.bad != 1 || capture.fill != 0)
        return 1;
    uint8_t error[2] = {2, 0x42};
    parse_transfer(&capture, error, sizeof(error));
    if (capture.bad != 2)
        return 1;
    log_message(
        "synthetic descriptor truncation, mode selection, malformed/error payloads: pass\n");
    return 0;
}
int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
        return self_test();
    if (argc == 2 && strcmp(argv[1], "--enumerate") == 0)
        return run(0, "", true);
    if (argc != 5 || strcmp(argv[1], "--count") != 0 || strcmp(argv[3], "--clock-id") != 0) {
        log_message("capture device unavailable or ambiguous\n");
        return 64;
    }
    uint32_t count = 0;
    if (!parse_count(argv[2], &count) || !valid_clock_id(argv[4]))
        return 64;
    if (signal(SIGINT, on_signal) == SIG_ERR || signal(SIGTERM, on_signal) == SIG_ERR ||
        signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        return 1;
    int flags = fcntl(STDOUT_FILENO, F_GETFL);
    if (flags < 0 || fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK) < 0)
        return 1;
    return run((uint32_t)count, argv[4], false);
}
