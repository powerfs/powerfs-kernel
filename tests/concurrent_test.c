/*
 * PowerFS 并发压力测试程序
 *
 * 测试目标:
 *   1. 多进程并发创建/删除文件
 *   2. 多进程并发读写操作
 *   3. 多进程并发重命名操作
 *   4. 深层目录嵌套测试
 *   5. 大量小文件操作
 *   6. 混合操作压力测试
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <signal.h>

#define MAX_PROCESSES 4
#define FILES_PER_PROCESS 50
#define TEST_DIR "/mnt/pfs/stress_test"
#define DIRS_PER_LEVEL 10
#define MAX_DEPTH 5

static volatile int test_failed = 0;
static int test_passed = 0;
static int test_total = 0;

#define TEST_ASSERT(cond, msg) do { \
    test_total++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        test_failed = 1; \
    } else { \
        test_passed++; \
    } \
} while(0)

/* 记录开始时间用于性能统计 */
static struct timespec start_time;

void get_time(struct timespec *t) {
    clock_gettime(CLOCK_MONOTONIC, t);
}

double elapsed_ms(struct timespec *start) {
    struct timespec end;
    get_time(&end);
    return (end.tv_sec - start->tv_sec) * 1000.0 +
           (end.tv_nsec - start->tv_nsec) / 1000000.0;
}

/*
 * 测试1: 并发创建文件
 * 多个进程同时创建文件
 */
int test_concurrent_create(int num_processes) {
    int i, j;
    pid_t pids[MAX_PROCESSES];
    int errors = 0;

    printf("\n=== 测试1: 并发创建文件 (%d 进程 × %d 文件) ===\n",
           num_processes, FILES_PER_PROCESS);

    get_time(&start_time);

    for (i = 0; i < num_processes; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            /* 子进程: 创建文件 */
            char path[256];
            char data[1024];
            int fd;

            snprintf(data, sizeof(data), "data from process %d\n", i);

            for (j = 0; j < FILES_PER_PROCESS; j++) {
                snprintf(path, sizeof(path), "%s/proc%d_file%d",
                        TEST_DIR, i, j);

                fd = open(path, O_CREAT | O_WRONLY, 0644);
                if (fd < 0) {
                    fprintf(stderr, "Proc %d: 无法创建文件 %s: %s\n",
                            i, path, strerror(errno));
                    exit(1);
                }

                if (write(fd, data, strlen(data)) != (ssize_t)strlen(data)) {
                    fprintf(stderr, "Proc %d: 写入失败\n", i);
                    close(fd);
                    exit(1);
                }

                close(fd);
            }
            exit(0);
        }
    }

    /* 等待所有子进程完成 */
    for (i = 0; i < num_processes; i++) {
        int status;
        if (waitpid(pids[i], &status, 0) < 0) {
            perror("waitpid");
            errors++;
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            fprintf(stderr, "进程 %d 退出码: %d\n", i, WEXITSTATUS(status));
            errors++;
        }
    }

    double elapsed = elapsed_ms(&start_time);
    int total_files = num_processes * FILES_PER_PROCESS;

    printf("创建 %d 个文件耗时: %.2f ms (%.2f files/ms)\n",
           total_files, elapsed, total_files / elapsed);

    TEST_ASSERT(errors == 0, "并发创建文件");

    /* 验证文件存在 */
    for (i = 0; i < num_processes; i++) {
        for (j = 0; j < FILES_PER_PROCESS; j++) {
            char path[256];
            struct stat st;

            snprintf(path, sizeof(path), "%s/proc%d_file%d",
                    TEST_DIR, i, j);

            if (stat(path, &st) < 0) {
                fprintf(stderr, "文件缺失: %s\n", path);
                errors++;
            }
        }
    }

    TEST_ASSERT(errors == 0, "验证文件存在");

    /* 清理 */
    for (i = 0; i < num_processes; i++) {
        for (j = 0; j < FILES_PER_PROCESS; j++) {
            char path[256];
            snprintf(path, sizeof(path), "%s/proc%d_file%d",
                    TEST_DIR, i, j);
            unlink(path);
        }
    }

    return errors;
}

/*
 * 测试2: 并发读写操作
 * 多个进程同时读写共享文件
 */
int test_concurrent_readwrite(int num_processes) {
    int i;
    pid_t pids[MAX_PROCESSES];
    int errors = 0;
    const char *shared_file = TEST_DIR "/shared_data";

    printf("\n=== 测试2: 并发读写操作 (%d 进程) ===\n", num_processes);

    /* 创建共享文件 */
    {
        int fd = open(shared_file, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) {
            char init_data[1024];
            memset(init_data, 'A', sizeof(init_data));
            if (write(fd, init_data, sizeof(init_data)) < 0) {
                fprintf(stderr, "写入初始数据失败: %s\n", strerror(errno));
            }
            close(fd);
        }
    }

    get_time(&start_time);

    for (i = 0; i < num_processes; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            /* 一半进程写，一半进程读 */
            if (i % 2 == 0) {
                /* 写入进程 */
                int fd;
                char wdata[1024];
                int j;

                memset(wdata, 'B' + (i % 24), sizeof(wdata));

                for (j = 0; j < 50; j++) {
                    fd = open(shared_file, O_WRONLY);
                    if (fd < 0) {
                        exit(1);
                    }

                    lseek(fd, 0, SEEK_END);
                    if (write(fd, wdata, sizeof(wdata)) != sizeof(wdata)) {
                        close(fd);
                        exit(1);
                    }
                    close(fd);
                }
            } else {
                /* 读取进程 */
                int fd;
                char rdata[1024];
                int j;

                for (j = 0; j < 50; j++) {
                    fd = open(shared_file, O_RDONLY);
                    if (fd < 0) {
                        exit(1);
                    }

                    lseek(fd, (off_t)j * sizeof(rdata), SEEK_SET);
                    if (read(fd, rdata, sizeof(rdata)) < 0) {
                        close(fd);
                        exit(1);
                    }
                    close(fd);
                }
            }
            exit(0);
        }
    }

    /* 等待所有子进程完成 */
    for (i = 0; i < num_processes; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            errors++;
        }
    }

    double elapsed = elapsed_ms(&start_time);
    printf("并发读写耗时: %.2f ms\n", elapsed);

    TEST_ASSERT(errors == 0, "并发读写操作");

    unlink(shared_file);
    return errors;
}

/*
 * 测试3: 并发删除文件
 */
int test_concurrent_delete(int num_processes) {
    int i, j;
    pid_t pids[MAX_PROCESSES];
    int errors = 0;
    const int files_per_proc = 50;

    printf("\n=== 测试3: 并发删除文件 (%d 进程 × %d 文件) ===\n",
           num_processes, files_per_proc);

    /* 先创建所有文件 */
    for (i = 0; i < num_processes; i++) {
        for (j = 0; j < files_per_proc; j++) {
            char path[256];
            int fd;

            snprintf(path, sizeof(path), "%s/del_test_%d_%d",
                    TEST_DIR, i, j);
            fd = open(path, O_CREAT | O_WRONLY, 0644);
            if (fd >= 0) {
                close(fd);
            }
        }
    }

    get_time(&start_time);

    /* 并发删除 */
    for (i = 0; i < num_processes; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            int j;
            for (j = 0; j < files_per_proc; j++) {
                char path[256];
                snprintf(path, sizeof(path), "%s/del_test_%d_%d",
                        TEST_DIR, i, j);

                /* 每个进程删除自己的文件，加上一些其他进程的文件 */
                if (j >= files_per_proc / 2 && (i + 1) < num_processes) {
                    /* 删除下一个进程的文件 */
                    int target_proc = (i + 1) % num_processes;
                    int target_j = j - files_per_proc / 2;
                    snprintf(path, sizeof(path), "%s/del_test_%d_%d",
                            TEST_DIR, target_proc, target_j);
                }

                if (unlink(path) < 0 && errno != ENOENT) {
                    fprintf(stderr, "Proc %d: 删除 %s 失败: %s\n",
                            i, path, strerror(errno));
                    exit(1);
                }
            }
            exit(0);
        }
    }

    /* 等待所有子进程完成 */
    for (i = 0; i < num_processes; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            errors++;
        }
    }

    double elapsed = elapsed_ms(&start_time);
    printf("并发删除耗时: %.2f ms\n", elapsed);

    TEST_ASSERT(errors == 0, "并发删除文件");
    return errors;
}

/*
 * 测试4: 深层目录嵌套测试
 */
int test_deep_directory(void) {
    int i;
    char path[1024];
    int errors = 0;

    printf("\n=== 测试4: 深层目录嵌套测试 (深度 %d) ===\n", MAX_DEPTH);

    /* 创建深层目录结构 */
    path[0] = '\0';
    strcat(path, TEST_DIR);
    strcat(path, "/deep_test");

    /* 先创建 deep_test 目录 */
    if (mkdir(path, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "创建目录 %s 失败: %s\n", path, strerror(errno));
        errors++;
    }

    /* 在 deep_test 下创建层级子目录 */
    for (i = 0; i < MAX_DEPTH; i++) {
        char name[64];
        snprintf(name, sizeof(name), "/level_%d", i);
        strncat(path, name, sizeof(path) - strlen(path) - 1);

        if (mkdir(path, 0755) < 0 && errno != EEXIST) {
            fprintf(stderr, "创建目录 %s 失败: %s\n", path, strerror(errno));
            errors++;
            break;
        }
    }

    TEST_ASSERT(errors == 0, "创建深层目录");

    /* 在最深层创建文件 */
    if (errors == 0) {
        char filepath[1024];
        int fd;
        const char *data = "deep file content";

        if (snprintf(filepath, sizeof(filepath), "%s/deep_file.txt", path)
            >= (int)sizeof(filepath)) {
            fprintf(stderr, "路径过长，截断\n");
            errors++;
        }
        fd = open(filepath, O_CREAT | O_WRONLY, 0644);
        if (fd < 0) {
            fprintf(stderr, "创建深层文件失败: %s\n", strerror(errno));
            errors++;
        } else {
            if (write(fd, data, strlen(data)) < 0) {
                fprintf(stderr, "写入深层文件失败: %s\n", strerror(errno));
                errors++;
            }
            close(fd);

            /* 读取验证 */
            fd = open(filepath, O_RDONLY);
            if (fd < 0) {
                errors++;
            } else {
                char rbuf[256];
                ssize_t n = read(fd, rbuf, sizeof(rbuf) - 1);
                close(fd);
                if (n != (ssize_t)strlen(data)) {
                    fprintf(stderr, "深层文件读取内容错误\n");
                    errors++;
                }
            }
        }

        TEST_ASSERT(errors == 0, "深层文件读写");
    }

    /* 清理深层目录 (从最深层开始) */
    if (errors == 0) {
        char filepath[1024];
        if (snprintf(filepath, sizeof(filepath), "%s/deep_file.txt", path)
            >= (int)sizeof(filepath)) {
            fprintf(stderr, "路径过长，截断\n");
        }
        unlink(filepath);

        /* rmdir 需要从最深层开始 */
        path[0] = '\0';
        strcat(path, TEST_DIR);
        strcat(path, "/deep_test");

        for (i = MAX_DEPTH - 1; i >= 0; i--) {
            char name[64];
            snprintf(name, sizeof(name), "/level_%d", i);
            strncat(path, name, sizeof(path) - strlen(path) - 1);
            rmdir(path);
        }

        /* 最后删除最外层 */
        snprintf(path, sizeof(path), "%s/deep_test", TEST_DIR);
        rmdir(path);
    }

    return errors;
}

/*
 * 测试5: 大量小文件操作
 */
int test_many_small_files(void) {
    int i;
    char path[256];
    int errors = 0;
    const int num_files = 500;

    printf("\n=== 测试5: 大量小文件操作 (%d 文件) ===\n", num_files);

    get_time(&start_time);

    /* 创建小文件 */
    for (i = 0; i < num_files; i++) {
        int fd;
        char data[16];

        snprintf(path, sizeof(path), "%s/small_%04d.txt", TEST_DIR, i);
        snprintf(data, sizeof(data), "file_%d", i);

        fd = open(path, O_CREAT | O_WRONLY, 0644);
        if (fd < 0) {
            fprintf(stderr, "创建文件 %s 失败: %s\n", path, strerror(errno));
            errors++;
            continue;
        }

        if (write(fd, data, strlen(data)) < 0) {
            fprintf(stderr, "写入文件 %s 失败: %s\n", path, strerror(errno));
            errors++;
        }
        close(fd);
    }

    double elapsed = elapsed_ms(&start_time);
    printf("创建 %d 个小文件耗时: %.2f ms\n", num_files, elapsed);

    TEST_ASSERT(errors == 0, "创建小文件");

    /* 读取并验证 */
    get_time(&start_time);
    for (i = 0; i < num_files; i++) {
        int fd;
        char rbuf[16];
        char expected[16];

        snprintf(path, sizeof(path), "%s/small_%04d.txt", TEST_DIR, i);
        snprintf(expected, sizeof(expected), "file_%d", i);

        fd = open(path, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "打开文件 %s 失败: %s\n", path, strerror(errno));
            errors++;
            continue;
        }

        if (read(fd, rbuf, sizeof(rbuf) - 1) < 0) {
            fprintf(stderr, "读取文件 %s 失败: %s\n", path, strerror(errno));
            errors++;
        }
        close(fd);
        rbuf[sizeof(rbuf) - 1] = '\0';

        if (strcmp(rbuf, expected) != 0) {
            fprintf(stderr, "文件 %s 内容错误: 期望 '%s', 实际 '%s'\n",
                    path, expected, rbuf);
            errors++;
        }
    }

    elapsed = elapsed_ms(&start_time);
    printf("读取 %d 个小文件耗时: %.2f ms\n", num_files, elapsed);

    TEST_ASSERT(errors == 0, "验证小文件内容");

    /* 删除所有文件 */
    for (i = 0; i < num_files; i++) {
        snprintf(path, sizeof(path), "%s/small_%04d.txt", TEST_DIR, i);
        unlink(path);
    }

    return errors;
}

/*
 * 测试6: 混合操作压力测试
 * 多进程同时执行创建/删除/读写操作
 */
int test_mixed_operations(int num_processes) {
    int i;
    pid_t pids[MAX_PROCESSES];
    int errors = 0;

    printf("\n=== 测试6: 混合操作压力测试 (%d 进程) ===\n", num_processes);

    get_time(&start_time);

    for (i = 0; i < num_processes; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            char path[256];
            int j;
            unsigned int seed = i * 1000;

            for (j = 0; j < 30; j++) {
                int op = rand_r(&seed) % 4;

                switch (op) {
                    case 0: /* 创建文件 */
                        snprintf(path, sizeof(path),
                                "%s/mixed_proc%d_%d", TEST_DIR, i, j);
                        {
                            int fd = open(path, O_CREAT | O_WRONLY, 0644);
                            if (fd >= 0) {
                                char data[64];
                                snprintf(data, sizeof(data),
                                        "mixed data %d-%d", i, j);
                                if (write(fd, data, strlen(data)) < 0)
                                    errors++;
                                close(fd);
                            }
                        }
                        break;

                    case 1: /* 删除文件 */
                        snprintf(path, sizeof(path),
                                "%s/mixed_proc%d_%d", TEST_DIR, i, j - 1);
                        unlink(path);
                        break;

                    case 2: /* 读取文件 */
                        snprintf(path, sizeof(path),
                                "%s/mixed_proc%d_%d", TEST_DIR, i, j - 2);
                        {
                            int fd = open(path, O_RDONLY);
                            if (fd >= 0) {
                                char rbuf[64];
                                if (read(fd, rbuf, sizeof(rbuf)) < 0)
                                    errors++;
                                close(fd);
                            }
                        }
                        break;

                    case 3: /* 重命名 */
                        {
                            char old_path[256], new_path[256];
                            snprintf(old_path, sizeof(old_path),
                                    "%s/mixed_proc%d_%d", TEST_DIR, i, j);
                            snprintf(new_path, sizeof(new_path),
                                    "%s/mixed_proc%d_renamed_%d",
                                    TEST_DIR, i, j);
                            rename(old_path, new_path);
                            /* 清理 */
                            unlink(new_path);
                        }
                        break;
                }
            }
            exit(0);
        }
    }

    /* 等待所有子进程完成 */
    for (i = 0; i < num_processes; i++) {
        int status;
        if (waitpid(pids[i], &status, 0) < 0) {
            errors++;
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            errors++;
        }
    }

    double elapsed = elapsed_ms(&start_time);
    printf("混合操作耗时: %.2f ms\n", elapsed);

    TEST_ASSERT(errors == 0, "混合操作压力测试");

    /* 清理残留文件 */
    {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -f %s/mixed_* 2>/dev/null", TEST_DIR);
        if (system(cmd) != 0)
            fprintf(stderr, "清理混合测试文件失败\n");
    }

    return errors;
}

int main(int argc, char *argv[]) {
    int num_processes = 4;
    int total_errors = 0;

    (void)argc;
    (void)argv;

    printf("========================================\n");
    printf("PowerFS 并发压力测试\n");
    printf("========================================\n\n");

    /* 创建测试目录 */
    if (mkdir(TEST_DIR, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "创建测试目录失败: %s\n", strerror(errno));
        return 1;
    }

    /* 执行所有测试 */
    total_errors += test_concurrent_create(num_processes);
    total_errors += test_concurrent_readwrite(num_processes);
    total_errors += test_concurrent_delete(num_processes);
    total_errors += test_deep_directory();
    total_errors += test_many_small_files();
    total_errors += test_mixed_operations(num_processes);

    /* 清理测试目录 */
    {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s 2>/dev/null", TEST_DIR);
        if (system(cmd) != 0)
            fprintf(stderr, "清理测试目录失败\n");
    }

    /* 输出总结 */
    printf("\n========================================\n");
    printf("测试总结\n");
    printf("========================================\n");
    printf("总测试数: %d\n", test_total);
    printf("通过: %d\n", test_passed);
    printf("失败: %d\n", test_total - test_passed);
    printf("错误数: %d\n", total_errors);

    if (test_failed || total_errors > 0) {
        printf("\n*** 测试失败 ***\n");
        return 1;
    } else {
        printf("\n*** 所有测试通过 ***\n");
        return 0;
    }
}