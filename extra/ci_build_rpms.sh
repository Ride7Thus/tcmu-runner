#!/bin/bash
#
# Build the RPMs in CI.
#
# USAGE: extra/ci_build_rpms.sh [full|rbd|glfs|qcow|zbc|fbo|tcmalloc]
#
# "full" (the default) builds every handler available on the
# distribution, any other value builds without that feature. The RPMs
# end up in extra/rpmbuild/{RPMS,SRPMS}.

set -e

VARIANT=${1:-full}
TOPDIR=$(cd "$(dirname "$0")/.." && pwd)

. /etc/os-release
EL=${VERSION_ID%%.*}

# CI checkouts are often owned by another user than the one running the
# build, which makes git refuse to work in them.
git config --global --add safe.directory "$TOPDIR"

ARGS=
if [ "$VARIANT" != full ]; then
	ARGS="--without $VARIANT"
fi
# No Gluster packages are available for EL10, see ci_install_deps.sh.
if [ "$EL" -ge 10 ] && [ "$VARIANT" != glfs ]; then
	ARGS="$ARGS --without glfs"
fi

cd "$TOPDIR/extra"
./make_runnerrpms.sh $ARGS
