#!/bin/bash

# determine platform
PLATFORM=$(uname)
if [[ $PLATFORM == 'Linux' ]]; then
    echo "Installing Linux dependencies..."
    add-apt-repository -y ppa:ubuntu-toolchain-r/test >/dev/null
    apt-get update >/dev/null
    apt-get install -y git build-essential pkg-config autoconf automake libtool bison flex libpq-dev clang++-5.0 gcc-5 g++-5 cpp-5 pandoc

    # check for installation failure before continuing
    if [ $? -eq 1 ]; then
        echo "Failed installing Linux dependencies\nExiting"
        exit 1
    fi

elif [[ $PLATFORM == 'Darwin' ]]; then
    # requires XCode to be already installed
    # install homebrew if not already installed
    brew help >/dev/null
    if [ $? -eq 1 ]; then
        echo "Installing homebrew..."
        /usr/bin/ruby -e "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)" >/dev/null
    fi

    echo "Installing Darwin dependencies..."
    brew install libsodium libtool automake pkg-config libpqxx pandoc

    # check for installation failure before continuing
    if [ $? -eq 1 ]; then
        echo "Failed installing Darwin dependencies\nExiting"
        exit 1
    fi
    
else
    echo "Unknown platform\nExiting..."
    exit 1
fi

git submodule init >/dev/null
git submodule update >/dev/null
./autogen.sh

# ./configure
# make
# make install


