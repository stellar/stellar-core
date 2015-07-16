Installation Instructions
==================

## Build Dependencies


- `clang` >= 3.5 or `g++` >= 4.9
- `pkg-config`
- `bison` and `flex`


### Ubuntu 14.04
    # sudo add-apt-repository ppa:ubuntu-toolchain-r/test
    # apt-get update
    # sudo apt-get install git build-essential pkg-config autoconf libtool bison flex libpq-dev clang++-3.5 gcc-4.9 g++-4.9 cpp-4.9

See [installing gcc 4.9 on ubuntu 14.04](http://askubuntu.com/questions/428198/getting-installing-gcc-g-4-9-on-ubuntu)

### OSX
When building on OSX, here's some dependencies you'll need:
Install xcode
Install homebrew
brew install libsodium
brew install libtool
brew install automake
brew install pkg-config

## Basic Installation

- `git clone ...`
- `git submodule init`
- `git submodule update`
- Type `./autogen.sh`.
- Type `CXX=gcc-4.9 ./configure` or `CXX=clang-3.5 ./configure`
- Type `make`.
- Type `make check` to run tests.
- Type `make install` to install.
