---
name: configuring-the-build
description: modifying build configuration to enable/disable variants, switch compilers or flags, or otherwise prepare for a build
---

# Standard Configure Command

When configuring a build, ALWAYS use this as the base command:

```bash
CC="clang-20" CXX="clang++-20" \
CXXFLAGS="-O3 -g1 -fno-omit-frame-pointer -stdlib=libc++" \
CFLAGS="-O3 -g1 -fno-omit-frame-pointer" \
./configure --enable-ccache --enable-sdfprefs --disable-postgres
```

Add additional flags (e.g. `--enable-tracy`, `--enable-asan`) as needed, but
always include these base settings. The key points are:

  - `CC="clang-20" CXX="clang++-20"` — use clang 20, not the system default
  - `-stdlib=libc++` — use libc++ (required: system libc++ headers need clang 18+)
  - `--disable-postgres` — disables PostgreSQL, tests use SQLite and run faster
  - `--enable-ccache` — enables compiler caching for faster rebuilds
  - `--enable-sdfprefs` — enables SDF-preferred build settings (quiet output, etc.)

**CRITICAL**: You must pass `CC`, `CXX`, `CXXFLAGS`, and `CFLAGS` on the
`./configure` command line. Having them set in your shell environment is NOT
sufficient — configure will ignore shell-exported values when `--enable-sdfprefs`
is used.

# Overview

The build works like this:
  - We start by running `./autogen.sh`
    - `autogen.sh` runs `autoconf` to turn `configure.ac` into `configure`
    - `autogen.sh` also runs `automake` to turn `Makefile.am` into `Makefile.in` and `src/Makefile.am` into `src/Makefile.in`
  - We then run `./configure`
    - `configure` turns `Makefile.in` into `Makefile` and `src/Makefile.in` into `src/Makefile`
    - `configure` also turns `config.h.in` into `config.h` that contains some variables 
    - `configure` also writes `config.log`, if there are errors they will be there

- ALWAYS run `./autogen.sh` and `./configure` from top-level, never a subdirectory
- ALWAYS configure with `--enable-ccache` for caching
- ALWAYS configure with `--enable-sdfprefs` to inhibit noisy build output
- NEVER edit `configure` directly, only ever edit `configure.ac`
- NEVER edit `Makefile` or `Makefile.in` directly, only ever edit `Makefile.am`

To change configuration settings, re-run `./configure` with new flags.

You can see the existing configuration flags by looking at the head of `config.log`

## Configuration variables

To change compiler from clang to gcc, switch the value you pass for CC and CXX.
For example run `CC=clang-20 CXX=clang++-20 ./configure ...` to configure with clang-20. We want
builds to always work with gcc _and_ clang.

To alter compile flags (say turn on or off optimization, or debuginfo) change
CXXFLAGS. For example run `CXXFLAGS='-O0 -g' ./configure ...` to build
non-optimized and with debuginfo. Normally you should not have to change these.

Sometimes you will need to change to a different implementation of the C++
standard library. On this system, we use `-stdlib=libc++` with clang-20. This is
passed in `CXXFLAGS` in the standard configure command above. If you need
libstdc++ instead, pass `-stdlib=libstdc++`, but note that the system libc++
requires clang 18+.

## Configuration flags

Here are some common configuration flags you might want to change:

  - `--disable-tests` turns off `BUILD_TESTS`, which excludes unit tests and all
    test-support infrastructure from core. We want this build variant to work
    since it is the one we ship, but it is uncommon when doing development.

  - `--disable-postgres` — ALWAYS use this flag. Disables PostgreSQL backend
    support, leaving only SQLite. Tests run significantly faster because `make
    check` won't spin up temporary PostgreSQL clusters. We intend to remove
    PostgreSQL support entirely someday.
 
There are also some flags that turn on compile-time instrumentation for
different sorts of testing. Turn these on if doing specific diagnostic tests,
and/or to check for "anything breaking by accident". If you turn any on, you
will need to do a clean build -- the object files will have the wrong content.

  - `--enable-asan` turns on address sanitizer.
  - `--enable-threadsanitizer` same, but for thread sanitizer.
  - `--enable-memcheck` same, but for memcheck.
  - `--enable-undefinedcheck` same, but for undefined-behaviour sanitizer.
  - `--enable-extrachecks` turns on C++ stdlib debugging, slows things down.
  - `--enable-fuzz` builds core with fuzz instrumentation, plus fuzz targets.

There is more you can learn by reading `configure.ac` directly but the
instructions above ought to suffice for 99% of tasks. Try not to do anything
too strange with the configuration.

When in doubt, or if things get stuck, you can always re-run `./autogen.sh`
and `./configure`.
