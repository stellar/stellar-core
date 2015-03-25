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

# Set up packages
sudo apt-get -y purge clang clang-3.4
sudo apt-get -y autoremove
wget -O - http://llvm.org/apt/llvm-snapshot.gpg.key | sudo apt-key add -
sudo apt-get update -qq
sudo apt-get install -qq python-software-properties
sudo add-apt-repository -y 'deb http://llvm.org/apt/precise/ llvm-toolchain-precise-3.5 main'
sudo add-apt-repository -y 'ppa:ubuntu-toolchain-r/test'
sudo apt-get update -qq
sudo apt-get install -qq autoconf automake libtool pkg-config flex bison clang-3.5 llvm-3.5 g++-4.9 libstdc++6 libpq5 libpq-dev
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-4.9 90 --slave /usr/bin/g++ g++ /usr/bin/g++-4.9
sudo update-alternatives --install /usr/bin/clang clang /usr/bin/clang-3.5 90 --slave /usr/bin/clang++ clang++ /usr/bin/clang++-3.5
sudo update-alternatives --install /usr/bin/llvm-symbolizer llvm-symbolizer /usr/bin/llvm-symbolizer-3.5 90
sudo rm -Rf /usr/local/clang*
hash -r
clang -v
g++ -v

# Create postgres databases
psql -c "create user test with password 'test';" -U postgres
psql -c "create database test;" -U postgres
for i in $(seq 0 8)
do
    psql -c "create database test$i;" -U postgres
done

# customize CC and CXX for asan, ccache
if [ $CC == clang ]
then
    export CXXFLAGS='-fsanitize=address -fno-omit-frame-pointer -g'
    export CFLAGS='-fsanitize=address -fno-omit-frame-pointer -g'
    export CC='ccache clang -Qunused-arguments -fcolor-diagnostics'
    export CXX='ccache clang++ -Qunused-arguments -fcolor-diagnostics'
else
    export CXXFLAGS='-fsanitize=address -fno-omit-frame-pointer -g'
    export CFLAGS='-fsanitize=address -fno-omit-frame-pointer -g'
    export CC='ccache gcc'
    export CXX='ccache g++'
fi
ccache -s
./autogen.sh
./configure
make
ccache -s
make check
