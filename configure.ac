# -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

# Copyright 2015 Stellar Development Foundation and contributors. Licensed
# under the Apache License, Version 2.0. See the COPYING file at the root
# of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

AC_PREREQ([2.68])
AC_INIT([stellar-core],[0.1],[],[],[http://www.stellar.org])
AM_INIT_AUTOMAKE([-Wall subdir-objects silent-rules])
AM_SILENT_RULES([yes])
AC_CONFIG_SRCDIR([configure.ac])
AC_CONFIG_MACRO_DIR([m4])

test "${WFLAGS+set}" || WFLAGS="-Wall"
test "${CFLAGS+set}" || CFLAGS="-g -O2"
test "${CXXFLAGS+set}" || CXXFLAGS="-g -O2"

AC_PROG_CC([clang gcc cc])
AC_PROG_CXX([clang++ g++ c++])
AM_PROG_AR
AM_PROG_CC_C_O

LT_INIT([disable-shared])
AC_SUBST(LIBTOOL_DEPS)

LIBSODIUM_MIN_VERSION="1.0.0"

dnl AC_PROG_INSTALL

test -z "$WFLAGS" || CFLAGS="$CFLAGS $WFLAGS" CXXFLAGS="$CXXFLAGS $WFLAGS"

AC_LANG(C++)
AX_CXX_COMPILE_STDCXX_11(noext,mandatory)
AX_FRESH_COMPILER

# Our large include path set makes for annoying warnings without this
AX_APPEND_COMPILE_FLAGS([-Wno-unused-command-line-argument])
AX_APPEND_COMPILE_FLAGS([-Qunused-arguments])

# Asio's headers have unused typedefs that flood the compilation output without this
AX_APPEND_COMPILE_FLAGS([-Wno-unused-local-typedef])

# We want to consider unused MUST_USE results as errors
AX_APPEND_COMPILE_FLAGS([-Werror=unused-result])

# -pthread seems to be required by -std=c++11 on some hosts
AX_APPEND_COMPILE_FLAGS([-pthread])

# add colorized diagnostics when possible
AX_APPEND_COMPILE_FLAGS([-fcolor-diagnostics])

# Also add CXXFLAGS changes to CFLAGS
AC_LANG_PUSH(C)
AX_APPEND_COMPILE_FLAGS([-Werror=unused-result])
AX_APPEND_COMPILE_FLAGS([-Wno-unused-command-line-argument])
AX_APPEND_COMPILE_FLAGS([-Qunused-arguments])
AX_APPEND_COMPILE_FLAGS([-fcolor-diagnostics])
AC_LANG_POP(C)

# Permit user to enable asan instrumentation
AC_ARG_ENABLE([asan],
              AS_HELP_STRING([--enable-asan],
                             [build with asan (address-sanitizer) instrumentation]))
AS_IF([test "x$enable_asan" = "xyes"], [
  AX_APPEND_COMPILE_FLAGS([-fsanitize=address])
  AX_APPEND_COMPILE_FLAGS([-fno-omit-frame-pointer])
  AC_LANG_PUSH(C)
  AX_APPEND_COMPILE_FLAGS([-fsanitize=address])
  AX_APPEND_COMPILE_FLAGS([-fno-omit-frame-pointer])
  AC_LANG_POP(C)
])

# Permit user to build with ccache
AC_ARG_ENABLE([ccache],
              AS_HELP_STRING([--enable-ccache],
                             [build with ccache]))
AS_IF([test "x$enable_ccache" = "xyes"], [
  AC_CHECK_PROGS([CCACHE], [ccache])
  CC="ccache ${CC}"
  CXX="ccache ${CXX}"
])

# Permit user to enable AFL instrumentation
AC_ARG_ENABLE([afl],
              AS_HELP_STRING([--enable-afl],
                             [build with AFL (fuzzer) instrumentation]))
AS_IF([test "x$enable_afl" = "xyes"], [
  AS_IF([test "x$enable_asan" = "xyes"], [
    AC_MSG_ERROR([AFL is presently incompatible with Address Sanitizer])
  ])
  AS_IF([test "x$enable_ccache" = "xyes"], [
    AC_MSG_ERROR([AFL is presently incompatible with ccache])
  ])
  AC_CHECK_PROGS([AFL_FUZZ], [afl-fuzz])
  AS_CASE(["$CC"],
          [clang*], [AC_CHECK_PROGS([AFL_CLANG], [afl-clang-fast])
                     AC_CHECK_PROGS([AFL_CLANGPP], [afl-clang-fast++])
                     CC="afl-clang-fast"
                     CXX="afl-clang-fast++"],
          [gcc*],   [AC_CHECK_PROGS([AFL_GCC], [afl-gcc])
                     AC_CHECK_PROGS([AFL_GPP], [afl-g++])
                     CC="afl-gcc"
                     CXX="afl-g++"],
          [AC_MSG_ERROR([Don't know how to instrument CC=$CC with AFL])])
])
AM_CONDITIONAL([USE_AFL_FUZZ], [test "x$enable_afl" == "xyes"])

AC_CHECK_PROGS(CLANG_FORMAT, [clang-format clang-format-3.6])
AM_CONDITIONAL([USE_CLANG_FORMAT], [test "x$CLANG_FORMAT" != "x"])

AX_VALGRIND_CHECK

# This is a simple replacement for AX_CONFIG_DIR. We can't use
# AC_CONFIG_SUBDIRS here because we want configuration to run immediately
# so we can use the results in the calls to PKG_CHECK_MODULES immediately
# below.  AX_CONFIG_DIR might work but it's ambiguously licensed.
function configure_pkgconfig_subdir() {
    ax_pkgname=`basename $1`
    if test -f "$1/${ax_pkgname}-uninstalled.pc" -o -f "$1/${ax_pkgname}-uninstalled.pc.in"
    then
        if test -f "$1/configure.ac"
        then
            if test ! -f "$1/configure"
            then
                AC_MSG_ERROR([included $ax_pkgname present but no configure, run ./autogen.sh ?])
            fi
            ax_popdir=`pwd`
            cd "$1"
            AC_MSG_NOTICE([Configuring sources in $1])
            eval ./configure $ac_configure_args "'--srcdir=$ac_srcdir'" "'$2'" "CXX='${CXX}'" "CC='${CC}'" "CXXFLAGS='${CXXFLAGS}'" "LDFLAGS='${LDFLAGS}'"
            AC_MSG_NOTICE([Finished configuring $1])
            cd "$ax_popdir"
        fi
        export PKG_CONFIG_PATH="${1}:${PKG_CONFIG_PATH}"
        eval ${ax_pkgname}_INTERNAL=1
    fi
}

configure_pkgconfig_subdir src/lib/libsodium --disable-shared
configure_pkgconfig_subdir src/lib/sqlite
configure_pkgconfig_subdir src/lib/xdrpp
configure_pkgconfig_subdir src/lib/libmedida

PKG_CHECK_MODULES([libsodium],[libsodium])
AM_CONDITIONAL(LIBSODIUM_INTERNAL, [test "x$libsodium_INTERNAL" != "x"])

PKG_CHECK_MODULES([sqlite],[sqlite])
AM_CONDITIONAL(SQLITE_INTERNAL, [test "x$sqlite_INTERNAL" != "x"])

PKG_CHECK_MODULES([xdrpp], [xdrpp])
AM_CONDITIONAL(XDRPP_INTERNAL, [test "x$xdrpp_INTERNAL" != "x"])

PKG_CHECK_MODULES([libmedida], [libmedida])
AM_CONDITIONAL(LIBMEDIDA_INTERNAL, [test "x$libmedida_INTERNAL" != "x"])

AC_MSG_CHECKING(for xdrc)
XDRC=$(pkg-config --variable=xdrc xdrpp)
AC_MSG_RESULT($XDRC)
AC_SUBST(XDRC)

# We do not bundle libpq, but we'll compile-in support if it's present.
# Add other optional database drivers here (and later, in SOCI).
PKG_CHECK_MODULES([libpq],[libpq],[use_POSTGRES=1],[use_POSTGRES=])
AM_CONDITIONAL(USE_POSTGRES, [test "x$use_POSTGRES" != "x"])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT

