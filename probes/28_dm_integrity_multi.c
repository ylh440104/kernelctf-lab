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

static int dm_ioctl_ver(int cmd, struct dm_ioctl *dmi, size_t len,
			unsigned int v0, unsigned int v1, unsigned int v2)
{
	int r;
	dmi->version[0] = v0;
	dmi->version[1] = v1;
	dmi->version[2] = v2;
	dmi->data_size = len;
	dmi->data_start = sizeof(struct dm_ioctl);
	r = ioctl(ctl_fd, cmd, dmi);
	if (r < 0)
		return -errno;
	return 0;
}

static int dm_ioctl(int cmd, struct dm_ioctl *dmi, size_t len)
{
	return dm_ioctl_ver(cmd, dmi, len, 4, 0, 0);
}

static int dm_cmd_ver(int cmd, const char *name, unsigned int v1)
{
	static char buf[4096];
	struct dm_ioctl *dmi = (struct dm_ioctl *)buf;

	memset(buf, 0, sizeof(buf));
	snprintf(dmi->name, sizeof(dmi->name), "%s", name);
	if (cmd == DM_DEV_CREATE)
		dmi->flags = DM_PERSISTENT_DEV_FLAG;
	return dm_ioctl_ver(cmd, dmi, sizeof(buf), 4, v1, 0);
}

static int dm_cmd(int cmd, const char *name)
{
	return dm_cmd_ver(cmd, name, 0);
}

static int dm_load(const char *name, const char *tt, unsigned long long len,
		   const char *spec, unsigned int v1)
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

	return dm_ioctl_ver(DM_TABLE_LOAD, dmi, sizeof(struct dm_ioctl) + sl, 4, v1, 0);
}

static int dm_suspend(const char *name, int do_suspend, unsigned int v1)
{
	static char buf[4096];
	struct dm_ioctl *dmi = (struct dm_ioctl *)buf;

	memset(buf, 0, sizeof(buf));
	snprintf(dmi->name, sizeof(dmi->name), "%s", name);
	dmi->flags = do_suspend ? DM_SUSPEND_FLAG : 0;
	return dm_ioctl_ver(DM_DEV_SUSPEND, dmi, sizeof(buf), 4, v1, 0);
}

static void st(const char *tag, int rc)
{
	printf("    %-42s rc=%d errno=%d\n", tag, rc, rc ? errno : 0);
	fflush(stdout);
}

static void dmesg_grep(const char *pat)
{
	char cmd[256];
	snprintf(cmd, sizeof(cmd),
		 "dmesg | grep -iE '%s' | tail -3 | sed 's/^/      /'", pat);
	fflush(stdout);
	if (system(cmd) < 0)
		printf("      (n/a)\n");
	fflush(stdout);
}

static void seed_ram(void)
{
	int fd = open("/dev/ram0", O_RDWR);
	char z[4096];
	int i;

	if (fd < 0) {
		printf("  open ram0 failed errno=%d\n", errno);
		return;
	}
	memset(z, 0, sizeof(z));
	for (i = 0; i < 64; i++)
		if (write(fd, z, sizeof(z)) < 0)
			break;
	close(fd);
}

static void variant_A(void)
{
	int rc;
	printf("[A] R mode, no hash, no seed\n");
	dm_cmd(DM_DEV_REMOVE, "va0");
	dm_cmd(DM_DEV_CREATE, "va0");
	rc = dm_load("va0", "integrity", 2048, "/dev/ram0 0 32 R 0", 0);
	st("LOAD R no-hash", rc);
	rc = dm_suspend("va0", 0, 0);
	st("RESUME", rc);
	dm_cmd(DM_DEV_REMOVE, "va0");
}

static void variant_B(void)
{
	int rc;
	printf("[B] reset_recalculate, no hash\n");
	dm_cmd(DM_DEV_REMOVE, "vb0");
	dm_cmd(DM_DEV_CREATE, "vb0");
	rc = dm_load("vb0", "integrity", 2048,
		     "/dev/ram0 0 32 J 1 reset_recalculate", 0);
	st("LOAD reset_recalculate", rc);
	rc = dm_suspend("vb0", 0, 0);
	st("RESUME", rc);
	dm_cmd(DM_DEV_REMOVE, "vb0");
}

static void variant_C(void)
{
	int rc;
	printf("[C] load fail then resume (no table)\n");
	dm_cmd(DM_DEV_REMOVE, "vc0");
	dm_cmd(DM_DEV_CREATE, "vc0");
	rc = dm_load("vc0", "integrity", 100000000ULL, "/dev/ram0 0 32 R 0", 0);
	st("LOAD oversized (expect fail)", rc);
	rc = dm_suspend("vc0", 0, 0);
	st("RESUME after failed load", rc);
	dm_cmd(DM_DEV_REMOVE, "vc0");
}

static void variant_D(void)
{
	int rc;
	printf("[D] bad version v1=0x1000000\n");
	dm_cmd_ver(DM_DEV_REMOVE, "vd0", 0x1000000);
	dm_cmd_ver(DM_DEV_CREATE, "vd0", 0x1000000);
	rc = dm_load("vd0", "integrity", 2048, "/dev/ram0 0 32 R 0", 0x1000000);
	st("LOAD bad version", rc);
	rc = dm_suspend("vd0", 0, 0x1000000);
	st("RESUME bad version", rc);
	dm_cmd_ver(DM_DEV_REMOVE, "vd0", 0x1000000);
}

static void variant_E(void)
{
	int rc;
	printf("[E] seed sb then R mode\n");
	seed_ram();
	dm_cmd(DM_DEV_REMOVE, "ve0");
	dm_cmd(DM_DEV_CREATE, "ve0");
	rc = dm_load("ve0", "integrity", 2048,
		     "/dev/ram0 0 - J 2 internal_hash:sha256 recalculate", 0);
	st("LOAD seed hash+recalc", rc);
	if (rc == 0) {
		rc = dm_suspend("ve0", 0, 0);
		st("RESUME seed", rc);
		dm_suspend("ve0", 1, 0);
		dm_cmd(DM_DEV_REMOVE, "ve0");
		dm_cmd(DM_DEV_CREATE, "ve0");
		rc = dm_load("ve0", "integrity", 2048, "/dev/ram0 0 32 R 0", 0);
		st("LOAD R on seeded sb", rc);
		rc = dm_suspend("ve0", 0, 0);
		st("RESUME R on seeded", rc);
	}
	dm_cmd(DM_DEV_REMOVE, "ve0");
}

static void variant_F(void)
{
	int rc;
	printf("[F] integrity then thin-pool (mixed targets)\n");
	dm_cmd(DM_DEV_REMOVE, "vf0");
	dm_cmd(DM_DEV_CREATE, "vf0");
	rc = dm_load("vf0", "integrity", 2048, "/dev/ram0 0 32 R 0", 0);
	st("LOAD integrity R", rc);
	rc = dm_suspend("vf0", 0, 0);
	st("RESUME", rc);
	dm_cmd(DM_DEV_REMOVE, "vf0");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== DM_INTEGRITY MULTI-VARIANT POC ===\n");

	ctl_fd = open("/dev/mapper/control", O_RDWR);
	if (ctl_fd < 0) {
		printf("open control failed errno=%d\n", errno);
		return 1;
	}
	printf("control fd=%d\n", ctl_fd);

	variant_A();
	variant_B();
	variant_C();
	variant_D();
	variant_E();
	variant_F();

	printf("--- dmesg summary ---\n");
	dmesg_grep("integrity|device-mapper|general protection|Oops");

	printf("=== ALL VARIANTS DONE (no crash) ===\n");
	close(ctl_fd);
	return 0;
}