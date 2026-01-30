---
name: configuring-the-build
description: modifying build configuration to enable/disable variants, switch compilers or flags, or otherwise prepare for a build
---

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
For example run `CXX=g++ CC=gcc ./configure ...` to configure with gcc. We want
builds to always work with gcc _and_ clang.

To alter compile flags (say turn on or off optimization, or debuginfo) change
CXXFLAGS. For example run `CXXFLAGS='-O0 -g' ./configure ...` to build
non-optimized and with debuginfo. Normally you should not have to change these.

Sometimes you will need to change to a different implementation of the C++
standard library. To do this, pass `-stdlib=libc++` or `-stdlib=libstdc++`
in `CXXFLAGS` explicitly. But again, normally you don't need to do this.

## Configuration flags

Here are some common configuration flags you might want to change:

  - `--disable-tests` turns off `BUILD_TESTS`, which excludes unit tests and all
    test-support infrastructure from core. We want this build variant to work
    since it is the one we ship, but it is uncommon when doing development.

  - `--disable-postgres` turns off postgresql backend support in core, leaving
    only sqlite. tests will run faster, and also this is a configuration we want
    to work (we will remove postgres entirely someday).
 
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