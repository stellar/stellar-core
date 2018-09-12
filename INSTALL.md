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

- `clang` >= 5.0 or `g++` >= 5.0
- `pkg-config`
- `libtool`
- `bison` and `flex`
- `libpq-dev` unless you `./configure --disable-postgres` in the build step below.
- 64-bit system
- `clang-format-5.0` (for `make format` to work)
- `pandoc`
- `perl`

### Ubuntu 14.04

```
sudo apt-get install software-properties-common
sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt-get update
sudo apt-get install git build-essential pkg-config autoconf automake libtool bison flex libpq-dev
sudo apt-get install clang++-5.0 gcc-5 g++-5 cpp-5
sudo apt-get install pandoc
#optional install for man pages
sudo apt-get install pandoc
```

In order to make changes, you'll need to install the proper version of `clang-format`.

In order to install the llvm (clang) toolchain, you may have to follow instructions on https://apt.llvm.org/
    # sudo apt-get install clang-format-5.0

Note that, for proper documentation generation (man page), `pandoc` is needed:

See [installing gcc 5 on ubuntu 14.04](https://askubuntu.com/questions/618474/how-to-install-the-latest-gcurrently-5-1-in-ubuntucurrently-14-04)



### OS X
When building on OSX, here's some dependencies you'll need:

- Install xcode
- Install homebrew

Then:

```
brew install libsodium
brew install libtool
brew install automake
brew install pkg-config
brew install libpqxx
#optional install for man pages
brew install pandoc
```

*(If ./configure later complains about libpq missing, try PKG_CONFIG_PATH='/usr/local/lib/pkgconfig')*

### Windows
See [INSTALL-Windows.md](INSTALL-Windows.md)

## Basic Installation

```
git clone https://github.com/stellar/stellar-core.git
cd stellar-core
git submodule init
git submodule update
./autogen.sh
#If configure complains about compiler versions, try setting CXX environment variable, e.g.
#CXX=clang-5.0 ./configure 
#CXX=g++-5 ./configure 
./configure
#make or make -j for parallel build
make
#run tests
make check
#install
make install
```
