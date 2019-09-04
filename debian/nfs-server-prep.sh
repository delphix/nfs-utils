#!/bin/bash
#
# Copyright 2019 Delphix
#

#
# Called by the nfs-server.service unit before the server is started
#

#
# DLPX-64900
# Work around for kernel failing to parse the 'versions' input string
# "-4.1 -4.2 -2 +3 +4" that is passed from rpc.nfsd after a reboot
#
# Prime the kernel version after a reboot to include +4 so that the -4.x works
#
# We need this work-around until Ubuntu picks up nfs-utils 2.3.3
#
versions=$(cat /proc/fs/nfsd/versions)
if [[ "$versions" == "-2 -3 -4 -4.0 -4.1 -4.2" ]]; then
	echo "Initializing the NFS server version"
	printf '+4\n' >/proc/fs/nfsd/versions
fi

#
# DLPX-65853
# Due to a permissions error, where '/var/lib/nfs' is owned by statd, the
# database used by nfsdcltrack(8) is never initialized.  This database is
# necessary for the NFSv4 server to track clients and notify them when the
# server is restarted.
#
# The work around is to create the directory that holds the client tracking
# database upfront to avoid the mkdir failure during the nfsdcltrack init.
#
NFSD_CL_TRACK_DIR="/var/lib/nfs/nfsdcltrack"
if [[ ! -d "$NFSD_CL_TRACK_DIR" ]]; then
	echo Creating dir for fsdcltrack database
	mkdir "$NFSD_CL_TRACK_DIR"
fi

exit 0
