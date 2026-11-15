#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <linux/if_alg.h>

static int alg_open(const char *type, const char *name)
{
	int fd = socket(AF_ALG, SOCK_SEQPACKET, 0);
	if (fd < 0) return -1;
	struct sockaddr_alg sa;
	memset(&sa, 0, sizeof(sa));
	sa.salg_family = AF_ALG;
	strncpy((char *)sa.salg_type, type, sizeof(sa.salg_type) - 1);
	strncpy((char *)sa.salg_name, name, sizeof(sa.salg_name) - 1);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(fd); return -1; }
	unsigned char key[32] = {0};
	if (setsockopt(fd, SOL_ALG, ALG_SET_KEY, key, 32) < 0) { close(fd); return -1; }
	int op = accept(fd, NULL, NULL);
	close(fd);
	return op;
}

static long do_send(int fd, void *buf, size_t len)
{
	struct iovec iov = { .iov_base = buf, .iov_len = len };
	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	return sendmsg(fd, &msg, 0);
}

static long do_recv(int fd, struct iovec *iov, int iovcnt)
{
	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = iov;
	msg.msg_iovlen = iovcnt;
	return recvmsg(fd, &msg, 0);
}

int main(void)
{
	int op = alg_open("skcipher", "cbc(aes)");
	if (op < 0) { printf("ALG open failed errno=%d\n", errno); printf("POC_OK\n"); return 0; }
	printf("ALG opfd=%d\n", op);

	unsigned char data[512];
	memset(data, 0x41, sizeof(data));
	long s = do_send(op, data, sizeof(data));
	printf("ALG send=%ld errno=%d\n", s, errno);

	void *p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) { printf("ALG mmap failed\n"); close(op); printf("POC_OK\n"); return 0; }
	munmap(p, 8192);

	struct iovec iov;
	iov.iov_base = p;
	iov.iov_len = 4096;
	long r = do_recv(op, &iov, 1);
	printf("ALG recv_unmapped=%ld errno=%d\n", r, errno);

	iov.iov_base = (void *)0x1000;
	iov.iov_len = 4096;
	r = do_recv(op, &iov, 1);
	printf("ALG recv_nullpage=%ld errno=%d\n", r, errno);

	void *q = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (q != MAP_FAILED) {
		mprotect(q, 65536, PROT_NONE);
		iov.iov_base = q;
		iov.iov_len = 65536;
		r = do_recv(op, &iov, 1);
		printf("ALG recv_protnone=%ld errno=%d\n", r, errno);
	}

	close(op);

	int h = alg_open("hash", "sha256");
	if (h >= 0) {
		printf("HASH opfd=%d\n", h);
		do_send(h, data, sizeof(data));
		iov.iov_base = (void *)0x2000;
		iov.iov_len = 4096;
		r = do_recv(h, &iov, 1);
		printf("HASH recv_badptr=%ld errno=%d\n", r, errno);
		void *w = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (w != MAP_FAILED) {
			munmap(w, 8192);
			iov.iov_base = w;
			iov.iov_len = 4096;
			r = do_recv(h, &iov, 1);
			printf("HASH recv_unmapped=%ld errno=%d\n", r, errno);
		}
		close(h);
	} else {
		printf("HASH open failed errno=%d\n", errno);
	}

	printf("POC_OK\n");
	return 0;
}