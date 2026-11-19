#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/dm-ioctl.h>

#define DM_CONTROL "/dev/mapper/control"

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

static int dm_create(const char *name)
{
	static char buf[4096];
	struct dm_ioctl *dmi = (struct dm_ioctl *)buf;

	memset(buf, 0, sizeof(buf));
	snprintf(dmi->name, sizeof(dmi->name), "%s", name);
	dmi->flags = DM_PERSISTENT_DEV_FLAG;
	return dm_ioctl(DM_DEV_CREATE, dmi, sizeof(buf));
}

static int dm_remove(const char *name)
{
	static char buf[4096];
	struct dm_ioctl *dmi = (struct dm_ioctl *)buf;

	memset(buf, 0, sizeof(buf));
	snprintf(dmi->name, sizeof(dmi->name), "%s", name);
	return dm_ioctl(DM_DEV_REMOVE, dmi, sizeof(buf));
}

static int dm_load(const char *name, unsigned long long start,
		   unsigned long long length, const char *target_spec)
{
	static char buf[8192];
	struct dm_ioctl *dmi = (struct dm_ioctl *)buf;
	struct dm_target_spec *spec;
	char *p;
	size_t spec_len;

	memset(buf, 0, sizeof(buf));
	snprintf(dmi->name, sizeof(dmi->name), "%s", name);
	dmi->target_count = 1;

	spec = (struct dm_target_spec *)(buf + sizeof(struct dm_ioctl));
	spec->sector_start = start;
	spec->length = length;
	snprintf(spec->target_type, sizeof(spec->target_type), "integrity");

	p = (char *)spec + sizeof(struct dm_target_spec);
	strcpy(p, target_spec);
	spec_len = sizeof(struct dm_target_spec) + strlen(target_spec) + 1;
	spec_len = (spec_len + 7) & ~7UL;

	return dm_ioctl(DM_TABLE_LOAD, dmi, sizeof(struct dm_ioctl) + spec_len);
}

static int dm_suspend(const char *name, int do_suspend)
{
	static char buf[4096];
	struct dm_ioctl *dmi = (struct dm_ioctl *)buf;

	memset(buf, 0, sizeof(buf));
	snprintf(dmi->name, sizeof(dmi->name), "%s", name);
	dmi->flags = do_suspend ? DM_SUSPEND_FLAG : 0;
	return dm_ioctl(DM_DEV_SUSPEND, dmi, sizeof(buf));
}

static void st(const char *tag, int rc)
{
	printf("%-46s rc=%d errno=%d\n", tag, rc, rc ? errno : 0);
	fflush(stdout);
}

static void dump_dmesg(const char *tag)
{
	char cmd[256];
	printf("---- dmesg after %s ----\n", tag);
	fflush(stdout);
	snprintf(cmd, sizeof(cmd), "dmesg | tail -12");
	system(cmd);
	fflush(stdout);
}

static void try_load(const char *tag, unsigned long long start,
		     unsigned long long len, const char *spec)
{
	int rc;

	dm_remove("dmi0");
	dm_create("dmi0");
	rc = dm_load("dmi0", start, len, spec);
	printf("[%s]\n  spec=%s\n", tag, spec);
	st("  DM_TABLE_LOAD", rc);
	if (rc == 0) {
		rc = dm_suspend("dmi0", 0);
		st("  DM_DEV_SUSPEND resume", rc);
	}
	dm_remove("dmi0");
}

int main(void)
{
	int rc;

	setvbuf(stdout, NULL, _IONBF, 0);

	printf("=== DM_INTEGRITY PARAM PROBE ===\n");

	ctl_fd = open(DM_CONTROL, O_RDWR);
	if (ctl_fd < 0) {
		printf("open control failed errno=%d\n", errno);
		return 1;
	}
	printf("control fd=%d\n", ctl_fd);

	try_load("A ram0 2048 sectors J",
		 0, 2048, "ram0 0 2048 J 1 journal_sectors:16 internal_hash:sha256");

	try_load("B ram0 with recalculate",
		 0, 2048, "ram0 0 2048 J 1 journal_sectors:16 internal_hash:sha256 recalculate");

	try_load("C ram0 S mode",
		 0, 2048, "ram0 0 2048 S 1 internal_hash:sha256 recalculate");

	try_load("D ram0 minimal",
		 0, 2048, "ram0 0 2048 J 1 internal_hash:sha256");

	try_load("E ram1 bigger",
		 0, 8192, "ram1 0 8192 J 2 journal_sectors:32 internal_hash:sha256 recalculate");

	printf("=== PROBE DONE ===\n");
	close(ctl_fd);
	return 0;
}