#!/bin/bash

# This is just a slightly more-debuggable script that does our travis build

set -ev

WITH_TESTS=1

case "$1" in
"--disable-tests")
        WITH_TESTS=0
        ;;
"")
        ;;
*)
        echo Usage: $0 "[--disable-tests]"
        exit 1
        ;;
esac

echo $TRAVIS_PULL_REQUEST

NPROCS=$(getconf _NPROCESSORS_ONLN)

echo "Found $NPROCS processors"
date

# Short-circuit transient 'auto-initialization' builds
git fetch origin master
MASTER=$(git describe --always FETCH_HEAD)
HEAD=$(git describe --always HEAD)
echo $MASTER
echo $HEAD
if [ $HEAD == $MASTER ]
then
    echo "HEAD SHA1 equals master; probably just establishing merge, exiting build early"
    exit 0
fi

# Try to ensure we're using the real g++ and clang++ versions we want
mkdir bin
which gcc-6
ln -s `which gcc-6` bin/gcc
which g++-6
ln -s `which g++-6` bin/g++
which clang-5.0
ln -s `which clang-5.0` bin/clang
which clang++-5.0
ln -s `which clang++-5.0` bin/clang++
which llvm-symbolizer-5.0
ln -s `which llvm-symbolizer-5.0` bin/llvm-symbolizer

export PATH=`pwd`/bin:$PATH
echo "PATH is $PATH"

hash -r

clang -v
which g++
g++ -v
llvm-symbolizer --version || true

if test $CXX = 'clang++'; then
    RUN_PARTITIONS=$(seq 0 $((NPROCS-1)))
elif test $CXX = 'g++'; then
    RUN_PARTITIONS=$(seq $NPROCS $((2*NPROCS-1)))
fi

config_flags="--enable-asan --enable-extrachecks --enable-ccache --enable-sdfprefs"
export CFLAGS="-O2 -g1"
export CXXFLAGS="-w -O2 -g1"

# disable leak detection: this requires the container to be run with
# "--cap-add SYS_PTRACE" or "--privileged"
# as the leak detector relies on ptrace
export LSAN_OPTIONS=detect_leaks=0

echo "config_flags = $config_flags"

#### ccache config
export CCACHE_COMPRESS=true
export CCACHE_COMPILERCHECK="string:$CXX"
export CCACHE_MAXSIZE=900M
export CCACHE_CPP2=true

ccache -p

ccache -s
date
time ./autogen.sh
time ./configure $config_flags
make format
d=`git diff | wc -l`
if [ $d -ne 0 ]
then
    echo "clang format must be run as part of the pull request, current diff:"
    git diff
    exit 1
fi

date
time make -j$(($NPROCS + 1))

ccache -s

if [ $WITH_TESTS -eq 0 ] ; then
    echo "Build done, skipping tests"
    exit 0
fi

# Create postgres databases
export PGUSER=postgres
psql -c "create database test;"
# we run NPROCS jobs in parallel
for j in $(seq 0 $((NPROCS-1))); do
    base_instance=$((j*50))
    for i in $(seq $base_instance $((base_instance+15))); do
        psql -c "create database test$i;"
    done
done

export ALL_VERSIONS=1
export TEMP_POSTGRES=0
export NUM_PARTITIONS=$((NPROCS*2))
export RUN_PARTITIONS
ulimit -n 256
time make check

echo All done
date
exit 0

