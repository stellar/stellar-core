Installation Instructions
==================

These are instructions for building stellar-core from source.

For a potentially quicker set up, the following projects could be good alternatives:

* stellar-core in a [docker container](https://github.com/stellar/docker-stellar-core)
* stellar-core and [horizon](https://github.com/stellar/stellar-horizon) in a [docker container](https://github.com/stellar/quickstart)
* pre-compiled [packages](https://github.com/stellar/packages)

## Which version to run?

In general, you should aim to run the most recent stable version of core, so make sure
to keep track of new releases.

We _highly_ recommend upgrading to the latest core release _within 30 days of a release_ as
highlighted in our [protocol and security release notes](docs/software/security-protocol-release-notes.md) in case
the release contains security fixes that could be exploited (we do not disclose
ahead of time if a release contains security fixes to give people time to upgrade).

As a consequence, old, potentially insecure or abandoned nodes _running releases that are older than 90 days will get blocked_ by newer nodes (if there are newer releases
of course).

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
 * releases are versions that made it all the way to production

## Containerized dev environment

We maintain a pre-configured Docker configuration ready for development with VSCode.

See the [dev container's README](.devcontainer/README.md) for more detail.

## Runtime dependencies

`stellar-core` does not have many dependencies.

If core was configured (see below) to work with Postgresql, a local Postgresql server
 will need to be deployed to the same host.

To install Postgresql, follow instructions from the [Postgresql download page](https://www.postgresql.org/download/).

## Build Dependencies

- c++ toolchain and headers that supports c++17
    - `clang` >= 20.0
    - `g++` >= 14.0
- `pkg-config`
- `bison` and `flex`
- `libpq-dev` unless you `./configure --disable-postgres` in the build step below.
- 64-bit system
- `clang-format-20` (for `make format` to work)
- `sed` and `perl`
- `libgoogle-perftools-dev` (Linux only, for tcmalloc memory allocator)
- Rust toolchain (see [Installing Rust](#installing-rust) subsection)
  - `cargo` >= 1.74
  - `rust` >= 1.74

### Installing Rust

Building the Rust components requires the `cargo` package manager and build system, as well as the `rustc` compiler, both version 1.74 or later.

We recommend installing Rust using the Rust project's `rustup` installer, which can be found on [rustup.rs](https://rustup.rs).

We also include a script in the repository `install-rust.sh` that downloads and runs a known version of `rustup` on x64-linux hosts, such as those used for CI and packaging.

### Ubuntu

#### Ubuntu 22.04
If you build Ubuntu with clang you have to use `libc++` instead of `libstdc++` when compiling.

Stock Ubuntu doesn't come packaged with clang-20, so you have add LLVM apt repository dependency for it,
```zsh
sudo wget -O /etc/apt/trusted.gpg.d/apt.llvm.org.asc https://apt.llvm.org/llvm-snapshot.gpg.key

sudo tee /etc/apt/sources.list.d/llvm.list <<EOF
deb http://apt.llvm.org/jammy/ llvm-toolchain-jammy-20 main
deb-src http://apt.llvm.org/jammy/ llvm-toolchain-jammy-20 main
EOF
```

# install clang-20 toolchain
```zsh
sudo apt-get update
sudo apt-get -y install clang-20 llvm-20
```

After installing packages, head to [building with clang and libc++](#building-with-clang-and-libc).


#### Adding gcc-14 / g++-14 from PPA (optional)
```zsh
# NOTE: newer version of the compilers are not
#    provided by stock distributions
#    and are provided by the /test toolchain
sudo apt-get install software-properties-common
sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt-get update
sudo apt install gcc-14 g++-14
```

#### Installing packages
```zsh
# common packages
sudo apt-get update
sudo apt-get install git build-essential pkg-config autoconf automake libtool bison flex libpq-dev libgoogle-perftools-dev parallel sed perl
# if using clang
sudo apt-get -y install clang-20 llvm-20
# clang with libc++
sudo apt-get install libc++-20-dev libc++abi-20-dev libclang-rt-20-dev
# clang with libstdc++
# sudo apt-get install gcc-14
# if using g++ or building with libstdc++
# sudo apt-get install gcc-14 g++-14 cpp-14
```

In order to make changes, you'll need to install the proper version of clang-format.

In order to install the llvm (clang) toolchain, you may have to follow instructions on https://apt.llvm.org/
```zsh
sudo apt-get install clang-format-20
```

### Ubuntu 24.04

For Ubuntu 24.04 and other newer Linux distros, it is recommended to build with gcc-14 or clang-20. This can be installed as follows:

```zsh
# if using clang
sudo apt-get install clang-20
# if using g++ or building with libstdc++
# sudo apt-get install gcc-14 g++-14 cpp-14
# if building with libc++
sudo apt-get install libc++-20-dev libc++abi-20-dev libclang-rt-20-dev
```

Note that installing libc++-20 via apt will uninstall all other libc++ versions.

Additionally, some newer Linux distros no longer package clang-format-20, and newer clang-format versions are
not backwards compatible. To build from source, you'll need to do the following:

```zsh
sudo apt update
git clone --depth 1 --branch release/20.x https://github.com/llvm/llvm-project.git
cd llvm-project

# Create build directory
mkdir build
cd build

# Configure with CMake
CC=clang CXX=clang++ cmake -S llvm -B build -G Ninja -DLLVM_ENABLE_PROJECTS="clang" -DCMAKE_BUILD_TYPE=Release

# Build
ninja clang-format

sudo cp bin/clang-format /usr/bin/clang-format-20
cd ../..
rm -rf llvm-project/
```

### MacOS 
When building on MacOS, here's some dependencies you'll need:
- Install xcode
- Install [Rust](#installing-rust)
- Install [homebrew](https://brew.sh)
- `brew install libsodium libtool autoconf automake pkg-config libpq openssl parallel ccache bison gnu-sed perl coreutils`

You'll also need to configure pkg-config by adding the following to your shell (`.zshenv` or `.zshrc`):
```zsh
export PKG_CONFIG_PATH="$PKG_CONFIG_PATH:$(brew --prefix)/opt/libpq/lib/pkgconfig"
export PKG_CONFIG_PATH="$PKG_CONFIG_PATH:$(brew --prefix)/opt/openssl@3/lib/pkgconfig"
export PATH="$(brew --prefix bison)/bin:$PATH"
```
- Install `clang-format-20` 
> Note: `brew` does not contain a standalone cask for `clang-format-20`, but you can get it from the `llvm@20` package.

To install `clang-format-20` using Homebrew:
```
# Install LLVM 20 which includes clang-format-20
brew install llvm@20

# Create a symlink to make clang-format-20 available globally
sudo ln -sf /opt/homebrew/opt/llvm@20/bin/clang-format /usr/local/bin/clang-format-20
# Ensure "clang-format version 20.0.0" is printed.
clang-format --version
```

> Note: macOS will block the execution for security purposes, open System Preferences > Security & Privacy and allow `clang-format` to run.

### Windows
See [INSTALL-Windows.md](INSTALL-Windows.md)

## Basic Installation

- `git clone https://github.com/stellar/stellar-core.git`
- `cd stellar-core`
- `git submodule init`
- `git submodule update`
- Type `./autogen.sh`.
- Type `./configure`   *(If configure complains about compiler versions, try `CXX=clang-20 ./configure` or `CXX=g++-14 ./configure` or similar, depending on your compiler.)*
- Type `make` or `make -j<N>` (where `<N>` is the number of parallel builds, a number less than the number of CPU cores available, e.g. `make -j3`)
- Type `make check` to run tests.
- Type `make install` to install.

## Building with clang and libc++

On some systems, building with `libc++`, [LLVM's version of the standard library](https://libcxx.llvm.org/) can be done instead of `libstdc++` (typically used on Linux).

NB: there are newer versions available of both clang and libc++, you will have to use the versions suited for your system.

You may need to install additional packages for this, for example, on Linux Ubuntu 24.04 LTS with clang-20:

    # install libc++ headers
    sudo apt-get install libc++-20-dev libc++abi-20-dev

Here are sample steps to achieve this:

    export CC=clang-20
    export CXX=clang++-20
    export CFLAGS="-O3 -g1 -fno-omit-frame-pointer"
    export CXXFLAGS="$CFLAGS -stdlib=libc++"
    git clone https://github.com/stellar/stellar-core.git
    cd stellar-core/
    ./autogen.sh && ./configure && make -j6

## Building for ARM Linux (i.e. Raspberry Pi)

`stellar-core` is lightweight and can run on many edge devices such as a Raspberry Pi. However, there is currently a
[linker bug](https://bugs.llvm.org/show_bug.cgi?id=16404) in the default ARM `libgcc` runtime, so `compiler-rt` must be used instead.
Here are sample steps to achieve this:

    export CC=clang-20
    export CXX=clang++-20
    export CFLAGS="-O3 -g1 -fno-omit-frame-pointer --rtlib=compiler-rt"
    export CXXFLAGS="$CFLAGS -stdlib=libc++"
    git clone https://github.com/stellar/stellar-core.git
    cd stellar-core/
    ./autogen.sh && ./configure && make -j4

## Building with Tracing

Configuring with `--enable-tracy` will build and embed the client component of the [Tracy](https://github.com/wolfpld/tracy) high-resolution tracing system in the `stellar-core` binary.

The tracing client will activate automatically when stellar-core is running, and will listen for connections from Tracy servers (a command-line capture utility, or a cross-platform GUI).

You do not need to download the tracy server, and will likely run into versioning issues if you do. Instead, the Tracy server components can also be compiled by configuring with `--enable-tracy-gui` or `--enable-tracy-capture`. Once compiled, the tracy server can be started with `./tracy-gui` or `./tracy`, respectively.

The GUI depends on the `capstone`, `freetype` and `glfw` libraries and their headers, and on linux or BSD the `GTK-2.0` libraries and headers. On Windows and MacOS, native toolkits are used instead.


    # On Ubuntu
    $ sudo apt-get install libcapstone-dev libfreetype6-dev libglfw3-dev libgtk2.0-dev

    # On MacOS
    $ brew install capstone freetype2 glfw
