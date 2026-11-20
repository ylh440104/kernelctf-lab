#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/dm-ioctl.h>

#define DEV "dsb0"
#define BACKDEV "/dev/ram1"
#define SB_BUF (8 * 512)

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
static unsigned long long g_sb_off;

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

static int dm_ctl(int cmd)
{
	static char buf[4096];
	struct dm_ioctl *dmi = (struct dm_ioctl *)buf;
	memset(buf, 0, sizeof(buf));
	snprintf(dmi->name, sizeof(dmi->name), "%s", DEV);
	if (cmd == DM_DEV_CREATE)
		dmi->flags = DM_PERSISTENT_DEV_FLAG;
	return dm_ioctl(cmd, dmi, sizeof(buf));
}

static int dm_load(unsigned long long len, const char *spec)
{
	static char buf[8192];
	struct dm_ioctl *dmi = (struct dm_ioctl *)buf;
	struct dm_target_spec *s;
	char *p;
	size_t sl;
	memset(buf, 0, sizeof(buf));
	snprintf(dmi->name, sizeof(dmi->name), "%s", DEV);
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

static int dm_suspend(int do_suspend)
{
	static char buf[4096];
	struct dm_ioctl *dmi = (struct dm_ioctl *)buf;
	memset(buf, 0, sizeof(buf));
	snprintf(dmi->name, sizeof(dmi->name), "%s", DEV);
	dmi->flags = do_suspend ? DM_SUSPEND_FLAG : 0;
	return dm_ioctl(DM_DEV_SUSPEND, dmi, sizeof(buf));
}

static int sb_io(unsigned char *buf, int wr)
{
	int fd, n;
	fd = open(BACKDEV, wr ? O_RDWR : O_RDONLY);
	if (fd < 0)
		return -1;
	if (lseek(fd, (off_t)g_sb_off, SEEK_SET) < 0) {
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
	return (n == SB_BUF) ? 0 : -1;
}

#define F_VERSION	1
#define F_FLAGS		2
#define F_RECALC	3
#define F_PDS		4
#define F_LBPBB		5
#define F_TAG		6
#define F_JOURNAL	7
#define F_INTERLEAVE	8

static int apply_mut(struct dm_sb *sb, int field, unsigned long long val)
{
	switch (field) {
	case F_VERSION:
		sb->version = (unsigned char)val;
		break;
	case F_FLAGS:
		sb->flags = (unsigned int)val;
		break;
	case F_RECALC:
		sb->recalc_sector = val;
		break;
	case F_PDS:
		sb->provided_data_sectors = val;
		break;
	case F_LBPBB:
		sb->log2_blocks_per_bitmap_bit = (unsigned char)val;
		break;
	case F_TAG:
		sb->integrity_tag_size = (unsigned short)val;
		break;
	case F_JOURNAL:
		sb->journal_sections = (unsigned int)val;
		break;
	case F_INTERLEAVE:
		sb->log2_interleave_sectors = (unsigned char)val;
		break;
	default:
		return -1;
	}
	return 0;
}

static int run_one(int idx, const char *tag, unsigned long long len,
		   const char *spec, unsigned long long start,
		   int field, unsigned long long val)
{
	unsigned char buf[SB_BUF];
	struct dm_sb *sb = (struct dm_sb *)buf;
	int rc;

	printf("\n[CASE %d] %s\n", idx, tag);
	fflush(stdout);

	g_sb_off = start * 512;

	dm_ctl(DM_DEV_REMOVE);
	dm_ctl(DM_DEV_CREATE);

	rc = dm_load(len, spec);
	if (rc) {
		printf("  LOAD failed rc=%d, skip\n", rc);
		fflush(stdout);
		dm_ctl(DM_DEV_REMOVE);
		return -1;
	}

	rc = dm_suspend(0);
	if (rc) {
		printf("  resume#1 failed rc=%d, skip\n", rc);
		fflush(stdout);
		dm_ctl(DM_DEV_REMOVE);
		return -1;
	}

	dm_suspend(1);

	if (sb_io(buf, 0) != 0) {
		printf("  read sb failed\n");
		fflush(stdout);
		dm_suspend(0);
		dm_ctl(DM_DEV_REMOVE);
		return -1;
	}
	if (memcmp(sb->magic, "integrt", 7) != 0) {
		printf("  bad magic, skip\n");
		fflush(stdout);
		dm_suspend(0);
		dm_ctl(DM_DEV_REMOVE);
		return -1;
	}

	if (apply_mut(sb, field, val) != 0) {
		printf("  bad field\n");
		dm_suspend(0);
		dm_ctl(DM_DEV_REMOVE);
		return -1;
	}

	printf("  mutating -> resume...\n");
	fflush(stdout);

	if (sb_io(buf, 1) != 0) {
		printf("  write sb failed\n");
		dm_ctl(DM_DEV_REMOVE);
		return -1;
	}

	rc = dm_suspend(0);
	printf("  resume#2 rc=%d\n", rc);
	fflush(stdout);

	dm_ctl(DM_DEV_REMOVE);
	return 0;
}

int main(void)
{
	int n = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== DM-INTEGRITY SUPERBLOCK FIELD FUZZ ===\n");

	ctl_fd = open("/dev/mapper/control", O_RDWR);
	if (ctl_fd < 0) {
		printf("open control errno=%d\n", errno);
		return 1;
	}
	printf("control fd=%d\n", ctl_fd);

	{
		int fd = open(BACKDEV, O_RDWR);
		char z[4096];
		int i;
		if (fd >= 0) {
			memset(z, 0, sizeof(z));
			for (i = 0; i < 512; i++)
				if (write(fd, z, sizeof(z)) < 0)
					break;
			close(fd);
			printf("zeroed %s\n", BACKDEV);
		} else {
			printf("open %s failed errno=%d\n", BACKDEV, errno);
			return 1;
		}
	}

	{
		const char *J = "/dev/ram1 8 4 J 1 journal_sectors:8";
		const char *JH = "/dev/ram1 8 - J 1 internal_hash:sha256";
		const char *D = "/dev/ram1 8 4 D 0";
		const char *R = "/dev/ram1 8 4 R 0";

		n++; run_one(n, "J nohash flags=DIRTY_BITMAP", 256, J, 8, F_FLAGS, 0x4);
		n++; run_one(n, "J nohash flags=0xfffffffd", 256, J, 8, F_FLAGS, 0xfffffffdULL);
		n++; run_one(n, "J nohash version=0", 256, J, 8, F_VERSION, 0);
		n++; run_one(n, "J nohash version=255", 256, J, 8, F_VERSION, 255);
		n++; run_one(n, "J nohash lbpbb=0", 256, J, 8, F_LBPBB, 0);
		n++; run_one(n, "J nohash lbpbb=31", 256, J, 8, F_LBPBB, 31);
		n++; run_one(n, "J nohash lbpbb=63", 256, J, 8, F_LBPBB, 63);
		n++; run_one(n, "J nohash pds=0", 256, J, 8, F_PDS, 0);
		n++; run_one(n, "J nohash pds=max", 256, J, 8, F_PDS, ~0ULL);
		n++; run_one(n, "J nohash recalc=max", 256, J, 8, F_RECALC, ~0ULL);
		n++; run_one(n, "J nohash recalc=100", 256, J, 8, F_RECALC, 100);
		n++; run_one(n, "J nohash tag=0", 256, J, 8, F_TAG, 0);
		n++; run_one(n, "J nohash tag=65535", 256, J, 8, F_TAG, 65535);
		n++; run_one(n, "J nohash journal=0", 256, J, 8, F_JOURNAL, 0);
		n++; run_one(n, "J nohash journal=0xffffffff", 256, J, 8, F_JOURNAL, 0xffffffffULL);
		n++; run_one(n, "J nohash interleave=0", 256, J, 8, F_INTERLEAVE, 0);
		n++; run_one(n, "J nohash interleave=31", 256, J, 8, F_INTERLEAVE, 31);
		n++; run_one(n, "J nohash interleave=63", 256, J, 8, F_INTERLEAVE, 63);

		n++; run_one(n, "JH flags=DIRTY_BITMAP", 256, JH, 8, F_FLAGS, 0x4);
		n++; run_one(n, "JH flags=0xfffffffd", 256, JH, 8, F_FLAGS, 0xfffffffdULL);
		n++; run_one(n, "JH lbpbb=0", 256, JH, 8, F_LBPBB, 0);
		n++; run_one(n, "JH lbpbb=63", 256, JH, 8, F_LBPBB, 63);
		n++; run_one(n, "JH recalc=0 flags=RECALC", 256, JH, 8, F_RECALC, 0);

		n++; run_one(n, "D nohash flags=DIRTY_BITMAP", 256, D, 8, F_FLAGS, 0x4);
		n++; run_one(n, "D nohash flags=0xfffffffd", 256, D, 8, F_FLAGS, 0xfffffffdULL);
		n++; run_one(n, "D nohash lbpbb=63", 256, D, 8, F_LBPBB, 63);
		n++; run_one(n, "D nohash version=255", 256, D, 8, F_VERSION, 255);

		n++; run_one(n, "R nohash flags=0xfffffffd", 256, R, 8, F_FLAGS, 0xfffffffdULL);
		n++; run_one(n, "R nohash lbpbb=63", 256, R, 8, F_LBPBB, 63);
	}

	printf("\n=== SB FUZZ DONE (%d cases) ===\n", n);
	close(ctl_fd);
	return 0;
}