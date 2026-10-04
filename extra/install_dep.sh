#!/bin/bash
#
# Userspace side of the LIO TCM-User backstore
#
# For now only fedora, rhel and centos systems are supported

if test $(id -u) != 0 ; then
	SUDO=sudo
fi

if [ y`uname`y = yLinuxy ]; then
	source /etc/os-release
	case $ID in
	fedora|rhel|centos)
		# for generic
		$SUDO dnf install -y cmake make gcc
		$SUDO dnf install -y libnl3
		$SUDO dnf install -y libnl3-devel
		$SUDO dnf install -y glib2
		$SUDO dnf install -y glib2-devel
	        $SUDO dnf install -y zlib
		$SUDO dnf install -y zlib-devel
		$SUDO dnf install -y kmod
		$SUDO dnf install -y kmod-devel
		$SUDO dnf install -y gperftools-devel

		# for glusterfs
		$SUDO dnf install -y glusterfs-api
		$SUDO dnf install -y glusterfs-api-devel
		# for ceph
		$SUDO dnf install -y librados2
		$SUDO dnf install -y librados2-devel
		$SUDO dnf install -y librbd1
		# Check if librbd-devel is available with different naming convention
		if ! $SUDO dnf list available | grep -q "librbd-devel"; then
			LIBRBD=librbd1
		else
			LIBRBD=librbd-devel
		fi
	        $SUDO dnf install -y $LIBRBD-devel
		;;
	debian)
		# Update APT cache
		$SUDO apt update
		
		# for generic
		$SUDO apt install -y cmake make gcc
		$SUDO apt install -y zlib1g kmod
		$SUDO apt install -y libnl-3-dev
		$SUDO apt install -y libnl-genl-3-dev
		$SUDO apt install -y libglib2.0-0
		$SUDO apt install -y libkmod-dev
		$SUDO apt install -y libgoogle-perftools-dev
		
		# for glusterfs
		$SUDO apt install -y libglusterfs-dev
		
		# for ceph
		$SUDO apt install -y librados2
		$SUDO apt install -y librbd-dev
		;;
	sles|opensuse-tumbleweed)
		# for generic
		$SUDO zypper install -y cmake make gcc
		$SUDO zypper install -y libnl3-200
		$SUDO zypper install -y glib2
		$SUDO zypper install -y zlib
		$SUDO zypper install -y kmod
		$SUDO zypper install -y libnl3-devel
		$SUDO zypper install -y glib2-devel
		$SUDO zypper install -y zlib-devel
		$SUDO zypper install -y libkmod-devel
		$SUDO zypper install -y gperftools-devel

		#for glusterfs
		$SUDO zypper install -y glusterfs
		$SUDO zypper install -y glusterfs-devel
		#for ceph
		$SUDO zypper install -y librbd-devel
		$SUDO zypper install -y librados-devel
		$SUDO zypper install -y librados2
		;;
	*)
		echo "TODO: distro not supported for now!"
		exit 1
		;;
	esac
else
	echo "TODO: only Linux is supported for now!"
	exit 1
fi

echo "Dependencies installed successfully"