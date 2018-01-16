Installation Instructions
==================
These are intructions for building stellar-core from source. For a potentially quicker set up we also have stellar-core in a docker container: https://github.com/stellar/docker-stellar-core-horizon

## Picking a version to run

Branches are organized in the following way:

| branch name | description | quality bar |
| ----------- | ----------- | ----------- |
| master      | development branch | all unit tests passing |
| testnet     | version deployed to testnet | acceptance tests passing |
| prod        | version currently deployed on the live network | no recall class issue found in testnet and staging |

For convenience, we also keep a record in the form of release tags of the
 versions that make it to production:
 * pre-releases are versions that get deployed to testnet
 * releases are versions that made it all the way in prod

When running a node, the best bet is to go with the latest release.

## Build Dependencies

- `clang` >= 3.5 or `g++` >= 4.9
- `pkg-config`
- `bison` and `flex`
- `libpq-dev` unless you `./configure --disable-postgres` in the build step below.
- 64-bit system
- `clang-format-5.0` (for `make format` to work)

### Ubuntu 14.04

    # sudo add-apt-repository ppa:ubuntu-toolchain-r/test
    # sudo apt-get update
    # sudo apt-get install git build-essential pkg-config autoconf automake libtool bison flex libpq-dev clang++-3.5 gcc-4.9 g++-4.9 cpp-4.9

In order to make changes, you'll need to install the proper version of clang-format (you may have to follow instructions on https://apt.llvm.org/ )
    # sudo apt-get install clang-format-5.0

See [installing gcc 4.9 on ubuntu 14.04](http://askubuntu.com/questions/428198/getting-installing-gcc-g-4-9-on-ubuntu)

### OS X
When building on OSX, here's some dependencies you'll need:
- Install xcode
- Install homebrew
- brew install libsodium
- brew install libtool
- brew install automake
- brew install pkg-config
- brew install libpqxx *(If ./configure later complains about libpq missing, try PKG_CONFIG_PATH='/usr/local/lib/pkgconfig')*

### Windows
See [INSTALL-Windows.md](INSTALL-Windows.md)

## Basic Installation

- `git clone https://github.com/stellar/stellar-core.git`
- `cd stellar-core`
- `git submodule init`
- `git submodule update`
- Type `./autogen.sh`.
- Type `./configure`   *(If configure complains about compiler versions, try `CXX=clang-3.5 ./configure` or `CXX=g++-4.9 ./configure` or similar, depending on your compiler.)*
- Type `make` or `make -j` (for aggressive parallel build)
- Type `make check` to run tests.
- Type `make install` to install.
