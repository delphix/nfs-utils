/*
 * support/export/rmtab.c
 *
 * Interface to the rmtab file.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

#include "misc.h"
#include "nfslib.h"
#include "exportfs.h"
#include "xio.h"
#include "xlog.h"

/*
 * See if the entry already exists.  If not,
 * this was an instantiated wild card, and we
 * must add it.
 */
static void
rmtab_read_wildcard(struct rmtabent *rep)
{
	nfs_export *exp, *exp2;
	struct addrinfo *ai;

	ai = host_addrinfo(rep->r_client);
	if (ai == NULL)
		return;

	exp = export_allowed(ai, rep->r_path);
	freeaddrinfo(ai);
	if (exp == NULL)
		return;

	exp2 = export_lookup(rep->r_client, exp->m_export.e_path, 0);
	if (exp2 == NULL) {
		struct exportent ee;

		memset(&ee, 0, sizeof(ee));
		dupexportent(&ee, &exp->m_export);

		ee.e_hostname = rep->r_client;
		exp2 = export_create(&ee, 0);
		exp2->m_changed = exp->m_changed;
	}
	exp2->m_mayexport = 1;
}

int
rmtab_read(void)
{
	struct rmtabent		*rep;

	setrmtabent("r");
	while ((rep = getrmtabent(1, NULL)) != NULL) {
		int			htype;

		htype = client_gettype(rep->r_client);
		if (htype == MCL_FQDN || htype == MCL_SUBNETWORK)
			rmtab_read_wildcard(rep);
	}

	if (errno == EINVAL) {
		/* Something goes wrong. We need to fix the rmtab
		   file. */
		int	lockid;
		FILE	*fp;
		if ((lockid = xflock(_PATH_RMTABLCK, "w")) < 0)
			return -1;
		rewindrmtabent();
		if (!(fp = fsetrmtabent(_PATH_RMTABTMP, "w"))) {
			endrmtabent ();
			xfunlock(lockid);
			return -1;
		}
		while ((rep = getrmtabent(0, NULL)) != NULL) {
			fputrmtabent(fp, rep, NULL);
		}
		if (rename(_PATH_RMTABTMP, _PATH_RMTAB) < 0) {
			xlog(L_ERROR, "couldn't rename %s to %s",
			     _PATH_RMTABTMP, _PATH_RMTAB);
		}
		endrmtabent();
		fendrmtabent(fp);
		xfunlock(lockid);
	}
	else {
		endrmtabent();
	}
	return 0;
}

/*
 * Check if an rmtab_path is exported given an export_path from etab file.
 * The path is considered exported if it is an exact match or is part of
 * a path nested under an export.
 */
static int
is_exported(const char* rmtab_path, const char* export_path) {
	/* first check that rmtab_path starts with the export_path */
	if (strstr(rmtab_path, export_path) != rmtab_path)
		return 0;

	/* then confirm that it is an exact match or that it's nested */
	char next = rmtab_path[strlen(export_path)];
	return next == '\0' || next == '/';
}

/*
 * The rmtab file can acculmulate stale entries over time. So when the list of
 * exports changes, make a best effort attempt to insure that every entry in
 * rmtab has a corresonding export. This code is based off of mountlist_del().
 */
void
rmtab_rebuild(void)
{
	struct rmtabent	*rep;
	struct stat	stb;
	FILE		*fp;
	int		rmtab_lockid, etab_lockid;
	int		entries_seen = 0, entries_copied = 0;
	struct exportent *xp;
	char		**e_path = NULL;
	int		i, len = 0;

	/* Hold locks for the rmtab and etab files during rebuilding */
	if ((rmtab_lockid = xflock(_PATH_RMTABLCK, "w")) < 0)
		return;

	/* fast path for no NFSv3 mounts */
	if (stat(_PATH_RMTAB, &stb) < 0 || stb.st_size == 0) {
		xfunlock(rmtab_lockid);
		return;
	}

	if ((etab_lockid = xflock(_PATH_ETABLCK, "r")) < 0) {
		xfunlock(rmtab_lockid);
		return;
	}

	if (!setrmtabent("r")) {
		xfunlock(rmtab_lockid);
		xfunlock(etab_lockid);
		return;
	}
	if (!(fp = fsetrmtabent(_PATH_RMTABTMP, "w"))) {
		endrmtabent();
		xfunlock(rmtab_lockid);
		xfunlock(etab_lockid);
		return;
	}

	/* build a list of exported paths */
	setexportent(_PATH_ETAB, "r");
	while ((xp = getexportent(0, 0)) != NULL) {
		if ((len % 8) == 0) {
			e_path = (char **) realloc(e_path,
			    (len + 8) * sizeof(*e_path));
		}
		e_path[len++] = strdup(xp->e_path);
	}
	endexportent();

	/* Visit all the entries in rmtab file */
	while ((rep = getrmtabent(1, NULL)) != NULL) {
		int saved = 0;

		/* filter entries that are not exported */
		for (int i = 0; i < len; i++) {
			if (is_exported(rep->r_path, e_path[i])) {
				fputrmtabent(fp, rep, NULL);
				entries_copied++;
				saved = 1;
				break;
			}
		}
		entries_seen++;

		if (!saved) {
			/* log this stale cleanup to the syslog file */
			xlog_syslog(1);
			xlog(L_NOTICE, "purged stale rmtab entry '%s:%s'",
			    rep->r_client, rep->r_path);
			xlog_syslog(0);
		}
	}
	endrmtabent();
	fendrmtabent(fp);

	if (entries_seen != entries_copied) {
		if (rename(_PATH_RMTABTMP, _PATH_RMTAB) < 0) {
			xlog(L_ERROR, "couldn't rename %s to %s",
					_PATH_RMTABTMP, _PATH_RMTAB);
		}
	} else {
		unlink(_PATH_RMTABTMP);
	}
	xfunlock(rmtab_lockid);
	xfunlock(etab_lockid);

	/* cleanup list of exported paths */
	for (i = 0; i < len; i++)
		free(e_path[i]);
	if (e_path)
		free(e_path);
}
