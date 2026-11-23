#!/bin/sh

if [ "$1" == "--help" -o "$1" == "help" ]; then
	echo ""
	echo "  USAGE:"
	echo ""
	echo "  # cd tcmu-runner/extra/"
	echo "  # ./make_runnerrpms.sh [--without (rbd|glfs|qcow|zbc|fbo|tcmalloc)]"
	echo ""
	echo "  Will build the RPMs in current dir by using the HEAD commit ID as default."
	echo ""
	exit
fi

TOPDIR=`pwd`/../

if [ ! -e $TOPDIR/.git ]; then
	echo ""
	echo "For now this will only support the git repo code."
	echo ""
	exit 1
fi

VERSION=`git describe --tags --match "v[0-9]*" 2>/dev/null`
if [ -z "$VERSION" ]; then
	# No release tags (e.g. in a fork): use the version from CMakeLists.txt
	# in the same format git describe would give: X.Y.Z.<commits>.g<hash>
	BASE_VERSION=`sed -n 's/^set(VERSION \([0-9.]*\))$/\1/p' $TOPDIR/CMakeLists.txt`
	if [ -z "$BASE_VERSION" ]; then
		echo "Could not determine the version from git tags or CMakeLists.txt"
		exit 1
	fi
	VERSION=$BASE_VERSION.`git rev-list --count HEAD`.g`git rev-parse --short HEAD`
fi
VERSION=`echo $VERSION | sed "s/-/./g"`
VERSION=`echo $VERSION | sed "s/v//"`
TCMURUNNER_TAR=tcmu-runner-$VERSION.tar.gz
rpmbuild_path=`pwd`/rpmbuild

# Try to clear the old rpmbuild data.
if [ -e $rpmbuild_path ]; then
	rm -rf $rpmbuild_path/*
fi

mkdir -p $rpmbuild_path/BUILD
mkdir -p $rpmbuild_path/SPECS
mkdir -p $rpmbuild_path/RPMS
mkdir -p $rpmbuild_path/SRPMS
mkdir -p $rpmbuild_path/SOURCES

cp ../tcmu-runner.spec $rpmbuild_path/SPECS/
SPEC=$rpmbuild_path/SPECS/tcmu-runner.spec

# Replace the Version
sed -i "s/Version:.*$/Version:       ${VERSION}/" $SPEC

# Delete all the _RC code if exists
LN=`grep -n "define" $SPEC |grep _RC | awk -F: '{print $1}'`
if [ ! -z "$LN" ]; then
	sed -i "${LN}d" $SPEC
fi
sed -i "s/%{?_RC:%{_RC}}/0/g" $SPEC
sed -i "s/%{?_RC:-%{_RC}}//g" $SPEC

# Generate the source package from the HEAD commit. git archive works in
# place, so unlike a local git clone it is not refused by the
# safe.directory check when the repo is owned by another user (CI
# containers).
PKG_NAME=tcmu-runner-$VERSION
if ! git -C $TOPDIR archive --format=tar.gz --prefix=$PKG_NAME/ \
		-o $rpmbuild_path/SOURCES/$TCMURUNNER_TAR HEAD; then
	echo "Failed to create the source tarball"
	exit 1
fi

# Build the RPMs
echo "Building RPMs with parameters: $@"
rpmbuild --define="_topdir $rpmbuild_path" -ba $rpmbuild_path/SPECS/tcmu-runner.spec "$@"

# Check if build was successful
if [ $? -eq 0 ]; then
    echo "RPM build completed successfully"
    echo "RPMs are located in: $rpmbuild_path/RPMS/"
    echo "SRPMs are located in: $rpmbuild_path/SRPMS/"
else
    echo "RPM build failed"
    exit 1
fi
