## Process this file with automake to produce Makefile.in

check_PROGRAMS = statdb_dump t-client-match
statdb_dump_SOURCES = statdb_dump.c

statdb_dump_LDADD = ../support/nfs/.libs/libnfs.a \
		    ../support/nsm/libnsm.a \
		    ../support/misc/libmisc.a $(LIBCAP)

t_client_match_SOURCES = t-client-match.c
t_client_match_CPPFLAGS = $(AM_CPPFLAGS) $(CPPFLAGS) \
			  -I$(top_builddir)/support/include \
			  -I$(top_srcdir)/support/export
t_client_match_LDADD = ../support/export/libexport.a \
		       ../support/nfs/libnfs.la \
		       ../support/misc/libmisc.a \
		       ../support/reexport/libreexport.a \
		       $(LIBBSD) $(LIBWRAP) $(LIBNSL) $(LIBBLKID) -luuid \
		       $(LIBTIRPC) $(LIBPTHREAD)

SUBDIRS = nsm_client

MAINTAINERCLEANFILES = Makefile.in

TESTS = t0001-statd-basic-mon-unmon.sh t-client-match
EXTRA_DIST = test-lib.sh t0001-statd-basic-mon-unmon.sh
