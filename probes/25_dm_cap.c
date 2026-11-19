#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/dm-ioctl.h>

static int ctl_fd = -1;

static int dm_ioctl(int cmd, struct dm_ioctl *dmi, size_t len)
{
	int r;
	dmi->version[0] = 4;
	dmi->version[1] = 0;
	dmi->version[2] = 0;
	dmi->data_size = len;
	dmi->data_start = sizeof(struct dm_ioctl);
	r = ioctl(ctl_fd, cmd, dmi);
	if (r < 0)
		return -errno;
	return 0;
}

static void dm_version(void)
{
	static char buf[4096];
	struct dm_ioctl *dmi = (struct dm_ioctl *)buf;
	int rc;

	memset(buf, 0, sizeof(buf));
	rc = dm_ioctl(DM_VERSION, dmi, sizeof(buf));
	printf("DM_VERSION                    rc=%d errno=%d\n", rc, rc ? errno : 0);
	if (rc == 0)
		printf("  kernel ver=%u.%u.%u\n", dmi->version[0], dmi->version[1], dmi->version[2]);
	fflush(stdout);
}

static void dm_list(void)
{
	static char buf[16384];
	struct dm_ioctl *dmi = (struct dm_ioctl *)buf;
	int rc;

	memset(buf, 0, sizeof(buf));
	rc = dm_ioctl(DM_LIST_DEVICES, dmi, sizeof(buf));
	printf("DM_LIST_DEVICES               rc=%d errno=%d count=%u\n",
	       rc, rc ? errno : 0, dmi->target_count);
	fflush(stdout);
}

static void dm_list_versions(void)
{
	static char buf[16384];
	struct dm_ioctl *dmi = (struct dm_ioctl *)buf;
	int rc;
	unsigned int i;

	memset(buf, 0, sizeof(buf));
	rc = dm_ioctl(DM_LIST_VERSIONS, dmi, sizeof(buf));
	printf("DM_LIST_VERSIONS              rc=%d errno=%d flags=%u\n",
	       rc, rc ? errno : 0, dmi->flags);
	fflush(stdout);
	if (rc == 0) {
		char *p = buf + sizeof(struct dm_ioctl);
		for (i = 0; i < dmi->flags; i++) {
			struct dm_target_versions *v = (struct dm_target_versions *)p;
			printf("  target: %-20s %u.%u.%u\n", v->name,
			       v->version[0], v->version[1], v->version[2]);
			p += ((v->next - (unsigned long)p) + 7) & ~7UL;
		}
	}
	fflush(stdout);
}

static void try_target(const char *tt, const char *spec)
{
	static char buf[8192];
	static char buf2[4096];
	struct dm_ioctl *dmi = (struct dm_ioctl *)buf2;
	struct dm_target_spec *s;
	char *p;
	size_t sl;
	int rc;

	memset(buf2, 0, sizeof(buf2));
	snprintf(dmi->name, sizeof(dmi->name), "probe0");
	dmi->target_count = 1;

	s = (struct dm_target_spec *)(buf2 + sizeof(struct dm_ioctl));
	s->sector_start = 0;
	s->length = 2048;
	snprintf(s->target_type, sizeof(s->target_type), "%s", tt);

	p = (char *)s + sizeof(struct dm_target_spec);
	strcpy(p, spec);
	sl = sizeof(struct dm_target_spec) + strlen(spec) + 1;
	sl = (sl + 7) & ~7UL;

	rc = dm_ioctl(DM_TABLE_LOAD, dmi, sizeof(struct dm_ioctl) + sl);
	printf("LOAD %-12s spec=\"%s\" rc=%d errno=%d\n",
	       tt, spec, rc, rc ? errno : 0);
	fflush(stdout);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== DM CAPABILITY PROBE ===\n");

	ctl_fd = open("/dev/mapper/control", O_RDWR);
	if (ctl_fd < 0) {
		printf("open control failed errno=%d\n", errno);
		return 1;
	}
	printf("control fd=%d\n", ctl_fd);

	dm_version();
	dm_list();
	dm_list_versions();

	printf("--- create + try each target ---\n");
	fflush(stdout);
	{
		static char cb[4096];
		struct dm_ioctl *c = (struct dm_ioctl *)cb;
		int rc;

		memset(cb, 0, sizeof(cb));
		snprintf(c->name, sizeof(c->name), "probe0");
		c->flags = DM_PERSISTENT_DEV_FLAG;
		rc = dm_ioctl(DM_DEV_CREATE, c, sizeof(cb));
		printf("DM_DEV_CREATE probe0          rc=%d errno=%d\n", rc, rc ? errno : 0);
		fflush(stdout);
	}

	try_target("zero", "0");
	try_target("linear", "/dev/ram0 0");
	try_target("integrity", "/dev/ram0 0 2048 J 1 internal_hash:sha256 recalculate");
	try_target("snapshot", "/dev/ram0 /dev/ram1 P 8");
	try_target("stripe", "2 8");
	try_target("error", "0");

	printf("=== CAP PROBE DONE ===\n");
	close(ctl_fd);
	return 0;
}