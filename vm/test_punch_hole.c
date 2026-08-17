/*
 * test_punch_hole — Pre-compiled PUNCH_HOLE test binary for PowerFS VM tests
 *
 * Usage: test_punch_hole <file> [offset] [length]
 *   Default: offset=4096, length=4096
 *
 * Punches a hole in the file using FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
 * then fsyncs. Exit 0 on success, 1 on error.
 *
 * Compiled statically for BusyBox VM (no shared library dependency):
 *   gcc -static -o test_punch_hole test_punch_hole.c
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/falloc.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file> [offset] [length]\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    off_t offset = (argc > 2) ? strtoll(argv[2], NULL, 10) : 4096;
    off_t length = (argc > 3) ? strtoll(argv[3], NULL, 10) : 4096;

    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", path, strerror(errno));
        return 1;
    }

    int ret = fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, offset, length);
    if (ret < 0) {
        fprintf(stderr, "fallocate(PUNCH_HOLE, offset=%lld, len=%lld) failed: %s\n",
                (long long)offset, (long long)length, strerror(errno));
        close(fd);
        return 1;
    }

    if (fsync(fd) < 0) {
        fprintf(stderr, "fsync failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    close(fd);
    printf("PUNCH_OK: punched %lld bytes at offset %lld in %s\n",
           (long long)length, (long long)offset, path);
    return 0;
}
