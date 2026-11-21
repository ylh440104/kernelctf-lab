#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/dm-ioctl.h>

#define DEV "dba0"
#define BACKDEV "/dev/ram1"
#define SB_BUF (8 * 512)

struct dm_sb {
	unsigned char magic[8];
	unsigned char version;
	unsigned char log2_interleave;
	unsigned short tag_size;
	unsigned int journal_sections;
	unsigned long long provided_data_sectors;
	unsigned int flags;
	unsigned char log2_spb;
	unsigned char log2_bpbb;
	unsigned char pad[2];
	unsigned long long recalc_sector;
	unsigned char pad2[8];
	unsigned char salt[16];
};

#define SB_FLAG_RECALCULATING	0x2u
#define SB_FLAG_DIRTY_BITMAP	0x4u

static int ctl_fd = -1;
static unsigned long long g_off;

static int dmio(int cmd, struct dm_ioctl *d, size_t len)
{
	d->version[0] = 4; d->version[1] = 0; d->version[2] = 0;
	d->data_size = len;
	d->data_start = sizeof(struct dm_ioctl);
	if (ioctl(ctl_fd, cmd, d) < 0) return -errno;
	return 0;
}

static int ctl(int cmd)
{
	static char b[4096];
	struct dm_ioctl *d = (struct dm_ioctl *)b;
	memset(b, 0, sizeof(b));
	snprintf(d->name, sizeof(d->name), "%s", DEV);
	return dmio(cmd, d, sizeof(b));
}

static int load(unsigned long long len, const char *spec)
{
	static char b[8192];
	struct dm_ioctl *d = (struct dm_ioctl *)b;
	struct dm_target_spec *s;
	char *p;
	size_t sl;
	int rr;
	memset(b, 0, sizeof(b));
	snprintf(d->name, sizeof(d->name), "%s", DEV);
	d->target_count = 1;
	s = (struct dm_target_spec *)(b + sizeof(struct dm_ioctl));
	s->sector_start = 0;
	s->length = len;
	snprintf(s->target_type, sizeof(s->target_type), "integrity");
	p = (char *)s + sizeof(struct dm_target_spec);
	strcpy(p, spec);
	sl = (sizeof(struct dm_target_spec) + strlen(spec) + 1 + 7) & ~7UL;
	rr = dmio(DM_TABLE_LOAD, d, sizeof(struct dm_ioctl) + sl);
	if (rr) printf("  load err=%d\n", rr);
	return rr;
}

static int susp(int on)
{
	static char b[4096];
	struct dm_ioctl *d = (struct dm_ioctl *)b;
	memset(b, 0, sizeof(b));
	snprintf(d->name, sizeof(d->name), "%s", DEV);
	d->flags = on ? DM_SUSPEND_FLAG : 0;
	return dmio(DM_DEV_SUSPEND, d, sizeof(b));
}

static int sbio(unsigned char *b, int wr)
{
	int fd, n;
	fd = open(BACKDEV, wr ? O_RDWR : O_RDONLY);
	if (fd < 0) return -1;
	if (lseek(fd, (off_t)g_off, SEEK_SET) < 0) { close(fd); return -1; }
	n = wr ? write(fd, b, SB_BUF) : read(fd, b, SB_BUF);
	if (wr) fsync(fd);
	close(fd);
	return n == SB_BUF ? 0 : -1;
}

static void wipe(void)
{
	int fd = open(BACKDEV, O_RDWR);
	char z[4096];
	int i;
	if (fd < 0) return;
	memset(z, 0, sizeof(z));
	if (lseek(fd, 0, SEEK_SET) == 0)
		for (i = 0; i < 512; i++)
			if (write(fd, z, sizeof(z)) < 0) break;
	fsync(fd);
	close(fd);
}

static int setup(unsigned long long len, const char *spec)
{
	ctl(DM_DEV_REMOVE);
	wipe();
	ctl(DM_DEV_CREATE);
	if (load(len, spec)) { ctl(DM_DEV_REMOVE); return -1; }
	if (susp(0))         { printf("  resume#1 fail\n"); ctl(DM_DEV_REMOVE); return -1; }
	return 0;
}

int main(void)
{
	unsigned char buf[SB_BUF];
	struct dm_sb *sb = (struct dm_sb *)buf;

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== DM-INTEGRITY recalc_sector ALIGNMENT TEST ===\n");

	ctl_fd = open("/dev/mapper/control", O_RDWR);
	if (ctl_fd < 0) { printf("ctl errno=%d\n", errno); return 1; }
	g_off = 8 * 512;

	printf("\n[A] mode B + internal_hash, clear DIRTY, set RECALC, recalc_sector=9\n");
	if (setup(256, "/dev/ram1 8 32 B 3 block_size:4096 sectors_per_bit:8 bitmap_flush_interval:100") == 0) {
		susp(1);
		if (sbio(buf, 0) == 0 && memcmp(sb->magic, "integrt", 7) == 0) {
			printf("  before: pds=%llu log2spb=%u lbpbb=%u flags=0x%x recalc=%llu\n",
			       sb->provided_data_sectors, sb->log2_spb, sb->log2_bpbb, sb->flags,
			       sb->recalc_sector);
			sb->flags &= ~SB_FLAG_DIRTY_BITMAP;
			sb->flags |= SB_FLAG_RECALCULATING;
			sb->recalc_sector = 9;
			sbio(buf, 1);
			printf("  resume -> expect BUG in block_bitmap_op\n");
			printf("  rc=%d\n", susp(0));
		} else {
			printf("  sb read/magic fail\n");
		}
		ctl(DM_DEV_REMOVE);
	}

	printf("\n[B] mode B + internal_hash, keep DIRTY, recalc_sector=9\n");
	if (setup(256, "/dev/ram1 8 32 B 3 block_size:4096 sectors_per_bit:8 bitmap_flush_interval:100") == 0) {
		susp(1);
		if (sbio(buf, 0) == 0 && memcmp(sb->magic, "integrt", 7) == 0) {
			printf("  before: flags=0x%x recalc=%llu\n", sb->flags, sb->recalc_sector);
			sb->flags |= SB_FLAG_RECALCULATING;
			sb->recalc_sector = 9;
			sbio(buf, 1);
			printf("  rc=%d\n", susp(0));
		} else {
			printf("  sb read/magic fail\n");
		}
		ctl(DM_DEV_REMOVE);
	}

	printf("\n[C] mode B + internal_hash, recalc_sector=5\n");
	if (setup(256, "/dev/ram1 8 32 B 3 block_size:4096 sectors_per_bit:8 bitmap_flush_interval:100") == 0) {
		susp(1);
		if (sbio(buf, 0) == 0 && memcmp(sb->magic, "integrt", 7) == 0) {
			sb->flags &= ~SB_FLAG_DIRTY_BITMAP;
			sb->flags |= SB_FLAG_RECALCULATING;
			sb->recalc_sector = 5;
			sbio(buf, 1);
			printf("  rc=%d\n", susp(0));
		} else {
			printf("  sb read/magic fail\n");
		}
		ctl(DM_DEV_REMOVE);
	}

	printf("\n=== DONE ===\n");
	close(ctl_fd);
	return 0;
}