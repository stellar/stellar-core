#!/bin/bash

# this script performs a build & test pass
# it depends on the CC and CXX environment variables

set -ev

# max age of cache before force purging
CACHE_MAX_DAYS=30

WITH_TESTS=1
export TEMP_POSTGRES=0

PROTOCOL_CONFIG=""

while [[ -n "$1" ]]; do
    COMMAND="$1"
    shift

    case "${COMMAND}" in
    "--disable-tests")
            WITH_TESTS=0
            echo Disabling tests
            ;;
    "--use-temp-db")
            export TEMP_POSTGRES=1
            echo Using temp database
            ;;
    "--protocol")
            PROTOCOL="$1"
            shift
            echo Testing with protocol $PROTOCOL
            case "${PROTOCOL}" in
            "current")
                ;;
            "next")
                PROTOCOL_CONFIG="--enable-next-protocol-version-unsafe-for-production"
                ;;
            *)
                echo Unknown protocol ${PROTOCOL}
                exit 1
                ;;
            esac
            ;;
    "")
            ;;
    *)
            echo Unknown parameter ${COMMAND}
            echo Usage: $0 "[--disable-tests][--use-temp-db]"
            exit 1
            ;;
    esac

done

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
    exit 1
fi

# Try to ensure we're using the real g++ and clang++ versions we want
mkdir bin

export PATH=`pwd`/bin:$PATH
echo "PATH is $PATH"
hash -r

if test $CXX = 'clang++'; then
    RUN_PARTITIONS=$(seq 0 $((NPROCS-1)))
    which clang-10
    ln -s `which clang-10` bin/clang
    which clang++-10
    ln -s `which clang++-10` bin/clang++
    which llvm-symbolizer-10
    ln -s `which llvm-symbolizer-10` bin/llvm-symbolizer
    clang -v
    llvm-symbolizer --version || true
elif test $CXX = 'g++'; then
    RUN_PARTITIONS=$(seq $NPROCS $((2*NPROCS-1)))
    which gcc-8
    ln -s `which gcc-8` bin/gcc
    which g++-8
    ln -s `which g++-8` bin/g++
    which g++
    g++ -v
fi

config_flags="--enable-asan --enable-extrachecks --enable-ccache --enable-sdfprefs ${PROTOCOL_CONFIG}"
export CFLAGS="-O2 -g1"
export CXXFLAGS="-w -O2 -g1"

# quarantine_size_mb / malloc_context_size : reduce memory usage to avoid
# crashing in tests that churn a lot of memory
# disable leak detection: this requires the container to be run with
# "--cap-add SYS_PTRACE" or "--privileged"
# as the leak detector relies on ptrace
export ASAN_OPTIONS="quarantine_size_mb=100:malloc_context_size=4:detect_leaks=0"

echo "config_flags = $config_flags"

#### ccache config
export CCACHE_DIR=$HOME/.ccache
export CCACHE_COMPRESS=true
export CCACHE_COMPRESSLEVEL=9
# cache size should be large enough for a full build
export CCACHE_MAXSIZE=500M
export CCACHE_CPP2=true

# purge cache if it's too old
if [ -d "$CCACHE_DIR" ] ; then
    if [ -n "$(find $CCACHE_DIR -mtime +$CACHE_MAX_DAYS -print -quit)" ] ; then
        echo Purging old cache $CCACHE_DIR
        rm -rf $CCACHE_DIR
    fi
fi

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

if [ $TEMP_POSTGRES -eq 0 ] ; then
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
fi

export ALL_VERSIONS=1
export NUM_PARTITIONS=$((NPROCS*2))
export RUN_PARTITIONS
ulimit -n 256
time make check

echo All done
date
exit 0
