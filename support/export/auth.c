/*
 * utils/mountd/auth.c
 *
 * Authentication procedures for mountd.
 *
 * Copyright (C) 1995, 1996 Olaf Kirch <okir@monad.swb.de>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "sockaddr.h"
#include "misc.h"
#include "nfslib.h"
#include "exportfs.h"
#include "export.h"
#include "v4root.h"

enum auth_error
{
  bad_path,
  unknown_host,
  no_entry,
  not_exported,
  illegal_port,
  success
};

static void		auth_fixpath(char *path);
static nfs_export my_exp;
static nfs_client my_client;

extern int use_ipaddr;

/*
void
auth_init(void)
{
	auth_reload();
}
*/

/*
 * A client can match many different netgroups and it's tough to know
 * beforehand whether it will. If the concatenated string of netgroup
 * m_hostnames is >512 bytes, then enable the "use_ipaddr" mode. This
 * makes mountd change how it matches a client ip address when a mount
 * request comes in. It's more efficient at handling netgroups at the
 * expense of larger kernel caches.
 */
static void
check_useipaddr(void)
{
	nfs_client *clp;
	int old_use_ipaddr = use_ipaddr;
	unsigned int len = 0;

	if (use_ipaddr > 1)
		/* fixed - don't check */
		return;

	/* add length of m_hostname + 1 for the comma */
	for (clp = clientlist[MCL_NETGROUP]; clp; clp = clp->m_next)
		len += (strlen(clp->m_hostname) + 1);

	if (len > (NFSCLNT_IDMAX / 2))
		use_ipaddr = 1;
	else
		use_ipaddr = 0;

	if (use_ipaddr != old_use_ipaddr)
		cache_flush();
}

unsigned int
auth_reload(void)
{
	struct stat		stb;
	static ino_t		last_inode;
	static int		last_fd = -1;
	static unsigned int	counter;
	int			fd;

	if ((fd = open(etab.statefn, O_RDONLY)) < 0) {
		xlog(L_FATAL, "couldn't open %s", etab.statefn);
	} else if (fstat(fd, &stb) < 0) {
		xlog(L_FATAL, "couldn't stat %s", etab.statefn);
		close(fd);
	} else if (last_fd != -1 && stb.st_ino == last_inode) {
		/* We opened the etab file before, and its inode
		 * number hasn't changed since then.
		 */
		close(fd);
		return counter;
	} else {
		/* Need to process entries from the etab file.  Close
		 * the file descriptor from the previous open (last_fd),
		 * and keep the current file descriptor open to prevent
		 * the file system reusing the current inode number
		 * (last_inode).
		 */
		if (last_fd != -1)
			close(last_fd);
		last_fd = fd;
		last_inode = stb.st_ino;
	}

	export_freeall();
	memset(&my_client, 0, sizeof(my_client));
	xtab_export_read();
	check_useipaddr();
	v4root_set();

	++counter;

	return counter;
}

static char *get_client_ipaddr_name(const struct sockaddr *caller)
{
	char buf[INET6_ADDRSTRLEN + 1];

	buf[0] = '$';
	host_ntop(caller, buf + 1, sizeof(buf) - 1);
	return strdup(buf);
}

static char *
get_client_hostname(const struct sockaddr *caller, struct addrinfo *ai,
		enum auth_error *error)
{
	char *n;

	if (use_ipaddr)
		return get_client_ipaddr_name(caller);
	n = client_compose(ai);
	*error = unknown_host;
	if (!n)
		return NULL;
	if (*n)
		return n;
	free(n);
	return strdup("DEFAULT");
}

bool ipaddr_client_matches(nfs_export *exp, struct addrinfo *ai)
{
	return client_check(exp->m_client, ai);
}

bool namelist_client_matches(nfs_export *exp, char *dom)
{
	return client_member(dom, exp->m_client->m_hostname);
}

/*
 * client_matches() depends only on exp->m_client, but lookup_export()
 * calls it once per export.  A server that exports many paths to the same
 * set of clients therefore recomputes each answer once per path, and for a
 * netgroup client that answer can cost a reverse resolution.
 *
 * Cache the answer on the client for the length of one export walk, which
 * the walk brackets with client_match_begin() and client_match_end().
 * Outside a bracket the cache is inert, and that is the point:
 * auth_authenticate_newcache() answers MOUNT by walking the table with its
 * own (dom, ai), testing each entry's own m_client, so a live cache would
 * hand it verdicts computed for a different host.  Since client_matches()
 * decides authorisation, what matters is that the cache is unreachable
 * rather than that it saves work -- a caller added later that does not know
 * about the bracket loses the optimisation, not the verdict.
 *
 * mountd's workers are forked processes, not threads: cache_fork_workers()
 * forks, and both mountd and exportd start their workers through it.  The
 * only thread the process can hold is nfsd_path.c's chroot workqueue, which
 * exists only in a build with HAVE_SCHED_H && HAVE_LIBPTHREAD &&
 * HAVE_UNSHARE and only when "[exports] rootdir" is set, runs nothing but
 * the syscall closures in nfsd_path.c, and blocks its submitter for the
 * duration.  None of this state is touched from that thread, so it needs
 * no locking; cache.c's exp_fsid_lock guards a different thing
 * (e_fsid_value, added by DLPX-82097) and doesn't imply otherwise.
 *
 * The bracket does not nest.  client_match_active is a flag rather than a
 * depth, so an inner client_match_end() would deactivate the cache for a
 * still-running outer walk.  No caller nests today; one that needs to
 * should make this a depth counter rather than pair up the calls by hand.
 */
static uint64_t		client_match_gen;
static bool		client_match_active;

void client_match_begin(void)
{
	client_match_active = true;

	/*
	 * 0 is the calloc()ed "never evaluated" state; never reuse it.  The
	 * counter is 64 bits because a client keeps its stamp until it is
	 * next evaluated, and the prefilter in export_matches() can skip one
	 * for many consecutive walks, so a narrower counter could wrap back
	 * onto a stamp still in use.
	 */
	if (++client_match_gen == 0)
		client_match_gen = 1;
}

void client_match_end(void)
{
	client_match_active = false;
}

bool client_matches(nfs_export *exp, char *dom, struct addrinfo *ai)
{
	nfs_client *clp = exp->m_client;
	bool res;

	if (client_match_active && clp->m_match_gen == client_match_gen)
		return clp->m_match;

	if (is_ipaddr_client(dom))
		res = ipaddr_client_matches(exp, ai);
	else
		res = namelist_client_matches(exp, dom);

	if (client_match_active) {
		clp->m_match = res;
		clp->m_match_gen = client_match_gen;
	}

	return res;
}

/* return static nfs_export with details filled in */
static nfs_export *
auth_authenticate_newcache(const struct sockaddr *caller,
			   const char *path, struct addrinfo *ai,
			   enum auth_error *error)
{
	nfs_export *exp;
	int i;

	free(my_client.m_hostname);

	my_client.m_hostname = get_client_hostname(caller, ai, error);
	if (my_client.m_hostname == NULL)
		return NULL;

	my_client.m_naddr = 1;
	set_addrlist(&my_client, 0, caller);
	my_exp.m_client = &my_client;

	exp = NULL;
	for (i = 0; !exp && i < MCL_MAXTYPES; i++)
		for (exp = exportlist[i].p_head; exp; exp = exp->m_next) {
			if (strcmp(path, exp->m_export.e_path))
				continue;
			if (!client_matches(exp, my_client.m_hostname, ai))
				continue;
			if (exp->m_export.e_flags & NFSEXP_V4ROOT)
				/* not acceptable for v[23] export */
				continue;
			break;
		}
	*error = not_exported;
	if (!exp)
		return NULL;

	my_exp.m_export = exp->m_export;
	exp = &my_exp;
	return exp;
}

static nfs_export *
auth_authenticate_internal(const struct sockaddr *caller, const char *path,
		struct addrinfo *ai, enum auth_error *error)
{
	nfs_export *exp;

	exp = auth_authenticate_newcache(caller, path, ai, error);
	if (!exp)
		return NULL;
	if (!(exp->m_export.e_flags & NFSEXP_INSECURE_PORT) &&
		     nfs_get_port(caller) >= IPPORT_RESERVED) {
		*error = illegal_port;
		return NULL;
	}
	*error = success;

	return exp;
}

nfs_export *
auth_authenticate(const char *what, const struct sockaddr *caller,
		const char *path)
{
	nfs_export	*exp = NULL;
	char		epath[MAXPATHLEN+1];
	char		*p = NULL;
	char		buf[INET6_ADDRSTRLEN];
	struct addrinfo *ai = NULL;
	enum auth_error	error = bad_path;

	if (path[0] != '/') {
		xlog(L_WARNING, "Bad path in %s request from %s: \"%s\"",
			     what, host_ntop(caller, buf, sizeof(buf)), path);
		return exp;
	}

	strncpy(epath, path, sizeof (epath) - 1);
	epath[sizeof (epath) - 1] = '\0';
	auth_fixpath(epath); /* strip duplicate '/' etc */

	ai = client_resolve(caller);
	if (ai == NULL)
		return exp;

	/* Try the longest matching exported pathname. */
	while (1) {
		exp = auth_authenticate_internal(caller, epath, ai, &error);
		if (exp || (error != not_exported && error != no_entry))
			break;
		/* We have to treat the root, "/", specially. */
		if (p == &epath[1]) break;
		p = strrchr(epath, '/');
		if (p == epath) p++;
		*p = '\0';
	}

	host_ntop(caller, buf, sizeof(buf));
	switch (error) {
	case bad_path:
		xlog(L_WARNING, "bad path in %s request from %s: \"%s\"",
		     what, buf, path);
		break;

	case unknown_host:
		xlog(L_WARNING, "refused %s request from %s for %s (%s): unmatched host",
		     what, buf, path, epath);
		break;

	case no_entry:
		xlog(L_WARNING, "refused %s request from %s for %s (%s): no export entry",
		     what, buf, path, epath);
		break;

	case not_exported:
		xlog(L_WARNING, "refused %s request from %s for %s (%s): not exported",
		     what, buf, path, epath);
		break;

	case illegal_port:
		xlog(L_WARNING, "refused %s request from %s for %s (%s): illegal port %u",
		     what, buf, path, epath, nfs_get_port(caller));
		break;

	case success:
		xlog(L_NOTICE, "authenticated %s request from %s:%u for %s (%s)",
		     what, buf, nfs_get_port(caller), path, epath);
		break;
	default:
		xlog(L_NOTICE, "%s request from %s:%u for %s (%s) gave %d",
		     what, buf, nfs_get_port(caller), path, epath, error);
	}

	nfs_freeaddrinfo(ai);
	return exp;
}

static void
auth_fixpath(char *path)
{
	char	*sp, *cp;

	for (sp = cp = path; *sp; sp++) {
		if (*sp != '/' || sp[1] != '/')
			*cp++ = *sp;
	}
	while (cp > path+1 && cp[-1] == '/')
		cp--;
	*cp = '\0';
}
