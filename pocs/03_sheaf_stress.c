#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <linux/aio_abi.h>

static volatile int stop;

static void *churn_timerfd(void *arg)
{
	long n = 0;
	(void)arg;
	while (!stop) {
		for (int i = 0; i < 64; i++) {
			int fd = syscall(SYS_timerfd_create, CLOCK_REALTIME, 0);
			if (fd < 0) continue;
			struct itimerspec its = {
				.it_value = { .tv_sec = 0, .tv_nsec = 1000000 },
			};
			syscall(SYS_timerfd_settime, fd,
				TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &its, NULL);
			if (n & 1) {
				struct itimerspec its2 = { .it_value = { .tv_sec = 1 } };
				syscall(SYS_timerfd_settime, fd, 0, &its2, NULL);
			}
			close(fd);
			n++;
		}
	}
	return (void *)n;
}

static void *churn_aio(void *arg)
{
	long n = 0;
	(void)arg;
	aio_context_t last = 0;
	while (!stop) {
		for (int i = 0; i < 32; i++) {
			aio_context_t ctx = 0;
			if (syscall(SYS_io_setup, 4, &ctx) == 0) {
				if (last)
					syscall(SYS_io_destroy, last);
				last = ctx;
				n++;
			}
		}
		if (last) { syscall(SYS_io_destroy, last); last = 0; }
	}
	return (void *)n;
}

int main(void)
{
	printf("STRESS start uid=%d\n", getuid());
	pthread_t th[6];
	for (int i = 0; i < 6; i++) {
		if (i < 4)
			pthread_create(&th[i], NULL, churn_timerfd, NULL);
		else
			pthread_create(&th[i], NULL, churn_aio, NULL);
	}
	struct timespec ts = { .tv_sec = 25 };
	nanosleep(&ts, NULL);
	stop = 1;
	long total = 0;
	for (int i = 0; i < 6; i++) {
		void *r = NULL;
		pthread_join(th[i], &r);
		total += (long)r;
	}
	printf("STRESS churned=%ld\n", total);
	printf("POC_OK\n");
	return 0;
}