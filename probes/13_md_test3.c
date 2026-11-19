#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/raid/md_u.h>

static int wr(const char *p, const char *v, int *er)
{
    int fd = open(p, O_WRONLY);
    if (fd < 0) { if (er) *er = errno; return -1; }
    ssize_t n = write(fd, v, strlen(v));
    int e = errno;
    close(fd);
    if (er) *er = e;
    return n < 0 ? -1 : 0;
}

static void rd1(const char *p, const char *tag)
{
    char b[96];
    int fd = open(p, O_RDONLY);
    if (fd < 0) { printf("%s=(no)\n", tag); return; }
    int n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) { printf("%s=(empty)\n", tag); return; }
    b[n] = 0;
    char *nl = strchr(b, '\n');
    if (nl) *nl = 0;
    printf("%s=%s\n", tag, b);
}

int main(void)
{
    printf("TEST3 uid=%d\n", getuid());
    fflush(stdout);

    int e = 0;
    wr("/sys/module/md_mod/parameters/new_array", "md7", &e);
    printf("create e=%d\n", e);

    printf("--- 可选属性探测 ---\n");
    const char *attrs[] = {
        "/sys/block/md7/md/metadata_version",
        "/sys/block/md7/md/chunk_size",
        "/sys/block/md7/md/component_size",
        "/sys/block/md7/md/layout",
        "/sys/block/md7/md/safe_mode_delay",
        "/sys/block/md7/md/sync_action",
        "/sys/block/md7/md/mismatch_cnt",
        "/sys/block/md7/md/reshape_position",
        NULL
    };
    for (int i = 0; attrs[i]; i++) {
        struct stat st;
        printf("%s %s\n", stat(attrs[i], &st) == 0 ? "HAVE" : "MISS", attrs[i]);
    }

    e = 0; wr("/sys/block/md7/md/metadata_version", "1.2", &e);
    printf("metadata_version=1.2 e=%d\n", e);
    rd1("/sys/block/md7/md/metadata_version", "  ver_now");

    e = 0; wr("/sys/block/md7/md/level", "raid1", &e);
    printf("level e=%d\n", e);

    e = 0; wr("/sys/block/md7/md/raid_disks", "1", &e);
    printf("raid_disks e=%d\n", e);

    e = 0; wr("/sys/block/md7/md/new_dev", "1:0", &e);
    printf("new_dev 1:0 e=%d\n", e);

    printf("--- 探测 dev 目录 ---\n");
    system("ls /sys/block/md7/md/ 2>&1 | head -20");
    system("ls /sys/block/md7/md/dev-ram0/ 2>&1 | head -10");

    e = 0;
    int r = wr("/sys/block/md7/md/array_state", "active", &e);
    printf("active r=%d e=%d\n", r, e);
    rd1("/sys/block/md7/md/array_state", "  state");

    printf("--- 再试 in_sync ---\n");
    e = 0; wr("/sys/block/md7/md/dev-ram0/state", "in_sync", &e);
    printf("dev state e=%d\n", e);
    e = 0; r = wr("/sys/block/md7/md/array_state", "active", &e);
    printf("active2 r=%d e=%d\n", r, e);
    rd1("/sys/block/md7/md/array_state", "  state2");

    printf("--- 用 ioctl 方式 ---\n");
    int fd = open("/dev/md7", O_RDONLY);
    printf("open /dev/md7 = %d errno=%d\n", fd, fd < 0 ? errno : 0);
    if (fd >= 0) close(fd);

    printf("--- dmesg ---\n");
    system("dmesg | tail -25");

    printf("TEST3 done\n");
    fflush(stdout);
    return 0;
}