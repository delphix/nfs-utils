#!/usr/bin/python3

'''NFS Apport Interface

Copyright (C) 2022 Canonical Ltd
Author: Andreas Hasenack <andreas@canonical.com>

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 2 of the License, or (at your
option) any later version.  See http://www.gnu.org/copyleft/gpl.html for
the full text of the license.
'''

from apport.hookutils import (
    attach_file_if_exists,
    command_output,
    recent_syslog,
)
from glob import glob
import re

def add_info(report, ui):
    file_list = ["/etc/nfs.conf","/etc/default/nfs-common", "/etc/default/nfs-kernel-server"]
    file_list.extend(glob("/etc/nfs.conf.d/*.conf"))
    file_list.append("/etc/exports")
    file_list.append("/etc/request-key.d/id_resolver.conf")
    for f in file_list:
        attach_file_if_exists(report, f)
    report["SyslogNFS"] = recent_syslog(re.compile("(rpc\.(nfsd|gssd|svcgssd|statd|mountd|idmapd)|blkmapd|nfsdcld|nfsidmap)\["))
    report["NFSMounts"] = command_output(["findmnt", "-n", "-t", "nfs"])
    report["NFSv4Mounts"] = command_output(["findmnt", "-n", "-t", "nfs4"])
