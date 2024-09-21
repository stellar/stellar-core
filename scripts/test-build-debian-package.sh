#!/bin/sh

# This is a file for testing the github.com/stellar/package
# debian packaging of stellar-core without running it through
# the full build pipeline. It's for debugging.

if [ ! -e stellar-core/autogen.sh ]
then
    echo run this from one level above the stellar-core/ directory
    exit 1
fi

if [ ! -e packages/stellar-core/debian ]
then
    echo run this with the packages/ directory checked out as well
    exit 1
fi

echo "info: installing script dependencies"
sudo apt install ubuntu-dev-tools pbuilder dh-make debootstrap devscripts dpkg-dev debhelper build-essential

export BUILD_VERSION=999
export DEBEMAIL=packages@stellar.org
export DEBFULLNAME='Package Maintainer'
export DISTRO=jammy

cd stellar-core
GIT_VERS=$(git describe --tag)
CORE_VERSION=${GIT_VERS%%-*}
CORE_VERSION=${CORE_VERSION#v}
cd ..

export FULL_VERSION=${CORE_VERSION}-${BUILD_VERSION}

echo "info: core version: ${CORE_VERSION} build version: ${BUILD_VERSION}"
echo "info: preparing code directory stellar-core-${CORE_VERSION}" 
rm -rf stellar-core-${CORE_VERSION}
cp -r stellar-core stellar-core-${CORE_VERSION}

echo "info: creating upstream tarball stellar-core-${CORE_VERSION}.tar.gz"
tar czf stellar-core-${CORE_VERSION}.tar.gz stellar-core-${CORE_VERSION}

echo 'info: copying packaging files to the code directory'
cp -rv packages/stellar-core/debian stellar-core-${CORE_VERSION}/

if [ ! -e stellar-core-${CORE_VERSION}/debian ]
then
    echo copy of debian files into stellar-core-${CORE_VERSION}/debian failed
    exit 1
fi

# These variables are used in src/Makefile.am
export LOCAL_PACKAGE=stellar-core
export LOCAL_VERSION=${CORE_VERSION}
export GIT_COMMIT=$(cd stellar-core-$CORE_VERSION && git rev-parse HEAD)

echo "info: updating changelog to match $FULL_VERSION"
dch --changelog stellar-core-$CORE_VERSION/debian/changelog --distribution ${DISTRO} --newversion="${FULL_VERSION}" "New stellar-core build"
cd stellar-core-$CORE_VERSION/
echo "info: creating upstream tarball, etc"
dh_make -ysf ../stellar-core-$CORE_VERSION.tar.gz 

if [ -f /var/cache/pbuilder/base-${DISTRO}.tgz ]; then
    echo "info: updating base-${DISTRO}.tgz"
    sudo /usr/bin/pbuilder-dist ${DISTRO} update --updates-only --basetgz /var/cache/pbuilder/base-${DISTRO}.tgz --debootstrapopts --variant=buildd
else
    echo "info: creating base-${DISTRO}.tgz"
    sudo /usr/bin/pbuilder-dist ${DISTRO} create --updates-only --basetgz /var/cache/pbuilder/base-${DISTRO}.tgz --debootstrapopts --variant=buildd
fi

# build the package
echo 'info: Starting package build'
/usr/bin/pdebuild --debbuildopts -b -- --basetgz /var/cache/pbuilder/base-${DISTRO}.tgz --distribution ${DISTRO} --use-network yes
