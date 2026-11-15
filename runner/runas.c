#include <stdio.h>
#include <unistd.h>
#include <grp.h>
#include <errno.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: runas prog [args]\n");
		return 2;
	}
	if (setgroups(0, NULL) != 0)
		fprintf(stderr, "runas: setgroups: %d\n", errno);
	if (setgid(65534) != 0)
		fprintf(stderr, "runas: setgid: %d\n", errno);
	if (setuid(65534) != 0)
		fprintf(stderr, "runas: setuid: %d\n", errno);
	if (getuid() != 65534) {
		fprintf(stderr, "runas: failed to drop privileges, uid=%d\n", getuid());
		return 1;
	}
	execv(argv[1], &argv[1]);
	fprintf(stderr, "runas: execv %s: %d\n", argv[1], errno);
	return 127;
}