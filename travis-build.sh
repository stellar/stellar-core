#!/bin/bash

# This is just a slightly more-debuggable script that does our travis build

set -ev

echo $TRAVIS_PULL_REQUEST

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
ln -s `which gcc-5` bin/gcc
ln -s `which g++-5` bin/g++
ln -s `which clang-5.0` bin/clang
ln -s `which clang++-5.0` bin/clang++
ln -s `which llvm-symbolizer-5.0` bin/llvm-symbolizer

export PATH=`pwd`/bin:$PATH
hash -r
clang -v
g++ -v
llvm-symbolizer --version || true

# Create postgres databases
if test $CXX = 'clang++'; then
    RUN_PARTITIONS="0 1"
elif test $CXX = 'g++'; then
    RUN_PARTITIONS="2 3"
fi
export PGUSER=postgres
psql -c "create database test;"
for j in $RUN_PARTITIONS; do
    base_instance=$((j*50))
    for i in $(seq $base_instance $((base_instance+15))); do
        psql -c "create database test$i;"
    done
done

committer_of(){
    local c=$(git cat-file -p "$1" 2> /dev/null \
	| sed -ne '/^committer \([^<]*[^ <]\)  *<.*>.*/{s//\1/p; q;}')
    test -n "$c" -a Latobarita != "$c" && echo "$c"
}
committer=$(committer_of HEAD) \
    || committer=$(committer_of HEAD^2) \
    || committer=$(committer_of HEAD^1) \
    || committer=Latobarita

case $committer in
    "David Mazieres")
        config_flags="--enable-asan --enable-ccache CXXFLAGS=-w"
	;;
    *)
	config_flags="--enable-asan --enable-ccache --enable-sdfprefs CXXFLAGS=-w"
	;;
esac

echo "committer = $committer, config_flags = $config_flags"

ccache -s
./autogen.sh
./configure $config_flags
make format
d=`git diff | wc -l`
if [ $d -ne 0 ]
then
    echo "clang format must be run as part of the pull request, current diff:"
    git diff
    exit 1
fi
make -j3
ccache -s
export ALL_VERSIONS=1
env TEMP_POSTGRES=0 NUM_PARTITIONS=4 RUN_PARTITIONS="$RUN_PARTITIONS" make check
