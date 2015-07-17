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
ln -s `which gcc-4.9` bin/gcc
ln -s `which g++-4.9` bin/g++
ln -s `which clang-3.5` bin/clang
ln -s `which clang++-3.5` bin/clang++

export PATH=`pwd`/bin:$PATH
hash -r
clang -v
g++ -v
llvm-symbolizer --version || true

# Create postgres databases
export PGUSER=postgres
psql -c "create database test;"
for i in $(seq 0 8)
do
    psql -c "create database test$i;"
done

ccache -s
./autogen.sh
./configure --enable-asan --enable-ccache --enable-sdfprefs
make
ccache -s
make check
