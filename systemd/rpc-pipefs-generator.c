/*
 * rpc-pipefs-generator:
 *   systemd generator to create ordering dependencies between
 *   nfs services and the rpc_pipefs mountpoint
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <mntent.h>

#include "nfslib.h"
#include "conffile.h"
#include "systemd.h"

#define RPC_PIPEFS_DEFAULT NFS_STATEDIR "/rpc_pipefs"

static int generate_mount_unit(const char *pipefs_path, const char *pipefs_unit,
			       const char *dirname)
{
	char	*path;
	FILE	*f;
	size_t size = (strlen(dirname) + 1 + strlen(pipefs_unit) + 1);

	path = malloc(size);
	if (!path)
		return 1;
	snprintf(path, size, "%s/%s", dirname, pipefs_unit);
	f = fopen(path, "w");
	if (!f)
	{
		free(path);
		return 1;
	}

	fprintf(f, "# Automatically generated by rpc-pipefs-generator\n\n[Unit]\n");
	fprintf(f, "Description=RPC Pipe File System\n");
	fprintf(f, "DefaultDependencies=no\n");
	fprintf(f, "After=systemd-tmpfiles-setup.service\n");
	fprintf(f, "Conflicts=umount.target\n");
	fprintf(f, "\n[Mount]\n");
	fprintf(f, "What=sunrpc\n");
	fprintf(f, "Where=%s\n", pipefs_path);
	fprintf(f, "Type=rpc_pipefs\n");

	fclose(f);
	free(path);
	return 0;
}

static
int generate_target(char *pipefs_path, const char *dirname)
{
	char	*path;
	char	filebase[] = "/rpc_pipefs.target";
	char	*pipefs_unit;
	FILE	*f;
	int 	ret = 0;

	pipefs_unit = systemd_escape(pipefs_path, ".mount");
	if (!pipefs_unit)
		return 1;

	ret = generate_mount_unit(pipefs_path, pipefs_unit, dirname);
	if (ret) {
		free(pipefs_unit);
		return ret;
	}

	path = malloc(strlen(dirname) + 1 + sizeof(filebase));
	if (!path) {
		free(pipefs_unit);
		return 2;
	}
	sprintf(path, "%s", dirname);
	mkdir(path, 0755);
	strcat(path, filebase);
	f = fopen(path, "w");
	if (!f)
	{
		free(path);
		free(pipefs_unit);
		return 1;
	}

	fprintf(f, "# Automatically generated by rpc-pipefs-generator\n\n[Unit]\n");
	fprintf(f, "Requires=%s\n", pipefs_unit);
	fprintf(f, "After=%s\n", pipefs_unit);
	fclose(f);
	free(path);
	free(pipefs_unit);

	return 0;
}

static int is_non_pipefs_mountpoint(char *path)
{
	FILE		*mtab;
	struct mntent	*mnt;

	mtab = setmntent("/etc/mtab", "r");
	if (!mtab)
		return 0;

	while ((mnt = getmntent(mtab)) != NULL) {
		if (strlen(mnt->mnt_dir) != strlen(path))
			continue;
		if (strncmp(mnt->mnt_dir, path, strlen(mnt->mnt_dir)))
			continue;
		if (strncmp(mnt->mnt_type, "rpc_pipefs", strlen(mnt->mnt_type)))
			break;
	}
	fclose(mtab);
	return mnt != NULL;
}

int main(int argc, char *argv[])
{
	int 	ret;
	char	*s;

	/* Avoid using any external services */
	xlog_syslog(0);

	if (argc != 4 || argv[1][0] != '/') {
		fprintf(stderr, "rpc-pipefs-generator: create systemd dependencies for nfs services\n");
		fprintf(stderr, "Usage: normal-dir early-dir late-dir\n");
		exit(1);
	}

	conf_init_file(NFS_CONFFILE);
	s = conf_get_str("general", "pipefs-directory");
	if (!s)
		exit(0);

	if (is_non_pipefs_mountpoint(s))
		exit(1);

	ret = generate_target(s, argv[1]);
	exit(ret);
}
