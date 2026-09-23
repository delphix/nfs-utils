/*
 * Unit test for the per-client match cache used by lookup_export().
 *
 * client_matches() depends only on exp->m_client, so its answer is cached
 * on the client for the length of one export walk, which the walk brackets
 * with client_match_begin() and client_match_end().
 *
 * The assertions come in two kinds.  Most pin *safety*: outside a bracket
 * the cache is neither read nor written, so a caller with a client of its
 * own cannot inherit a verdict computed for another host -- which is what
 * decides MOUNT authorisation in auth_authenticate_newcache().  The last
 * two pin that the cache is actually *used*, so that a change which
 * quietly disabled it would fail here rather than only showing up as a
 * performance regression on a test rig.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nfslib.h"
#include "exportfs.h"
#include "export.h"

/*
 * libexport.a expects its host daemon to define this; rpc.mountd and
 * exportd both do.  The name-list path exercised below never reads it,
 * but the linker still needs it resolved.
 */
int use_ipaddr = -1;

static int failures;

static void
check(const char *what, int got, int want)
{
	if (got == want) {
		printf("ok - %s\n", what);
	} else {
		printf("not ok - %s (got %d, want %d)\n", what, got, want);
		failures++;
	}
}

/*
 * A name-list client: client_matches() falls to namelist_client_matches(),
 * which asks whether m_hostname appears in the comma-separated dom.  No
 * addrinfo is consulted on this path, so ai may be NULL.
 */
static nfs_client	test_client;
static nfs_export	test_export;

static void
init_client(nfs_client *clp, nfs_export *exp, const char *hostname)
{
	memset(clp, 0, sizeof(*clp));
	memset(exp, 0, sizeof(*exp));
	clp->m_hostname = (char *)hostname;
	clp->m_type = MCL_FQDN;
	exp->m_client = clp;
}

static void
setup(const char *hostname)
{
	init_client(&test_client, &test_export, hostname);
}

int
main(void)
{
	setup("alpha");

	/*
	 * Before any walk has been begun the cache is inert, so each call is
	 * answered on its own terms.
	 */
	check("answers correctly with no walk in progress",
	      client_matches(&test_export, "alpha,beta", NULL), 1);
	check("and for a dom that does not list the client",
	      client_matches(&test_export, "delta", NULL), 0);

	/*
	 * The shape that matters.  auth_authenticate_newcache() decides MOUNT
	 * authorisation by walking the export table with its own (dom, ai),
	 * calling client_matches() on each entry's own m_client rather than
	 * on the client it composed for the caller, and it runs in the same
	 * process as the cache upcalls.  So it can be reached immediately
	 * after a walk has stamped every client in the table with a verdict
	 * for a different host, and an active cache would hand it that
	 * verdict on every same-path entry.  It must not inherit one.
	 */
	client_match_begin();
	check("walk for alpha matches alpha",
	      client_matches(&test_export, "alpha", NULL), 1);
	client_match_end();
	check("caller outside the walk does not inherit the verdict",
	      client_matches(&test_export, "beta", NULL), 0);

	/* Nor can a caller outside a walk leave a verdict behind. */
	check("caller outside the walk did not poison the cache",
	      client_matches(&test_export, "alpha", NULL), 1);

	/* A later walk derives afresh rather than reusing the last one. */
	client_match_begin();
	check("a new walk re-derives",
	      client_matches(&test_export, "gamma", NULL), 0);
	client_match_end();

	client_match_begin();
	check("and is not sticky in one direction",
	      client_matches(&test_export, "alpha", NULL), 1);
	client_match_end();

	/*
	 * Two clients in one generation must not share a verdict.  This is
	 * the failure that matters: a cache keyed on anything coarser than
	 * the client would answer for "delta" with "alpha"'s result.
	 */
	{
		nfs_client other_client;
		nfs_export other_export;

		init_client(&other_client, &other_export, "delta");

		client_match_begin();
		check("first client matches",
		      client_matches(&test_export, "alpha", NULL), 1);
		check("second client does not inherit the first client's verdict",
		      client_matches(&other_export, "alpha", NULL), 0);
		client_match_end();
	}

	/*
	 * Two exports sharing one client resolve the client once and agree.
	 */
	{
		nfs_export shared_export;

		memset(&shared_export, 0, sizeof(shared_export));
		shared_export.m_client = &test_client;

		client_match_begin();
		check("shared client, first export",
		      client_matches(&test_export, "alpha", NULL), 1);
		check("shared client, second export agrees",
		      client_matches(&shared_export, "alpha", NULL), 1);
		client_match_end();
	}

	/*
	 * Finally, prove the cache is reached at all.  Every assertion above
	 * still passes if the cache is disabled outright, so without these
	 * two a change that quietly turned it off would look clean here and
	 * only show up as a performance regression on a rig.
	 *
	 * Inside a walk, poke a verdict that disagrees with what the
	 * predicate would compute and require the poked value back; outside
	 * one, require the predicate's own answer.
	 */
	{
		setup("alpha");

		client_match_begin();
		(void) client_matches(&test_export, "alpha", NULL);
		test_client.m_match = false;	/* the predicate would say true */
		check("an in-walk repeat is served from the cache",
		      client_matches(&test_export, "alpha", NULL), 0);
		client_match_end();

		check("the same call outside the walk is recomputed",
		      client_matches(&test_export, "alpha", NULL), 1);
	}

	if (failures) {
		printf("FAILED %d\n", failures);
		return 1;
	}
	printf("PASSED\n");
	return 0;
}
