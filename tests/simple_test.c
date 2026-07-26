#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

#define TEST_DIR "/mnt/pfs/simple_test"
#define NUM_PROCS 4
#define FILES_PER_PROC 100

static int wait_for_child(pid_t pid, int timeout_ms) {
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        int status;
        pid_t ret = waitpid(pid, &status, WNOHANG);
        if (ret < 0) {
            perror("waitpid");
            return -1;
        }
        if (ret > 0) {
            if (WIFEXITED(status))
                return WEXITSTATUS(status);
            return -1;
        }
        usleep(100 * 1000); /* 100ms */
        elapsed += 100;
    }
    return -2; /* timeout */
}

static int run_concurrent_create(void) {
    int i, j;
    pid_t pids[NUM_PROCS];
    int errors = 0;

    printf("  [Concurrent create] %d procs x %d files...\n",
           NUM_PROCS, FILES_PER_PROC);

    for (i = 0; i < NUM_PROCS; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            char path[256];
            int fd;
            int local_err = 0;
            for (j = 0; j < FILES_PER_PROC; j++) {
                snprintf(path, sizeof(path), "%s/p%d_f%d", TEST_DIR, i, j);
                fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
                if (fd < 0) {
                    fprintf(stderr, "P%d: create %s failed: %s\n",
                            i, path, strerror(errno));
                    local_err = 1;
                    break;
                }
                if (write(fd, "data", 4) != 4) {
                    fprintf(stderr, "P%d: write %s failed: %s\n",
                            i, path, strerror(errno));
                    local_err = 1;
                    close(fd);
                    break;
                }
                close(fd);
            }
            _exit(local_err);
        }
    }

    for (i = 0; i < NUM_PROCS; i++) {
        int ret = wait_for_child(pids[i], 30000);
        if (ret == -2) {
            fprintf(stderr, "P%d: TIMEOUT (30s)\n", i);
            errors++;
            kill(pids[i], SIGKILL);
        } else if (ret < 0) {
            fprintf(stderr, "P%d: error %d\n", i, ret);
            errors++;
        } else if (ret != 0) {
            fprintf(stderr, "P%d: exit code %d\n", i, ret);
            errors++;
        }
    }

    printf("  Created %d files, errors: %d\n", NUM_PROCS * FILES_PER_PROC, errors);
    return errors;
}

static int run_nested_dirs(void) {
    char path[512];
    int k;
    int errors = 0;

    printf("  [Nested dirs] depth 5...\n");

    snprintf(path, sizeof(path), "%s/deep", TEST_DIR);
    if (mkdir(path, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir %s failed: %s\n", path, strerror(errno));
        return 1;
    }

    for (k = 0; k < 5; k++) {
        char name[64];
        snprintf(name, sizeof(name), "/lvl%d", k);
        strncat(path, name, sizeof(path) - strlen(path) - 1);
        if (mkdir(path, 0755) < 0 && errno != EEXIST) {
            fprintf(stderr, "mkdir %s failed: %s\n", path, strerror(errno));
            errors++;
            break;
        }
    }

    if (errors == 0) {
        char fpath[600];
        snprintf(fpath, sizeof(fpath), "%s/deep_file.txt", path);
        int fd = open(fpath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            fprintf(stderr, "open deep failed: %s\n", strerror(errno));
            errors++;
        } else {
            if (write(fd, "deep", 4) != 4) {
                fprintf(stderr, "write deep failed: %s\n", strerror(errno));
                errors++;
            }
            close(fd);

            fd = open(fpath, O_RDONLY);
            if (fd < 0) {
                fprintf(stderr, "open for read failed: %s\n", strerror(errno));
                errors++;
            } else {
                char buf[64];
                ssize_t n = read(fd, buf, sizeof(buf));
                close(fd);
                if (n != 4) {
                    fprintf(stderr, "read deep failed: %zd bytes (expected 4)\n", n);
                    errors++;
                }
            }
        }
    }

    /* cleanup */
    unlink("/mnt/pfs/simple_test/deep/lvl4/deep_file.txt");
    for (k = 4; k >= 0; k--) {
        char rpath[512];
        snprintf(rpath, sizeof(rpath), "%s/deep/lvl%d", TEST_DIR, k);
        rmdir(rpath);
    }
    rmdir("/mnt/pfs/simple_test/deep");

    printf("  Nested dirs, errors: %d\n", errors);
    return errors;
}

static int run_small_files(void) {
    int k;
    int fd;
    char path[256];
    int errors = 0;

    printf("  [Small files] 200 files...\n");

    for (k = 0; k < 200; k++) {
        snprintf(path, sizeof(path), "%s/small_%d", TEST_DIR, k);
        fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            fprintf(stderr, "create %s failed: %s\n", path, strerror(errno));
            errors++;
            break;
        }
        if (write(fd, "x", 1) != 1) {
            fprintf(stderr, "write %s failed: %s\n", path, strerror(errno));
            errors++;
            close(fd);
            break;
        }
        close(fd);
    }

    printf("  Created %d small files, errors: %d\n", k, errors);
    return errors;
}

static int run_concurrent_delete(void) {
    int i, j;
    pid_t pids[NUM_PROCS];
    int errors = 0;

    printf("  [Concurrent delete] %d procs x %d files...\n",
           NUM_PROCS, FILES_PER_PROC);

    for (i = 0; i < NUM_PROCS; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            char path[256];
            int local_err = 0;
            for (j = 0; j < FILES_PER_PROC; j++) {
                snprintf(path, sizeof(path), "%s/p%d_f%d", TEST_DIR, i, j);
                if (unlink(path) < 0 && errno != ENOENT) {
                    fprintf(stderr, "P%d: unlink %s failed: %s\n",
                            i, path, strerror(errno));
                    local_err = 1;
                }
            }
            _exit(local_err);
        }
    }

    for (i = 0; i < NUM_PROCS; i++) {
        int ret = wait_for_child(pids[i], 30000);
        if (ret == -2) {
            fprintf(stderr, "P%d: TIMEOUT (30s)\n", i);
            errors++;
            kill(pids[i], SIGKILL);
        } else if (ret < 0) {
            fprintf(stderr, "P%d: error %d\n", i, ret);
            errors++;
        } else if (ret != 0) {
            fprintf(stderr, "P%d: exit code %d\n", i, ret);
            errors++;
        }
    }

    printf("  Deleted, errors: %d\n", errors);
    return errors;
}

int main(int argc, char *argv[]) {
    int total_errors = 0;

    (void)argc;
    (void)argv;

    printf("=== PowerFS Simple Concurrent Test ===\n");
    printf("Config: %d procs x %d files each\n\n", NUM_PROCS, FILES_PER_PROC);

    /* Create test directory */
    printf("Creating test dir %s...\n", TEST_DIR);
    if (mkdir(TEST_DIR, 0755) < 0 && errno != EEXIST) {
        /* Try to remove and recreate */
        rmdir(TEST_DIR);
        if (mkdir(TEST_DIR, 0755) < 0) {
            fprintf(stderr, "mkdir failed: %s\n", strerror(errno));
            return 1;
        }
    }

    /* Test 1: concurrent create */
    printf("\n--- Test 1: Concurrent Create ---\n");
    total_errors += run_concurrent_create();

    /* Verify files exist */
    {
        int count = 0;
        int expected = NUM_PROCS * FILES_PER_PROC;
        for (int i = 0; i < NUM_PROCS; i++) {
            for (int j = 0; j < FILES_PER_PROC; j++) {
                char path[256];
                struct stat st;
                snprintf(path, sizeof(path), "%s/p%d_f%d", TEST_DIR, i, j);
                if (stat(path, &st) == 0)
                    count++;
            }
        }
        printf("  Verified %d/%d files exist\n", count, expected);
        if (count != expected) {
            fprintf(stderr, "  WARNING: %d files missing!\n", expected - count);
            total_errors += (expected - count);
        }
    }

    /* Test 2: nested dirs with read/write */
    printf("\n--- Test 2: Nested Dirs ---\n");
    total_errors += run_nested_dirs();

    /* Test 3: many small files */
    printf("\n--- Test 3: Small Files ---\n");
    total_errors += run_small_files();

    /* Test 4: concurrent delete */
    printf("\n--- Test 4: Concurrent Delete ---\n");
    total_errors += run_concurrent_delete();

    /* Cleanup test directory */
    printf("\nCleanup...\n");
    {
        DIR *d = opendir(TEST_DIR);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (strcmp(ent->d_name, ".") == 0 ||
                    strcmp(ent->d_name, "..") == 0)
                    continue;
                char fpath[512];
                snprintf(fpath, sizeof(fpath), "%s/%s", TEST_DIR, ent->d_name);
                if (unlink(fpath) < 0)
                    fprintf(stderr, "  cleanup: unlink %s failed: %s\n",
                            fpath, strerror(errno));
            }
            closedir(d);
        }
        rmdir(TEST_DIR);
    }

    printf("\n========================================\n");
    printf("Total errors: %d\n", total_errors);
    printf("Test %s\n", total_errors == 0 ? "PASSED" : "FAILED");
    printf("========================================\n");

    return total_errors;
}
