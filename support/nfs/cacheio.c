/*
 * support/nfs/cacheio.c
 * support IO on the cache channel files in 2.5 and beyond.
 * These use 'qwords' which are like words, but with a little quoting.
 *
 */


/*
 * Support routines for text-based upcalls.
 * Fields are separated by spaces.
 * Fields are either mangled to quote space tab newline slosh with slosh
 * or a hexified with a leading \x
 * Record is terminated with newline.
 *
 */

#include <nfslib.h>
#include <stdio.h>
#include <stdio_ext.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

void qword_add(char **bpp, int *lp, char *str)
{
	char *bp = *bpp;
	int len = *lp;
	char c;

	if (len < 0) return;

	while ((c=*str++) && len)
		switch(c) {
		case ' ':
		case '\t':
		case '\n':
		case '\\':
			if (len >= 4) {
				*bp++ = '\\';
				*bp++ = '0' + ((c & 0300)>>6);
				*bp++ = '0' + ((c & 0070)>>3);
				*bp++ = '0' + ((c & 0007)>>0);
			}
			len -= 4;
			break;
		default:
			*bp++ = c;
			len--;
		}
	if (c || len <1) len = -1;
	else {
		*bp++ = ' ';
		len--;
	}
	*bpp = bp;
	*lp = len;
}

void qword_addhex(char **bpp, int *lp, char *buf, int blen)
{
	char *bp = *bpp;
	int len = *lp;

	if (len < 0) return;

	if (len > 2) {
		*bp++ = '\\';
		*bp++ = 'x';
		len -= 2;
		while (blen && len >= 2) {
			unsigned char c = *buf++;
			*bp++ = '0' + ((c&0xf0)>>4) + (c>=0xa0)*('a'-'9'-1);
			*bp++ = '0' + (c&0x0f) + ((c&0x0f)>=0x0a)*('a'-'9'-1);
			len -= 2;
			blen--;
		}
	}
	if (blen || len<1) len = -1;
	else {
		*bp++ = ' ';
		len--;
	}
	*bpp = bp;
	*lp = len;
}

void qword_addint(char **bpp, int *lp, int n)
{
	int len;

	len = snprintf(*bpp, *lp, "%d ", n);
	if (len > *lp)
		len = *lp;
	*bpp += len;
	*lp -= len;
}

void qword_adduint(char **bpp, int *lp, unsigned int n)
{
	int len;

	len = snprintf(*bpp, *lp, "%u ", n);
	if (len > *lp)
		len = *lp;
	*bpp += len;
	*lp -= len;
}

void qword_addeol(char **bpp, int *lp)
{
	if (*lp <= 0)
		return;
	**bpp = '\n';
	(*bpp)++;
	(*lp)--;
}

#define isodigit(c) (isdigit(c) && c <= '7')
int qword_get(char **bpp, char *dest, int bufsize)
{
	/* return bytes copied, or -1 on error */
	char *bp = *bpp;
	int len = 0;

	while (*bp == ' ') bp++;

	if (bp[0] == '\\' && bp[1] == 'x') {
		/* HEX STRING */
		bp += 2;
		while (isxdigit(bp[0]) && isxdigit(bp[1]) && len < bufsize) {
			int byte = isdigit(*bp) ? *bp-'0' : toupper(*bp)-'A'+10;
			bp++;
			byte <<= 4;
			byte |= isdigit(*bp) ? *bp-'0' : toupper(*bp)-'A'+10;
			*dest++ = byte;
			bp++;
			len++;
		}
	} else {
		/* text with \nnn octal quoting */
		while (*bp != ' ' && *bp != '\n' && *bp && len < bufsize-1) {
			if (*bp == '\\' &&
			    isodigit(bp[1]) && (bp[1] <= '3') &&
			    isodigit(bp[2]) &&
			    isodigit(bp[3])) {
				int byte = (*++bp -'0');
				bp++;
				byte = (byte << 3) | (*bp++ - '0');
				byte = (byte << 3) | (*bp++ - '0');
				*dest++ = byte;
				len++;
			} else {
				*dest++ = *bp++;
				len++;
			}
		}
	}

	if (*bp != ' ' && *bp != '\n' && *bp != '\0')
		return -1;
	while (*bp == ' ') bp++;
	*bpp = bp;
	*dest = '\0';
	return len;
}

int qword_get_int(char **bpp, int *anint)
{
	char buf[50];
	char *ep;
	int rv;
	int len = qword_get(bpp, buf, 50);
	if (len < 0) return -1;
	if (len ==0) return -1;
	rv = strtol(buf, &ep, 0);
	if (*ep) return -1;
	*anint = rv;
	return 0;
}

int qword_get_uint(char **bpp, unsigned int *anint)
{
	char buf[50];
	char *ep;
	unsigned int rv;
	int len = qword_get(bpp, buf, 50);
	if (len < 0) return -1;
	if (len ==0) return -1;
	rv = strtoul(buf, &ep, 0);
	if (*ep) return -1;
	*anint = rv;
	return 0;
}

/* Check if we should use the new caching interface
 * This succeeds iff the "nfsd" filesystem is mounted on
 * /proc/fs/nfs
 */
int
check_new_cache(void)
{
	return	(access("/proc/fs/nfs/filehandle", F_OK) == 0) ||
		(access("/proc/fs/nfsd/filehandle", F_OK) == 0);
}	


/* flush the kNFSd caches.
 * Set the flush time to the mtime of _PATH_ETAB or
 * if force, to now.
 * the caches to flush are:
 *  auth.unix.ip nfsd.export nfsd.fh
 */

void
cache_flush(int force)
{
	struct stat stb;
	int c;
	char stime[20];
	char path[200];
	time_t now;
	/* Note: the order of these caches is important.
	 * They need to be flushed in dependancy order. So
	 * a cache that references items in another cache,
	 * as nfsd.fh entries reference items in nfsd.export,
	 * must be flushed before the cache that it references.
	 */
	static char *cachelist[] = {
		"auth.unix.ip",
		"auth.unix.gid",
		"nfsd.fh",
		"nfsd.export",
		NULL
	};
	now = time(0);
	if (force ||
	    stat(_PATH_ETAB, &stb) != 0 ||
	    stb.st_mtime > now)
		stb.st_mtime = time(0);
	
	sprintf(stime, "%ld\n", stb.st_mtime);
	for (c=0; cachelist[c]; c++) {
		int fd;
		sprintf(path, "/proc/net/rpc/%s/flush", cachelist[c]);
		fd = open(path, O_RDWR);
		if (fd >= 0) {
			if (write(fd, stime, strlen(stime)) != (ssize_t)strlen(stime)) {
				xlog_warn("Writing to '%s' failed: errno %d (%s)",
				path, errno, strerror(errno));
			}
			close(fd);
		}
	}
}

/*
 * Update the expiration of an existing entry in cache so that it gets evicted.
 * cache is either 'nfsd.fh' or 'nfsd.export'
 */
static void
cache_update_entry(const char *cache, const char *message)
{
	char path[MAXPATHLEN];
	int fd;
	ssize_t mesg_len = (ssize_t)strlen(message);

	snprintf(path, sizeof(path), "/proc/net/rpc/%s/channel", cache);
	fd = open(path, O_RDWR);
	if (fd >= 0) {
		if (write(fd, message, mesg_len) != mesg_len) {
			xlog_warn("Writing '%s' to '%s' failed: errno %d (%s)",
			message, path, errno, strerror(errno));
		}
		close(fd);
	}
}

/*
 * Check if there is an entry in cache for mountpoint.
 * cache is either 'nfsd.fh' or 'nfsd.export'
 */
static bool
cache_has_entry(const char *cache, const char *mountpoint)
{
	char path[MAXPATHLEN];
	bool found = false;
	FILE *fp;

	snprintf(path, sizeof(path), "/proc/net/rpc/%s/content", cache);
	fp = fopen(path, "r");
	if (fp != NULL) {
		char *linebuf = NULL;
		size_t linelen = 0;

		while(getline(&linebuf, &linelen, fp) != -1) {
			if (linebuf[0] == '#')
				continue;
			if (strstr(linebuf, mountpoint) != NULL) {
				found = true;
				break;
			}
		}
		free(linebuf);
		(void) fclose(fp);
	}

	return found;
}

#define MILLISEC        1000
#define NANOSEC         1000000000
#define NSEC2MSEC(n)    ((n) / (NANOSEC / MILLISEC))

static uint64_t
gethrtime(void)
{
	struct timespec ts;
	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((((uint64_t)ts.tv_sec) * NANOSEC) + ts.tv_nsec);
}

/*
 * Check if mount is busy without causing an actual unmount.
 */
static bool
can_umount(const char *path)
{
	struct statfs64 stfs;
	int error = 0;

	/*
	 * We leverage the umount2 MNT_EXPIRE mechanism to let us know if
	 * a mount is busy without causing an actual unmount.  With the
	 * MNT_EXPIRE flag we will get back EBUSY or EAGAIN (mount is not
	 * busy) but not cause an actual unmount.
	 *
	 * After checking the result we make a statfs call to clear the
	 * MNT_EXPIRE state (i.e. the mnt_expiry_mark).
	 */
	if (umount2(path, MNT_EXPIRE) == -1)
		error = errno;

	/*
	 * Undo the mount point expired state by taking a temporary reference.
	 *
	 * NOTE -- another exportfs -F <path> could theoretically race us here
	 * but that only means that the other instance could have unmounted the
	 * path. Regardless, cache_flush_entry() can still proceed.
	 */
	(void) statfs64(path, &stfs);

	return (error != EBUSY);
}

/*
 * Flush the NFSD 'nfsd.fh' and 'nfsd.export' cache entries for exported path
 *
 * Used by ZFS libshare to flush a specific export.
 */
void
cache_flush_entry(const char *path)
{
	struct statfs64 stfs;
	char fsid_val[36];
	char message[200];
	int expiry;
	int rc;

	/* Note - sunrpc cache_clean() will expire if less than now */
	expiry = time(0) - 10;

	if (cache_has_entry("nfsd.fh", path)) {
		/* Obtain the FSID for the 'nfsd.fh' message */
		rc = statfs64(path, &stfs);
		if (rc != 0) {
			xlog(L_FATAL, "Cannot obtain the FSID for '%s' - %s",
			    path, strerror(errno));
		}
		snprintf(fsid_val, sizeof(fsid_val), "%08x%08x0000000000000000",
		    stfs.f_fsid.__val[0], stfs.f_fsid.__val[1]);

		/* message format: <client> <fsidtype> <fsid> <expiry> <path> */
		snprintf(message, sizeof(message),
		    "* 6 \\x%s %d %s\n", fsid_val, expiry, path);

		cache_update_entry("nfsd.fh", message);
	}

	if (cache_has_entry("nfsd.export", path)) {
		/* message format: <client> <path> <expiry> */
		snprintf(message, sizeof(message), "* %s %d\n", path, expiry);
		cache_update_entry("nfsd.export", message);
	}

	/*
	 * Wait here (up to 10 seconds) for nfsd_filecache_laundrette thread
	 * to flush entries holding a reference on this mount.
	 */
	uint64_t start = gethrtime();
	do {
		if (can_umount(path))
			break;
		usleep(100 * MILLISEC);
	} while (NSEC2MSEC(gethrtime() - start) < (10 * MILLISEC));
}
