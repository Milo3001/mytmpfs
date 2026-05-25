/*
 * Simple userspace tester to write/read page-aligned patterns to a file.
 * Usage:
 *   test_swap write <path> <pages>
 *   test_swap read  <path> <pages>
 *
 * It writes `pages` pages sized by `getpagesize()` filling page i with byte (i % 256).
 * It prints a 32-bit checksum to stdout (hex) after completing the write/read.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

static uint32_t update_crc(uint32_t crc, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    for (size_t i = 0; i < len; ++i)
        crc = crc * 31 + p[i];
    return crc;
}

int do_write(const char *path, size_t pages)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t *buf = malloc(pg);
    if (!buf) { perror("malloc"); close(fd); return 1; }
    uint32_t crc = 0;
    for (size_t i = 0; i < pages; ++i) {
        memset(buf, (uint8_t)(i & 0xff), pg);
        ssize_t w = write(fd, buf, pg);
        if (w != (ssize_t)pg) {
            fprintf(stderr, "write failed at page %zu: %s\n", i, strerror(errno));
            free(buf); close(fd); return 1;
        }
        crc = update_crc(crc, buf, pg);
    }
    if (fsync(fd) != 0) perror("fsync");
    free(buf);
    close(fd);
    printf("%08x\n", crc);
    return 0;
}

int do_read(const char *path, size_t pages)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t *buf = malloc(pg);
    if (!buf) { perror("malloc"); close(fd); return 1; }
    uint32_t crc = 0;
    for (size_t i = 0; i < pages; ++i) {
        ssize_t r = read(fd, buf, pg);
        if (r != (ssize_t)pg) {
            fprintf(stderr, "read failed at page %zu: %s\n", i, (r<0)?strerror(errno):"short read");
            free(buf); close(fd); return 1;
        }
        crc = update_crc(crc, buf, pg);
    }
    free(buf);
    close(fd);
    printf("%08x\n", crc);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <write|read> <path> <pages>\n", argv[0]);
        return 2;
    }
    const char *op = argv[1];
    const char *path = argv[2];
    long pages = strtol(argv[3], NULL, 10);
    if (pages <= 0) { fprintf(stderr, "invalid pages\n"); return 2; }
    if (strcmp(op, "write") == 0) return do_write(path, (size_t)pages);
    if (strcmp(op, "read") == 0) return do_read(path, (size_t)pages);
    fprintf(stderr, "unknown op '%s'\n", op);
    return 2;
}
