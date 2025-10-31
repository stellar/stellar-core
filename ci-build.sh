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
    "--disable-postgres")
            export DISABLE_POSTGRES='--disable-postgres'
            echo Disabling postgres
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
            echo Usage: $0 "[--disable-tests][--use-temp-db][--disable-postgres]"
            exit 1
            ;;
    esac

done

NPROCS=$(getconf _NPROCESSORS_ONLN)

echo "Found $NPROCS processors"
date

SRC_DIR=$(pwd)

mkdir -p "build-${CC}-${PROTOCOL}"
cd "build-${CC}-${PROTOCOL}"

# Check to see if we _just_ tested this rev in
# a merge queue, and if so don't bother doing
# it again. Wastes billable CPU time.
if [ -e prev-pass-rev ]
   PREV_REV=$(cat prev-pass-rev)
   CURR_REV=$(git rev-parse HEAD)
   if [ "${PREV_REV}" = "${CURR_REV}" ]
   then
       exit 0
   fi
   rm prev-pass-rev
fi


# restore source file mtimes based on content hashes
if which mtime-travel >/dev/null 2>&1; then
    for DIR in src lib
    do
	if [ -e mtimes-${DIR}.json ]
	then
	    mtime-travel restore -f mtimes-${DIR}.json ${SRC_DIR}/${DIR}
	fi
	rm -f mtimes-${DIR}.json
	mtime-travel save -f mtimes-${DIR}.json ${SRC_DIR}/${DIR}
    done
fi

# Try to ensure we're using the real g++ and clang++ versions we want
mkdir -p bin

export PATH=`pwd`/bin:$PATH
echo "PATH is $PATH"
hash -r

if test $CXX = 'clang++'; then
    RUN_PARTITIONS=$(seq 0 $((NPROCS-1)))
    # Use CLANG_VERSION environment variable if set, otherwise default to 12
    CLANG_VER=${CLANG_VERSION:-12}
    if test ${CLANG_VER} != 'none'; then
	which clang-${CLANG_VER}
	ln -sf `which clang-${CLANG_VER}` bin/clang
	which clang++-${CLANG_VER}
	ln -sf `which clang++-${CLANG_VER}` bin/clang++
	which llvm-symbolizer-${CLANG_VER}
	ln -sf `which llvm-symbolizer-${CLANG_VER}` bin/llvm-symbolizer
	clang -v
	llvm-symbolizer --version || true
    fi
elif test $CXX = 'g++'; then
    RUN_PARTITIONS=$(seq $NPROCS $((2*NPROCS-1)))
    which gcc-10
    ln -sf `which gcc-10` bin/gcc
    which g++-10
    ln -sf `which g++-10` bin/g++
    which g++
    g++ -v
fi

config_flags="--enable-asan --enable-extrachecks --enable-ccache --enable-sdfprefs --enable-threadsafety ${PROTOCOL_CONFIG} ${DISABLE_POSTGRES}"
export CFLAGS="-O2 -g1 -fno-omit-frame-pointer -fsanitize-address-use-after-scope -fno-common"
export CXXFLAGS="$CFLAGS"

# quarantine_size_mb / malloc_context_size : reduce memory usage to avoid
# crashing in tests that churn a lot of memory
# disable leak detection: this requires the container to be run with
# "--cap-add SYS_PTRACE" or "--privileged"
# as the leak detector relies on ptrace
export ASAN_OPTIONS="quarantine_size_mb=100:malloc_context_size=4:detect_leaks=0:strict_string_checks=1:detect_stack_use_after_return=1:check_initialization_order=1:strict_init_order=1:log_path=stdout"

echo "config_flags = $config_flags"

#### ccache config
export CCACHE_DIR=$(pwd)/.ccache
export CCACHE_COMPRESS=true
export CCACHE_COMPRESSLEVEL=9
# cache size should be large enough for a full build
export CCACHE_MAXSIZE=800M
export CCACHE_CPP2=true

# periodically check to see if caches are old and purge them if so
if [ -d "$CCACHE_DIR" ] ; then
    if [ -n "$(find $CCACHE_DIR -mtime +$CACHE_MAX_DAYS -print -quit)" ] ; then
        echo Purging old cache dirs $CCACHE_DIR ./target $HOME/.cargo/registry $HOME/.cargo/git
        rm -rf $CCACHE_DIR ./target $HOME/.cargo/registry $HOME/.cargo/git
    fi
fi

ccache -p
ccache -s
ccache -z
date
time (cd "${SRC_DIR}" && ./autogen.sh)
time "${SRC_DIR}/configure" $config_flags
if [ -z "${SKIP_FORMAT_CHECK}" ]; then
    make format
    d=`git diff | wc -l`
    if [ $d -ne 0 ]
    then
        echo "clang format must be run as part of the pull request, current diff:"
        git diff
        exit 1
    fi
fi

crlf=$(find . ! \( -type d -o -path './.git/*' -o -path './Builds/*' -o -path './lib/*' \) -print0 | xargs -0 -n1 -P9 file "{}" | grep CRLF || true)
if [ -n "$crlf" ]
then
    echo "Found some files with Windows line endings:"
    echo "$crlf"
    exit 1
fi

date
time make -j$(($NPROCS - 1))

ccache -s
### incrementally purge old content from target directory
(cd "${SRC_DIR}" && CARGO_TARGET_DIR="build-${CC}-${PROTOCOL}/target" cargo sweep --maxsize 800MB)

if [ $WITH_TESTS -eq 0 ] ; then
    echo "Build done, skipping tests"
    exit 0
fi

if [ $DISABLE_POSTGRES != '--disable-postgres' ] ; then
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
fi

export ALL_VERSIONS=1
export NUM_PARTITIONS=$((NPROCS*2))
export RUN_PARTITIONS
export RND_SEED=$(($(date +%s) / 86400))  # Convert to days since epoch
echo "Using RND_SEED: $RND_SEED"
ulimit -n 4096
time make check

echo Running fixed check-test-tx-meta tests
export TEST_SPEC='[tx]'
export STELLAR_CORE_TEST_PARAMS="--ll fatal -r simple --all-versions --rng-seed 12345 --check-test-tx-meta ${SRC_DIR}/test-tx-meta-baseline-${PROTOCOL}"
export SKIP_SOROBAN_TESTS=true
time make check

echo All done
date

git rev-parse HEAD >prev-pass-rev
exit 0
