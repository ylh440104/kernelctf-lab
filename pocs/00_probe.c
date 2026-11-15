#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <linux/if_alg.h>

static void rd(const char *p)
{
	char buf[64] = {0};
	int fd = open(p, O_RDONLY);
	if (fd < 0) { printf("SYSCTL %-42s ERR=%d\n", p, errno); return; }
	int n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n > 0) { buf[strcspn(buf, "\n")] = 0; printf("SYSCTL %-42s %s\n", p, buf); }
}

static int check_alg(void)
{
	int fd = socket(AF_ALG, SOCK_SEQPACKET, 0);
	if (fd < 0) { printf("SURF af_alg ERR=%d\n", errno); return -1; }
	struct sockaddr_alg sa;
	memset(&sa, 0, sizeof(sa));
	sa.salg_family = AF_ALG;
	strcpy((char *)sa.salg_type, "skcipher");
	strcpy((char *)sa.salg_name, "cbc(aes)");
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		printf("SURF af_alg bind_ERR=%d\n", errno);
		close(fd);
		return -1;
	}
	unsigned char key[16] = {0};
	if (setsockopt(fd, SOL_ALG, ALG_SET_KEY, key, sizeof(key)) < 0) {
		printf("SURF af_alg setkey_ERR=%d\n", errno);
		close(fd);
		return -1;
	}
	int op = accept(fd, NULL, NULL);
	printf("SURF af_alg OK opfd=%d\n", op);
	if (op >= 0) close(op);
	close(fd);
	return 0;
}

int main(void)
{
	struct utsname u;
	uname(&u);
	printf("PROBE uid=%d gid=%d\n", getuid(), getgid());
	printf("PROBE kernel=%s\n", u.release);
	printf("PROBE arch=%s\n", u.machine);

	rd("/proc/sys/kernel/unprivileged_bpf_disabled");
	rd("/proc/sys/kernel/unprivileged_userfaultfd");
	rd("/proc/sys/kernel/perf_event_paranoid");
	rd("/proc/sys/kernel/kptr_restrict");
	rd("/proc/sys/kernel/dmesg_restrict");
	rd("/proc/sys/kernel/msgmax");
	rd("/proc/sys/kernel/msgmnb");
	rd("/proc/sys/kernel/keys/maxkeys");
	rd("/proc/sys/vm/unprivileged_userfaultfd");

	check_alg();

	int s = socket(AF_UNIX, SOCK_DGRAM, 0);
	printf("SURF af_unix_dgram %s\n", s >= 0 ? "OK" : "ERR");
	if (s >= 0) close(s);

	s = socket(AF_INET, SOCK_DGRAM, 0);
	printf("SURF af_inet_udp %s\n", s >= 0 ? "OK" : "ERR");
	if (s >= 0) close(s);

	int q = msgget(IPC_PRIVATE, 0600 | IPC_CREAT);
	printf("SURF sysv_msg %s id=%d\n", q >= 0 ? "OK" : "ERR", q);
	if (q >= 0) msgctl(q, IPC_RMID, NULL);

	long r = syscall(SYS_userfaultfd, 0);
	printf("SURF userfaultfd %s errno=%d\n", r >= 0 ? "OK" : "ERR", r >= 0 ? 0 : errno);
	if (r >= 0) close((int)r);

	int dev = open("/dev/kvm", O_RDWR);
	printf("SURF dev_kvm %s\n", dev >= 0 ? "OK" : "ERR");
	if (dev >= 0) close(dev);

	dev = open("/dev/net/tun", O_RDWR);
	printf("SURF dev_tun %s\n", dev >= 0 ? "OK" : "ERR");
	if (dev >= 0) close(dev);

	printf("POC_OK\n");
	return 0;
}