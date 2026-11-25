#!/bin/bash
#
# Install the RPM build dependencies in an EL8/EL9/EL10 container
# (Rocky Linux, AlmaLinux, CentOS Stream). Used by the GitHub Actions and
# GitLab CI pipelines; must run as root.

set -e

. /etc/os-release
EL=${VERSION_ID%%.*}

# Mirrors are sometimes caught in the middle of a sync, with metadata
# that refers to packages not uploaded yet. Retry with fresh metadata.
dnf_install() {
	local i

	for i in 1 2 3; do
		if dnf install -y "$@"; then
			return 0
		fi
		echo "dnf install failed (attempt $i/3), retrying with fresh metadata" >&2
		dnf clean metadata
		sleep 15
	done
	return 1
}

dnf_install dnf-plugins-core epel-release
case "$EL" in
8)
	# CentOS 8 SIG repositories are end of life and break dnf;
	# glusterfs-api-devel and librbd1-devel ship in AppStream.
	dnf config-manager --set-enabled powertools
	;;
9)
	# glusterfs-api-devel is only available from the CentOS Storage SIG.
	dnf config-manager --set-enabled crb
	dnf_install centos-release-gluster11
	;;
*)
	# There is no CentOS Storage SIG Gluster release for EL10 yet,
	# so the glfs handler cannot be built there.
	dnf config-manager --set-enabled crb
	;;
esac

GLUSTER=
if [ "$EL" -lt 10 ]; then
	GLUSTER=glusterfs-api-devel
fi

# librbd-devel and librados-devel (Ceph 16.2 on EL9, 18.2 on EL10) come
# from CRB: no Ceph SIG release package, whose rebuilds by the
# distributions have been broken (AlmaLinux 10).
dnf_install git rpm-build rpmdevtools cmake make gcc \
	libnl3-devel glib2-devel zlib-devel kmod-devel gperftools-devel \
	librbd1-devel librados2-devel $GLUSTER
