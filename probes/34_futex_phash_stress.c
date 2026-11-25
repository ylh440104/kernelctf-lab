#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <signal.h>
#include <linux/futex.h>
#include <stdint.h>
#include <time.h>

#ifndef PR_FUTEX_HASH
#define PR_FUTEX_HASH 78
#define PR_FUTEX_HASH_SET_SLOTS 1
#define PR_FUTEX_HASH_GET_SLOTS 2
#endif

#ifndef SYS_futex_waitv
#define SYS_futex_waitv 449
#endif

#define NFUT 64
#define NWAIT 8
#define NWAKE 4
#define NPI 3

static int *fut;
static volatile int stop_flag;
static unsigned long resizes, waits, wakes, waitvs, pis;
static unsigned int slots_now;

static long futex(int *uaddr, int op, int val, const struct timespec *to,
                  int *uaddr2, int val3)
{
	return syscall(SYS_futex, uaddr, op, val, to, uaddr2, val3);
}

static void bump(unsigned long *p)
{
	__sync_fetch_and_add(p, 1);
}

static void *th_wait(void *arg)
{
	long id = (long)arg;
	int i = (int)(id % NFUT);
	struct timespec to;

	while (!stop_flag) {
		to.tv_sec = 0;
		to.tv_nsec = 100000;
		futex(&fut[i], FUTEX_WAIT_PRIVATE, 0, &to, NULL, 0);
		bump(&waits);
		i = (i + 7) % NFUT;
	}
	return NULL;
}

static void *th_wake(void *arg)
{
	long id = (long)arg;
	int i = (int)(id % NFUT);

	while (!stop_flag) {
		__sync_lock_test_and_set(&fut[i], 1);
		futex(&fut[i], FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
		bump(&wakes);
		__sync_lock_test_and_set(&fut[i], 0);
		i = (i + 3) % NFUT;
	}
	return NULL;
}

static void *th_resize(void *arg)
{
	static const unsigned int sizes[] = { 16, 32, 64, 128, 256, 512, 1024, 2048 };
	int k = 0;
	(void)arg;

	while (!stop_flag) {
		unsigned int sz = sizes[k % (int)(sizeof(sizes) / sizeof(sizes[0]))];
		long r = prctl(PR_FUTEX_HASH, PR_FUTEX_HASH_SET_SLOTS, sz, 0, 0);
		if (r == 0)
			bump(&resizes);
		slots_now = (unsigned int)prctl(PR_FUTEX_HASH, PR_FUTEX_HASH_GET_SLOTS, 0, 0, 0);
		k++;
	}
	return NULL;
}

struct wv {
	uint64_t val;
	uint32_t uaddr_lo;
	uint32_t uaddr_hi;
	uint32_t flags;
	uint32_t reserved;
};

static void *th_waitv(void *arg)
{
	int idx[4];
	long id = (long)arg;
	struct wv v[4];
	struct timespec to;
	int k;

	for (k = 0; k < 4; k++)
		idx[k] = (int)((id * 5 + k * 11) % NFUT);

	while (!stop_flag) {
		for (k = 0; k < 4; k++) {
			uintptr_t p = (uintptr_t)&fut[idx[k]];
			v[k].val = 0;
			v[k].uaddr_lo = (uint32_t)(p & 0xffffffffu);
			v[k].uaddr_hi = (uint32_t)(p >> 32);
			v[k].flags = FUTEX_PRIVATE_FLAG;
			v[k].reserved = 0;
		}
		to.tv_sec = 0;
		to.tv_nsec = 200000;
		syscall(SYS_futex_waitv, v, 4, 0, &to, 0);
		bump(&waitvs);
	}
	return NULL;
}

static void *th_pi(void *arg)
{
	long id = (long)arg;
	int i = (int)(id % NFUT);

	while (!stop_flag) {
		futex(&fut[i], FUTEX_LOCK_PI_PRIVATE, 0, NULL, NULL, 0);
		futex(&fut[i], FUTEX_UNLOCK_PI_PRIVATE, 0, NULL, NULL, 0);
		bump(&pis);
		i = (i + 13) % NFUT;
	}
	return NULL;
}

static void *th_churn(void *arg)
{
	(void)arg;
	while (!stop_flag) {
		pthread_t t[3];
		int k;
		for (k = 0; k < 3; k++) {
			if (pthread_create(&t[k], NULL, th_wait, (void *)(long)(k + 1)))
				break;
		}
		for (k = 0; k < 3; k++)
			pthread_join(t[k], NULL);
	}
	return NULL;
}

static void run_children(int seconds)
{
	pid_t kids[8];
	int k, done = 0;

	for (k = 0; k < 8; k++) {
		pid_t p = fork();

		if (p == 0) {
			int i;
			for (i = 0; i < 2000; i++) {
				prctl(PR_FUTEX_HASH, PR_FUTEX_HASH_SET_SLOTS, 16 << (i % 7), 0, 0);
				futex(&fut[(i * 3 + k) % NFUT], FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
				futex(&fut[(i * 3 + k) % NFUT], FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
				if (stop_flag)
					break;
			}
			_exit(0);
		}
		kids[done++] = p;
	}

	for (k = 0; k < seconds; k++) {
		usleep(1000000);
		if (stop_flag)
			break;
	}

	for (k = 0; k < done; k++) {
		kill(kids[k], SIGKILL);
		waitpid(kids[k], NULL, 0);
	}
}

int main(int argc, char **argv)
{
	pthread_t th[NWAIT + NWAKE + NPI + 4];
	int n = 0, i;
	int seconds = 90;

	if (argc > 1)
		seconds = atoi(argv[1]);

	setvbuf(stdout, NULL, _IONBF, 0);
	fut = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (fut == MAP_FAILED) {
		printf("mmap failed\n");
		return 1;
	}
	memset(fut, 0, 4096);

	printf("START pid=%d threads=%d seconds=%d\n", getpid(),
	       NWAIT + NWAKE + NPI + 4, seconds);

	for (i = 0; i < NWAIT; i++)
		pthread_create(&th[n++], NULL, th_wait, (void *)(long)i);
	for (i = 0; i < NWAKE; i++)
		pthread_create(&th[n++], NULL, th_wake, (void *)(long)i);
	for (i = 0; i < NPI; i++)
		pthread_create(&th[n++], NULL, th_pi, (void *)(long)i);
	pthread_create(&th[n++], NULL, th_resize, NULL);
	pthread_create(&th[n++], NULL, th_waitv, (void *)(long)1);
	pthread_create(&th[n++], NULL, th_waitv, (void *)(long)2);
	pthread_create(&th[n++], NULL, th_churn, NULL);

	run_children(seconds);

	stop_flag = 1;
	for (i = 0; i < n; i++)
		pthread_join(th[i], NULL);

	printf("STAT resizes=%lu waits=%lu wakes=%lu waitv=%lu pi=%lu slots=%u\n",
	       resizes, waits, wakes, waitvs, pis, slots_now);
	printf("POC DONE\n");
	return 0;
}
