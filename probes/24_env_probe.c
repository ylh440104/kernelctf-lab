#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <linux/dm-ioctl.h>

int main(void)
{
	static const char *devs[] = {
		"/dev/ram0", "/dev/ram1", "/dev/loop0", "/dev/loop1",
		"/dev/vda", "/dev/vdb", "/dev/sda", "/dev/null", NULL
	};
	int i;
	struct stat st;

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== ENV PROBE ===\n");

	for (i = 0; devs[i]; i++) {
		if (stat(devs[i], &st) == 0) {
			printf("EXIST %-14s mode=%o rdev=%u:%u\n", devs[i],
			       st.st_mode, major(st.st_rdev), minor(st.st_rdev));
		} else {
			printf("MISS  %-14s errno=%d\n", devs[i], errno);
		}
	}

	printf("--- block devices from /sys/block ---\n");
	fflush(stdout);
	system("ls /sys/block 2>/dev/null");
	fflush(stdout);

	printf("--- dm modules ---\n");
	fflush(stdout);
	system("grep -E 'dm_integrity|dm_snapshot|dm_thin' /proc/modules 2>/dev/null | head -5; "
	       "ls /sys/module | grep -E 'dm_' | head -10");
	fflush(stdout);

	printf("--- try create loop device from file ---\n");
	fflush(stdout);
	{
		int fd = open("/tmp/img0", O_RDWR | O_CREAT | O_TRUNC, 0600);
		if (fd >= 0) {
			char zero[4096];
			int n;
			memset(zero, 0, sizeof(zero));
			for (n = 0; n < 2048; n++)
				write(fd, zero, sizeof(zero));
			close(fd);
			printf("created /tmp/img0 size=%d\n", 2048 * 4096);
		} else {
			printf("create /tmp/img0 failed errno=%d\n", errno);
		}
	}
	fflush(stdout);

	printf("=== ENV PROBE DONE ===\n");
	return 0;
}