/*
 * test_fallocate_modes.c - Direct fallocate(2) syscall test for powerfs
 *
 * Tests all fallocate modes that BusyBox fallocate doesn't support:
 *   - FALLOC_FL_KEEP_SIZE
 *   - FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE
 *   - Default mode (extend)
 *
 * Build: gcc -static -o test_fallocate_modes test_fallocate_modes.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/falloc.h>

static int pass = 0, fail = 0;

static void ok(const char *msg) { printf("PASS: %s\n", msg); pass++; }
static void ng(const char *msg) { printf("FAIL: %s\n", msg); fail++; }

static int count_nonzero(const char *path, off_t off, size_t len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char *buf = malloc(len);
    if (!buf) { close(fd); return -1; }
    ssize_t n = pread(fd, buf, len, off);
    close(fd);
    if (n <= 0) { free(buf); return n; }
    int nz = 0;
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] != 0) nz++;
    }
    free(buf);
    return nz;
}

static int write_pattern(const char *path, const void *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    ssize_t n = write(fd, data, len);
    close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/mnt/pfs";
    char path[512];
    struct stat st;
    int fd, ret;

    /* Prepare known pattern: 16KB, byte value = offset%256 */
    size_t fsize = 16 * 1024;
    unsigned char *data = malloc(fsize);
    for (size_t i = 0; i < fsize; i++)
        data[i] = (unsigned char)(i % 256);

    /* === Test 1: Default mode (extend) === */
    snprintf(path, sizeof(path), "%s/falloc_t1", dir);
    write_pattern(path, data, fsize);
    fd = open(path, O_WRONLY);
    if (fd < 0) { ng("open t1 failed"); goto t2; }
    /* Extend by 8KB */
    ret = fallocate(fd, 0, fsize, 8192);
    if (ret < 0) {
        printf("FAIL: default extend: %s\n", strerror(errno));
        fail++;
    } else {
        fstat(fd, &st);
        if (st.st_size == (off_t)(fsize + 8192))
            ok("default extend size correct");
        else {
            printf("FAIL: default extend size=%ld expected=%zu\n",
                   (long)st.st_size, fsize + 8192);
            fail++;
        }
        /* Check extended region is zero */
        int nz = count_nonzero(path, fsize, 8192);
        if (nz == 0)
            ok("default extend region all zero");
        else {
            printf("FAIL: default extend has %d non-zero bytes\n", nz);
            fail++;
        }
        /* Check original data intact */
        int nz2 = count_nonzero(path, 0, fsize);
        /* original has all non-zero (i%256, but byte 0 is 0x00) */
        /* Actually byte at offset 0,256,512... is 0x00. Count non-zero. */
        /* Expected non-zero = fsize - fsize/256 = 16384 - 64 = 16320 */
        if (nz2 == 16320)
            ok("original data intact after extend");
        else {
            printf("FAIL: original data nz=%d expected=16320\n", nz2);
            fail++;
        }
    }
    close(fd);
    unlink(path);

t2:
    /* === Test 2: KEEP_SIZE mode === */
    snprintf(path, sizeof(path), "%s/falloc_t2", dir);
    write_pattern(path, data, fsize);
    fd = open(path, O_WRONLY);
    if (fd < 0) { ng("open t2 failed"); goto t3; }
    ret = fallocate(fd, FALLOC_FL_KEEP_SIZE, fsize, 8192);
    if (ret < 0) {
        printf("FAIL: KEEP_SIZE fallocate: %s\n", strerror(errno));
        fail++;
    } else {
        fstat(fd, &st);
        if (st.st_size == (off_t)fsize)
            ok("KEEP_SIZE size unchanged");
        else {
            printf("FAIL: KEEP_SIZE size=%ld expected=%zu\n",
                   (long)st.st_size, fsize);
            fail++;
        }
    }
    close(fd);
    unlink(path);

t3:
    /* === Test 3: PUNCH_HOLE mode === */
    snprintf(path, sizeof(path), "%s/falloc_t3", dir);
    write_pattern(path, data, fsize);
    /* sync to ensure data is on server */
    fd = open(path, O_WRONLY);
    if (fd < 0) { ng("open t3 failed"); goto t4; }
    fsync(fd);
    close(fd);

    fd = open(path, O_WRONLY);
    if (fd < 0) { ng("open t3 rw failed"); goto t4; }
    /* Punch hole [2048, 4096) */
    ret = fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                    2048, 2048);
    if (ret < 0) {
        printf("FAIL: PUNCH_HOLE fallocate: %s\n", strerror(errno));
        fail++;
    } else {
        fstat(fd, &st);
        if (st.st_size == (off_t)fsize)
            ok("PUNCH_HOLE size unchanged");
        else {
            printf("FAIL: PUNCH_HOLE size=%ld expected=%zu\n",
                   (long)st.st_size, fsize);
            fail++;
        }

        /* Check punched region is zero */
        int nz = count_nonzero(path, 2048, 2048);
        if (nz == 0)
            ok("PUNCH_HOLE region all zero (immediate read)");
        else {
            printf("FAIL: PUNCH_HOLE region has %d non-zero bytes (immediate)\n", nz);
            fail++;
        }

        /* Sync and re-check */
        fsync(fd);
        close(fd);
        sync();
        usleep(500000);

        int nz2 = count_nonzero(path, 2048, 2048);
        if (nz2 == 0)
            ok("PUNCH_HOLE region all zero (after sync)");
        else {
            printf("FAIL: PUNCH_HOLE region has %d non-zero bytes (after sync)\n", nz2);
            fail++;
        }

        /* Check data before hole preserved */
        int nz3 = count_nonzero(path, 0, 2048);
        /* [0,2048): bytes 0,256,512,768,1024,1280,1536,1792 are 0x00 = 8 zeros */
        if (nz3 == 2048 - 8)
            ok("data before hole preserved");
        else {
            printf("FAIL: data before hole nz=%d expected=%d\n", nz3, 2048 - 8);
            fail++;
        }

        /* Check data after hole preserved */
        int nz4 = count_nonzero(path, 4096, fsize - 4096);
        if (nz4 == (int)(fsize - 4096) - (int)((fsize - 4096) / 256))
            ok("data after hole preserved");
        else {
            printf("FAIL: data after hole nz=%d expected=%d\n",
                   nz4, (int)(fsize - 4096) - (int)((fsize - 4096) / 256));
            fail++;
        }
    }
    if (ret >= 0) close(fd);
    unlink(path);

t4:
    /* === Test 4: PUNCH_HOLE + re-read from server (invalidate cache) === */
    snprintf(path, sizeof(path), "%s/falloc_t4", dir);
    write_pattern(path, data, fsize);
    fd = open(path, O_WRONLY);
    if (fd < 0) { ng("open t4 failed"); goto done; }
    fsync(fd);
    close(fd);

    /* Read once to populate pagecache */
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        char tmp[4096];
        pread(fd, tmp, sizeof(tmp), 0);
        close(fd);
    }

    /* Punch hole */
    fd = open(path, O_WRONLY);
    if (fd < 0) { ng("open t4 w failed"); goto done; }
    ret = fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                    4096, 4096);
    if (ret < 0) {
        printf("FAIL: PUNCH_HOLE t4: %s\n", strerror(errno));
        fail++;
        close(fd);
        goto done;
    }
    close(fd);

    /* posix_fadvise to drop pagecache, forcing re-read from server */
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        /* Re-read punched region from server */
        int nz = count_nonzero(path, 4096, 4096);
        if (nz == 0)
            ok("PUNCH_HOLE region zero after cache drop (server zeroed)");
        else {
            printf("FAIL: PUNCH_HOLE region has %d non-zero after cache drop (server NOT zeroed)\n", nz);
            fail++;
        }
        close(fd);
    }

done:
    unlink(path);
    free(data);

    printf("\n=========================================\n");
    printf("  Results: PASS=%d FAIL=%d\n", pass, fail);
    printf("=========================================\n");
    return (fail == 0) ? 0 : 1;
}
