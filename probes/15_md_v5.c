#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

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

static void show(const char *p)
{
    char b[128];
    int fd = open(p, O_RDONLY);
    if (fd < 0) { printf("  %s=(no)\n", p); return; }
    int n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) { printf("  %s=(empty)\n", p); return; }
    b[n] = 0;
    char *nl = strchr(b, '\n');
    if (nl) *nl = 0;
    printf("  %s = %s\n", p, b);
}

int main(void)
{
    int e = 0;
    printf("V5 start\n"); fflush(stdout);

    wr("/sys/module/md_mod/parameters/new_array", "md3", &e);
    printf("create e=%d\n", e);

    wr("/sys/block/md3/md/metadata_version", "none", &e);
    printf("meta=none e=%d\n", e);
    show("/sys/block/md3/md/metadata_version");

    wr("/sys/block/md3/md/level", "raid1", &e);
    printf("level e=%d\n", e);
    show("/sys/block/md3/md/level");

    wr("/sys/block/md3/md/raid_disks", "1", &e);
    printf("disks e=%d\n", e);
    show("/sys/block/md3/md/raid_disks");

    wr("/sys/block/md3/md/new_dev", "1:0", &e);
    printf("new_dev e=%d\n", e);

    printf("--- dev dir ---\n");
    system("ls /sys/block/md3/md/ 2>&1");

    wr("/sys/block/md3/md/dev-ram0/slot", "0", &e);
    printf("slot=0 e=%d\n", e);

    wr("/sys/block/md3/md/dev-ram0/state", "in_sync", &e);
    printf("state=in_sync e=%d\n", e);
    show("/sys/block/md3/md/dev-ram0/state");

    printf("--- component_size ---\n");
    show("/sys/block/md3/md/component_size");
    wr("/sys/block/md3/md/component_size", "1024", &e);
    printf("component_size=1024 e=%d\n", e);

    printf("--- active ---\n");
    e = 0;
    int r = wr("/sys/block/md3/md/array_state", "active", &e);
    printf("active r=%d e=%d\n", r, e);
    show("/sys/block/md3/md/array_state");

    printf("--- dmesg md lines ---\n");
    system("dmesg 2>/dev/null | grep -iE 'md|raid|ram0' | tail -30");

    printf("--- clear ---\n");
    e = 0;
    r = wr("/sys/block/md3/md/array_state", "clear", &e);
    printf("clear r=%d e=%d\n", r, e);

    printf("--- dmesg after clear ---\n");
    system("dmesg 2>/dev/null | tail -25");

    printf("V5 done\n"); fflush(stdout);
    return 0;
}