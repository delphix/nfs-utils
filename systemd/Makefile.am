## Process this file with automake to produce Makefile.in

MAINTAINERCLEANFILES = Makefile.in

unit_files =  \
    nfs-client.target \
    \
    nfs-config.service \
    nfs-mountd.service \
    nfs-server.service \
    nfs-utils.service \
    rpc-statd-notify.service \
    rpc-statd.service \
    \
    proc-fs-nfsd.mount \
    var-lib-nfs-rpc_pipefs.mount

if CONFIG_NFSV4
unit_files += \
    nfs-idmapd.service
endif

if CONFIG_NFSV41
unit_files += \
    nfs-blkmap.service
endif

if CONFIG_GSS
unit_files += \
    auth-rpcgss-module.service \
    rpc-gssd.service

if CONFIG_SVCGSS
unit_files += \
    rpc-svcgssd.service
endif
endif

EXTRA_DIST = $(unit_files)

unit_dir = /lib/systemd/system
generator_dir = /lib/systemd/system-generators

EXTRA_PROGRAMS	= nfs-server-generator
genexecdir = $(generator_dir)
nfs_server_generator_LDADD = ../support/export/libexport.a \
			     ../support/nfs/libnfs.a \
			     ../support/misc/libmisc.a

if INSTALL_SYSTEMD
genexec_PROGRAMS = nfs-server-generator
install-data-hook: $(unit_files)
	mkdir -p $(DESTDIR)/$(unitdir)
	cp $(unit_files) $(DESTDIR)/$(unitdir)
else
noinst_PROGRAMS = nfs-server-generator
endif
