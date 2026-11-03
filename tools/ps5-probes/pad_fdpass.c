/* Nonshipping diagnostic payload: can a virtual pad outlive its creator when
 * the /dev/hid descriptor is passed to another process with SCM_RIGHTS?
 *
 * -DPAD_FDPASS_CREATOR: register a pad, send the descriptor and handle on
 *                       AF_UNIX PAD_FDPASS_PATH, then exit.
 * default (adopter):    receive descriptor and handle, keep the pad alive
 *                       with neutral reports for 30 s after the creator is
 *                       gone, then remove it and exit.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef PAD_FDPASS_PATH
#define PAD_FDPASS_PATH "/data/ps5-dev/pad-fdpass.sock"
#endif
#define HID_ADD 0xc018482aUL
#define HID_REPORT 0x8018482cUL
#define HID_REMOVE 0x80104850UL

int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(int32_t *user_id);

typedef struct {
    uint32_t operation_or_handle;
    uint32_t reserved;
    void *input;
    void *output;
} pad_request;

typedef struct {
    uint8_t operation;
    uint8_t padding[7];
    uint32_t handle;
    uint32_t reserved;
} remove_request;

static __attribute__((unused)) int neutral_report(int fd, int32_t handle)
{
    const float orientation = 1.0f;
    uint8_t report[160] = {0};
    report[0x10] = report[0x11] = report[0x12] = report[0x13] = 0x80;
    report[0x17] = 1;
    memcpy(report + 0x84, &orientation, sizeof(orientation));
    pad_request request = {(uint32_t)handle, 0, report, NULL};
    return ioctl(fd, HID_REPORT, &request);
}

static __attribute__((unused)) void sleep_ms(long ms)
{
    struct timespec delay = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static struct sockaddr_un socket_address(void)
{
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    (void)strncpy(address.sun_path, PAD_FDPASS_PATH, sizeof(address.sun_path) - 1);
    return address;
}

#ifdef PAD_FDPASS_CREATOR
int main(void)
{
    int32_t user = -1;
    if (sceUserServiceInitialize(NULL) != 0 || sceUserServiceGetForegroundUser(&user) != 0)
        return 1;
    int fd = open("/dev/hid", O_RDWR);
    uint8_t descriptor[168] = {0};
    descriptor[0] = 3;
    for (unsigned i = 0; i < 4; ++i)
        descriptor[0x1c + i] = (uint8_t)((uint32_t)user >> (8U * i));
    descriptor[0x20] = 1;
    int32_t handle = -1;
    pad_request add = {3, 0, descriptor, &handle};
    int rc = ioctl(fd, HID_ADD, &add);
    printf("FDPASS creator pid=%ld fd=%d add=%d handle=%d\n", (long)getpid(), fd, rc, (int)handle);
    if (fd < 0 || rc != 0 || handle < 0)
        return 1;
    struct sockaddr_un address = socket_address();
    (void)unlink(PAD_FDPASS_PATH);
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    int socket_errno = listener < 0 ? errno : 0;
    int bound = listener < 0 ? -1 : bind(listener, (struct sockaddr *)&address, sizeof(address));
    int bind_errno = bound != 0 ? errno : 0;
    int listened = bound != 0 ? -1 : listen(listener, 1);
    if (listened != 0) {
        printf("FDPASS creator unix listen failed socket=%d/%d bind=%d/%d listen=%d/%d\n", listener,
               socket_errno, bound, bind_errno, listened, errno);
        return 1;
    }
    (void)fflush(stdout);
    int client = accept(listener, NULL, NULL);
    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof(control));
    struct iovec payload = {&handle, sizeof(handle)};
    struct msghdr message = {0};
    message.msg_iov = &payload;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &fd, sizeof(int));
    ssize_t sent = sendmsg(client, &message, 0);
    char ack = 0;
    ssize_t acked = read(client, &ack, 1);
    printf("FDPASS creator sent=%zd errno=%d ack=%zd exiting\n", sent, sent < 0 ? errno : 0, acked);
    (void)fflush(stdout);
    (void)close(client);
    (void)close(listener);
    (void)unlink(PAD_FDPASS_PATH);
    (void)close(fd);
    return 0;
}
#else
int main(void)
{
    struct sockaddr_un address = socket_address();
    int connection = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connection < 0 || connect(connection, (struct sockaddr *)&address, sizeof(address)) != 0) {
        printf("FDPASS adopter connect failed errno=%d\n", errno);
        return 1;
    }
    int32_t handle = -1;
    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof(control));
    struct iovec payload = {&handle, sizeof(handle)};
    struct msghdr message = {0};
    message.msg_iov = &payload;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    ssize_t received = recvmsg(connection, &message, 0);
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    int fd = -1;
    if (received == (ssize_t)sizeof(handle) && header != NULL && header->cmsg_type == SCM_RIGHTS)
        memcpy(&fd, CMSG_DATA(header), sizeof(int));
    printf("FDPASS adopter pid=%ld received=%zd fd=%d handle=%d\n", (long)getpid(), received, fd,
           (int)handle);
    (void)write(connection, "k", 1);
    (void)close(connection);
    if (fd < 0)
        return 1;
    sleep_ms(1000);
    int failures = 0;
    for (int i = 0; i < 600; ++i) {
        if (neutral_report(fd, handle) != 0)
            ++failures;
        if (i % 100 == 0) {
            printf("FDPASS adopter t=%ds report_failures=%d\n", i / 20, failures);
            (void)fflush(stdout);
        }
        sleep_ms(50);
    }
    remove_request remove = {.operation = 5, .handle = (uint32_t)handle};
    int rc = ioctl(fd, HID_REMOVE, &remove);
    printf("FDPASS adopter done report_failures=%d remove=%d\n", failures, rc);
    (void)close(fd);
    return 0;
}
#endif
