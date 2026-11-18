#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>

static void R(const char *n, long rc, int e)
{
    if (rc >= 0)
        printf("OK   %-40s rc=%ld\n", n, rc);
    else
        printf("DENY %-40s errno=%d(%s)\n", n, e, strerror(e));
}

static int wr(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    ssize_t w = write(fd, val, strlen(val));
    int e = errno;
    close(fd);
    if (w < 0) { errno = e; return -1; }
    return 0;
}

static long perf_open(int type, unsigned long long cfg, unsigned long flags)
{
    struct perf_event_attr a;
    memset(&a, 0, sizeof(a));
    a.type = type;
    a.size = sizeof(a);
    a.config = cfg;
    a.disabled = 1;
    a.exclude_kernel = 0;
    return syscall(__NR_perf_event_open, &a, 0, -1, -1, flags);
}

int main(void)
{
    printf("=== PROBE target surface uid=%d euid=%d ===\n", getuid(), geteuid());

    printf("--- tracefs 状态 ---\n");
    struct stat st;
    const char *paths[] = {
        "/sys/kernel/tracing",
        "/sys/kernel/debug/tracing",
        "/sys/kernel/debug",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        if (stat(paths[i], &st) == 0)
            printf("EXIST %-32s mode=%o uid=%d gid=%d\n", paths[i], st.st_mode & 07777, st.st_uid, st.st_gid);
        else
            printf("MISS  %-32s\n", paths[i]);
    }

    printf("--- tracefs 写入测试 ---\n");
    R("write tracing_on", wr("/sys/kernel/tracing/tracing_on", "0"), errno);
    R("write set_event (empty)", wr("/sys/kernel/tracing/set_event", ""), errno);
    R("write current_tracer=nop", wr("/sys/kernel/tracing/current_tracer", "nop"), errno);
    R("write events/sched/enable=0", wr("/sys/kernel/tracing/events/sched/enable", "0"), errno);

    printf("--- tracefs filter 测试（关键）---\n");
    R("create kprobe_events", wr("/sys/kernel/tracing/kprobe_events", ""), errno);
    R("filter: prev_pid==1", wr("/sys/kernel/tracing/events/sched/sched_switch/filter", "prev_pid == 1"), errno);
    R("filter: 深层括号", wr("/sys/kernel/tracing/events/sched/sched_switch/filter",
        "((((prev_pid == 1))))"), errno);
    R("filter: 长表达式", wr("/sys/kernel/tracing/events/sched/sched_switch/filter",
        "prev_pid == 1 && prev_pid == 2 && prev_pid == 3 && prev_pid == 4 && prev_pid == 5 && prev_pid == 6"), errno);
    R("filter: 清理", wr("/sys/kernel/tracing/events/sched/sched_switch/filter", "0"), errno);

    printf("--- perf_event_open 测试 ---\n");
    long p;
    p = perf_open(PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_CLOCK, 0);
    R("perf SW_CPU_CLOCK (cpu0)", p, errno); if (p >= 0) close(p);
    p = perf_open(PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK, 0);
    R("perf SW_TASK_CLOCK", p, errno); if (p >= 0) close(p);
    p = perf_open(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, 0);
    R("perf HW_CPU_CYCLES", p, errno); if (p >= 0) close(p);
    p = perf_open(PERF_TYPE_TRACEPOINT, 0, 0);
    R("perf TRACEPOINT", p, errno); if (p >= 0) close(p);

    printf("--- perf_event_paranoid ---\n");
    int fd = open("/proc/sys/kernel/perf_event_paranoid", O_RDONLY);
    if (fd >= 0) {
        char b[32] = {0};
        read(fd, b, sizeof(b) - 1);
        printf("perf_event_paranoid = %s", b);
        close(fd);
    } else {
        printf("cannot read perf_event_paranoid\n");
    }

    printf("--- hugetlbfs 测试 ---\n");
    if (stat("/dev/hugepages", &st) == 0)
        printf("EXIST /dev/hugepages mode=%o\n", st.st_mode & 07777);
    else
        printf("MISS  /dev/hugepages\n");
    fd = open("/proc/sys/vm/nr_hugepages", O_RDWR);
    R("open nr_hugepages rw", fd, errno);
    if (fd >= 0) close(fd);
    fd = open("/sys/kernel/mm/hugepages", O_RDONLY | O_DIRECTORY);
    R("open /sys/kernel/mm/hugepages", fd, errno);
    if (fd >= 0) close(fd);

    printf("--- 其它无特权面 ---\n");
    int ufd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    R("userfaultfd", ufd, errno); if (ufd >= 0) close(ufd);
    int mfd = syscall(__NR_memfd_create, "x", 0);
    R("memfd_create", mfd, errno); if (mfd >= 0) close(mfd);
    int ifd = syscall(__NR_io_uring_setup, 8, NULL);
    R("io_uring_setup", ifd, errno); if (ifd >= 0) close(ifd);
    int bfd = syscall(__NR_bpf, 0, NULL, 0);
    R("bpf (null)", bfd, errno); if (bfd >= 0) close(bfd);

    printf("=== PROBE DONE ===\n");
    return 0;
}