#!/bin/bash
#
# Install the RPM build dependencies in an EL8/EL9/EL10 container
# (Rocky Linux, AlmaLinux, CentOS Stream). Used by the GitHub Actions and
# GitLab CI pipelines; must run as root.

set -e

. /etc/os-release
EL=${VERSION_ID%%.*}

dnf install -y dnf-plugins-core epel-release
case "$EL" in
8)
	# CentOS 8 SIG repositories are end of life and break dnf;
	# glusterfs-api-devel and librbd1-devel ship in AppStream.
	dnf config-manager --set-enabled powertools
	;;
9)
	dnf config-manager --set-enabled crb
	dnf install -y centos-release-gluster11 centos-release-ceph-squid
	;;
*)
	# There is no CentOS Storage SIG Gluster release for EL10 yet,
	# so the glfs handler cannot be built there.
	dnf config-manager --set-enabled crb
	dnf install -y centos-release-ceph-squid
	;;
esac

GLUSTER=
if [ "$EL" -lt 10 ]; then
	GLUSTER=glusterfs-api-devel
fi

dnf install -y git rpm-build rpmdevtools cmake make gcc \
	libnl3-devel glib2-devel zlib-devel kmod-devel gperftools-devel \
	librbd1-devel librados2-devel $GLUSTER
