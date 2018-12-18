Installation Instructions
==================
These are instructions for building stellar-core from source.

For a potentially quicker set up, the following projects could be good alternatives:

* stellar-core in a [docker container](https://github.com/stellar/docker-stellar-core-horizon)
* pre-compiled [packages](https://github.com/stellar/packages)

## Picking a version to run

Best is to use the latest *stable* release that can be downloaded from https://github.com/stellar/stellar-core/releases


Alternatively, branches are organized in the following way:

| branch name | description | quality bar |
| ----------- | ----------- | ----------- |
| master      | development branch | all unit tests passing |
| testnet     | version deployed to testnet | acceptance tests passing |
| prod        | version currently deployed on the live network | no recall class issue found in testnet and staging |

For convenience, we also keep a record in the form of release tags of the
 versions that make it to production:
 * pre-releases are versions that get deployed to testnet
 * releases are versions that made it all the way in prod

## Build Dependencies

- c++ toolchain and headers that supports c++14
    - `clang` >= 5.0
    - `g++` >= 6.0
- `pkg-config`
- `bison` and `flex`
- `libpq-dev` unless you `./configure --disable-postgres` in the build step below.
- 64-bit system
- `clang-format-5.0` (for `make format` to work)
- `pandoc`
- `perl`

### Ubuntu

#### Ubuntu 14.04
You will have to install the [test toolchain](#adding-the-test-toolchain) in order to both build and run stellar-core.

#### Ubuntu 16.04
Just like 14.04, you can install the test toolchain to build and run stellar-core.

Alternatively, if you want to just depend on stock 16.04, you will have to build with clang *and* have use `libc++` instead of `libstdc++` when compiling.

After installing packages, head to [building with clang and libc++](#building-with-clang-and-libc).


#### Adding the test toolchain
    # NOTE: newer version of the compilers are not
    #    provided by stock distributions
    #    and are provided by the /test toolchain
    sudo apt-get install software-properties-common
    sudo add-apt-repository ppa:ubuntu-toolchain-r/test
    sudo apt-get update

#### Installing packages
    # common packages
    sudo apt-get install git build-essential pkg-config autoconf automake libtool bison flex libpq-dev
    # if using clang
    sudo apt-get install clang-5.0
    # clang with libstdc++
    sudo apt-get install gcc-6
    # if using g++ or building with libstdc++
    # sudo apt-get install gcc-6 g++-6 cpp-6

    # optional: pandoc (to compile man pages)
    sudo apt-get install pandoc

In order to make changes, you'll need to install the proper version of clang-format.

In order to install the llvm (clang) toolchain, you may have to follow instructions on https://apt.llvm.org/

    sudo apt-get install clang-format-5.0


### OS X
When building on OSX, here's some dependencies you'll need:
- Install xcode
- Install homebrew
- brew install libsodium
- brew install libtool
- brew install automake
- brew install pkg-config
- brew install libpqxx *(If ./configure later complains about libpq missing, try PKG_CONFIG_PATH='/usr/local/lib/pkgconfig')*
- brew install pandoc

### Windows
See [INSTALL-Windows.md](INSTALL-Windows.md)

## Basic Installation

- `git clone https://github.com/stellar/stellar-core.git`
- `cd stellar-core`
- `git submodule init`
- `git submodule update`
- Type `./autogen.sh`.
- Type `./configure`   *(If configure complains about compiler versions, try `CXX=clang-5.0 ./configure` or `CXX=g++-6 ./configure` or similar, depending on your compiler.)*
- Type `make` or `make -j` (for aggressive parallel build)
- Type `make check` to run tests.
- Type `make install` to install.

## Building with clang and libc++

On some systems, building with `libc++`, [LLVM's version of the standard library](https://libcxx.llvm.org/) can be done instead of `libstdc++` (typically used on Linux).

NB: there are newer versions available of both clang and libc++, you will have to use the versions suited for your system.

You may need to install additional packages for this, for example, on Linux Ubuntu:

    # install libc++ headers
    sudo apt-get install libc++-dev libc++abi-dev

Here are sample steps to achieve this:

    export CC=clang-5.0
    export CXX=clang++-5.0
    export CFLAGS="-O3 -g1 -fno-omit-frame-pointer"
    export CXXFLAGS="$CFLAGS -stdlib=libc++ -isystem /usr/include/libcxxabi"
    git clone https://github.com/stellar/stellar-core.git
    cd stellar-core/
    ./autogen.sh && ./configure && make -j6
