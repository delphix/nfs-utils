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
#include <arpa/inet.h>

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

/*
 * client_matches() takes a second path when the caller is in "use_ipaddr"
 * mode (dom[0] == '$'): ipaddr_client_matches() -> client_check(), which
 * compares the caller's struct addrinfo against the client's own address
 * list instead of matching dom by name.  A deployment lands on this path
 * once use_ipaddr auto-enables (check_useipaddr(), auth.c), and it's also
 * the one MCL_WILDCARD and MCL_NETGROUP resolve through -- the case this
 * PR's client-before-path reordering optimises for.  Exercise it too,
 * rather than only the namelist path above.
 *
 * MCL_WILDCARD and MCL_NETGROUP themselves resolve through a reverse DNS
 * lookup (host_canonname()) and, for MCL_NETGROUP, innetgr(3) -- neither
 * hermetic in a unit test.  MCL_FQDN's client_check() is pure address
 * comparison (nfs_compare_sockaddr(), no DNS), so it drives the same
 * client_matches()/cache code on the ipaddr path without a network
 * dependency; the cache does not care which nfs_client type produced the
 * verdict it is holding.
 */
static void
init_fqdn_ai(struct addrinfo *ai, struct sockaddr_in *sin, uint32_t addr)
{
	memset(sin, 0, sizeof(*sin));
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(addr);

	memset(ai, 0, sizeof(*ai));
	ai->ai_addr = (struct sockaddr *)sin;
	ai->ai_addrlen = sizeof(*sin);
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

	/* The ip-address-keyed path: see init_fqdn_ai()'s comment above. */
	{
		nfs_client	ipaddr_client;
		nfs_export	ipaddr_export;
		struct addrinfo	client_addr_ai, match_ai, nomatch_ai;
		struct sockaddr_in client_addr_sin, match_sin, nomatch_sin;

		memset(&ipaddr_client, 0, sizeof(ipaddr_client));
		memset(&ipaddr_export, 0, sizeof(ipaddr_export));
		ipaddr_client.m_type = MCL_FQDN;
		ipaddr_client.m_hostname = (char *)"192.0.2.1";
		ipaddr_client.m_naddr = 1;
		init_fqdn_ai(&client_addr_ai, &client_addr_sin, 0xc0000201); /* 192.0.2.1 */
		set_addrlist(&ipaddr_client, 0, client_addr_ai.ai_addr);
		ipaddr_export.m_client = &ipaddr_client;

		init_fqdn_ai(&match_ai, &match_sin, 0xc0000201);     /* 192.0.2.1 */
		init_fqdn_ai(&nomatch_ai, &nomatch_sin, 0xc0000202); /* 192.0.2.2 */

		check("ipaddr path: matching address",
		      client_matches(&ipaddr_export, "$192.0.2.1", &match_ai), 1);
		check("ipaddr path: non-matching address",
		      client_matches(&ipaddr_export, "$192.0.2.1", &nomatch_ai), 0);

		client_match_begin();
		(void) client_matches(&ipaddr_export, "$192.0.2.1", &match_ai);
		ipaddr_client.m_match = false;	/* the predicate would say true */
		check("ipaddr path: in-walk repeat is served from the cache",
		      client_matches(&ipaddr_export, "$192.0.2.1", &match_ai), 0);
		client_match_end();

		check("ipaddr path: recomputed outside the walk",
		      client_matches(&ipaddr_export, "$192.0.2.1", &match_ai), 1);
	}

	if (failures) {
		printf("FAILED %d\n", failures);
		return 1;
	}
	printf("PASSED\n");
	return 0;
}
