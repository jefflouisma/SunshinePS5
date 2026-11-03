/* Deterministic injected read error and concurrent fixture mutation. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int fault;
static const char *fixture;
static char replaced[512], moved[512], replacement_root[512];
static int close_error, consumed_fd = -1, consumed_attempts;
static int injected_close(int fd)
{
    if (fd == consumed_fd)
        ++consumed_attempts;
    struct stat actual, slash;
    if (close_error && !fstat(fd, &actual) && !stat("/", &slash) && actual.st_dev == slash.st_dev &&
        actual.st_ino == slash.st_ino) {
        close_error = 0;
        consumed_fd = fd;
        ++consumed_attempts;
        if (close(fd))
            return -1;
        errno = EIO;
        return -1;
    }
    return close(fd);
}
static ssize_t injected_read(int fd, void *buffer, size_t length)
{
    if (fault == 1) {
        errno = EIO;
        return -1;
    }
    ssize_t result = read(fd, buffer, length);
    if (fault == 2 && result > 0) {
        int writer = open(fixture, O_WRONLY | O_APPEND);
        if (writer < 0 || write(writer, "x", 1) != 1 || close(writer))
            return -1;
        fault = 0;
    }
    if (fault == 3 && result > 0) {
        if (rename(replaced, moved) || mkdir(replaced, 0700) ||
            (strcmp(replaced, replacement_root) && mkdir(replacement_root, 0700)))
            return -1;
        fault = 0;
    }
    return result;
}
#define read injected_read
#define close injected_close
#define main inspector_unused_main
#define RECOVERY_INSPECTOR_HOST
#include "recovery_inspector.c"
#undef main
#undef read
#undef close
static int descriptors(void)
{
    int count = 0;
    for (int fd = 0; fd < 1024; ++fd)
        if (fcntl(fd, F_GETFD) >= 0)
            ++count;
    return count;
}
int main(int argc, char **argv)
{
    if (argc != 3)
        return 64;
    fixture = argv[2];
    int baseline = descriptors();
    for (int i = 0; i < 100; ++i) {
        fault = 1;
        struct inspection observation = {0};
        if (collect(argv[1], &observation) != -1)
            return 1;
        if (descriptors() != baseline)
            return 2;
    }
    fault = 2;
    struct inspection observation = {0};
    if (collect(argv[1], &observation) != -1)
        return 3;
    if (descriptors() != baseline)
        return 4;
    close_error = 1;
    if (inspect(argv[1]) != 1 || close_error || consumed_attempts != 1 || descriptors() != baseline)
        return 5;
    consumed_fd = -1;
    for (int ancestor = 0; ancestor < 2; ++ancestor) {
        char parent[512], file[600], moved_file[600], moved_root[600];
        snprintf(parent, sizeof(parent), "%s/race-parent", argv[1]);
        snprintf(replacement_root, sizeof(replacement_root), "%s/race-parent/root", argv[1]);
        snprintf(replaced, sizeof(replaced), "%s", ancestor ? parent : replacement_root);
        snprintf(moved, sizeof(moved), "%s/race-moved", argv[1]);
        snprintf(file, sizeof(file), "%s/file", replacement_root);
        snprintf(moved_root, sizeof(moved_root), "%s%s", moved, ancestor ? "/root" : "");
        snprintf(moved_file, sizeof(moved_file), "%s/file", moved_root);
        if (mkdir(parent, 0700) || mkdir(replacement_root, 0700))
            return 6;
        int fd = open(file, O_CREAT | O_WRONLY, 0600);
        if (fd < 0 || write(fd, "fixture", 7) != 7 || close(fd))
            return 7;
        fault = 3;
        if (inspect(replacement_root) != 1 || fault || descriptors() != baseline)
            return 8;
        if (unlink(moved_file) || rmdir(moved_root) || (ancestor && rmdir(moved)) ||
            rmdir(replacement_root) || rmdir(parent))
            return 9;
    }
    puts("PASS: 100 EIO refusals, append-during-read refusal, ancestor/root replacement refusal, "
         "consumed ancestor close EIO refusal without retry, descriptor counts unchanged");
    return 0;
}
