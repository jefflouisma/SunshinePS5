/* NONSHIPPING staging helper for the guarded foreground-app transaction when
 * no FTP service is running. Line commands on TCP 9042 (one client at a time):
 *
 *   MKDIR <path>               create one directory (fails if it exists)
 *   PUT <path> <bytes> <mode>  then <bytes> raw bytes; create a new file only
 *   COPY <src> <dst> <mode>    copy a console file to a new file only
 *   RM <path> | RMDIR <path>   remove one file / one empty directory
 *   QUIT                       exit
 *
 * Every create uses O_EXCL or mkdir, so nothing existing is ever overwritten.
 * Replies "OK" or "ERR <errno>". Exits by itself after ten minutes.
 */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STAGE_PORT 9042
#define STAGE_SECONDS 600
#define STAGE_MAX_BYTES (16 * 1024 * 1024)

static int read_line(int fd, char *line, size_t capacity)
{
    size_t used = 0;
    while (used + 1 < capacity) {
        char c;
        ssize_t count = read(fd, &c, 1);
        if (count <= 0)
            return -1;
        if (c == '\n')
            break;
        line[used++] = c;
    }
    line[used] = 0;
    return (int)used;
}

static int read_exact(int fd, uint8_t *data, size_t size)
{
    size_t used = 0;
    while (used < size) {
        ssize_t count = read(fd, data + used, size - used);
        if (count <= 0)
            return -1;
        used += (size_t)count;
    }
    return 0;
}

static int write_new(const char *path, const uint8_t *data, size_t size, unsigned mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, (mode_t)mode);
    if (fd < 0)
        return -1;
    size_t used = 0;
    while (used < size) {
        ssize_t count = write(fd, data + used, size - used);
        if (count <= 0) {
            int saved = errno;
            (void)close(fd);
            (void)unlink(path);
            errno = saved;
            return -1;
        }
        used += (size_t)count;
    }
    if (fchmod(fd, (mode_t)mode) != 0 || close(fd) != 0)
        return -1;
    return 0;
}

static uint8_t *read_file(const char *path, size_t *size)
{
    int fd = open(path, O_RDONLY);
    struct stat info;
    if (fd < 0 || fstat(fd, &info) != 0 || info.st_size < 0 || info.st_size > STAGE_MAX_BYTES) {
        if (fd >= 0)
            (void)close(fd);
        return NULL;
    }
    uint8_t *data = malloc((size_t)info.st_size + 1);
    if (data == NULL || read_exact(fd, data, (size_t)info.st_size) != 0) {
        free(data);
        (void)close(fd);
        return NULL;
    }
    (void)close(fd);
    *size = (size_t)info.st_size;
    return data;
}

static void reply(int fd, int ok)
{
    char text[32];
    int size = ok == 0 ? snprintf(text, sizeof(text), "OK\n")
                       : snprintf(text, sizeof(text), "ERR %d\n", errno);
    (void)write(fd, text, (size_t)size);
}

int main(void)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    struct sockaddr_in address = {0};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(STAGE_PORT);
    if (listener < 0 || setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0 ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        printf("FS_STAGE listen failed errno=%d\n", errno);
        return 1;
    }
    printf("FS_STAGE ready port=%d\n", STAGE_PORT);
    (void)fflush(stdout);
    time_t deadline = time(NULL) + STAGE_SECONDS;
    while (time(NULL) < deadline) {
        int client = accept(listener, NULL, NULL);
        if (client < 0)
            continue;
        char line[1024];
        while (read_line(client, line, sizeof(line)) >= 0) {
            char path[512], target[512];
            unsigned long size = 0;
            unsigned mode = 0;
            if (strncmp(line, "QUIT", 4) == 0) {
                reply(client, 0);
                (void)close(client);
                (void)close(listener);
                printf("FS_STAGE quit\n");
                return 0;
            } else if (sscanf(line, "MKDIR %511s", path) == 1) {
                reply(client, mkdir(path, 0755));
            } else if (sscanf(line, "PUT %511s %lu %o", path, &size, &mode) == 3 &&
                       size <= STAGE_MAX_BYTES) {
                uint8_t *data = malloc(size + 1);
                int ok = data != NULL && read_exact(client, data, size) == 0
                             ? write_new(path, data, size, mode)
                             : -1;
                free(data);
                reply(client, ok);
            } else if (sscanf(line, "COPY %511s %511s %o", path, target, &mode) == 3) {
                size_t bytes = 0;
                uint8_t *data = read_file(path, &bytes);
                int ok = data != NULL ? write_new(target, data, bytes, mode) : -1;
                free(data);
                reply(client, ok);
            } else if (sscanf(line, "RMDIR %511s", path) == 1) {
                reply(client, rmdir(path));
            } else if (sscanf(line, "RM %511s", path) == 1) {
                reply(client, unlink(path));
            } else {
                errno = EINVAL;
                reply(client, -1);
            }
            printf("FS_STAGE %s\n", line);
            (void)fflush(stdout);
        }
        (void)close(client);
    }
    (void)close(listener);
    return 0;
}
