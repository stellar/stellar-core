# -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

# Copyright 2015 Stellar Development Foundation and contributors. Licensed
# under the Apache License, Version 2.0. See the COPYING file at the root
# of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

AC_PREREQ([2.68])
AC_INIT([stellar-core],[0.1],[],[],[http://www.stellar.org])
# tar-ustar is required for long file names when libsodium is bundled
AM_INIT_AUTOMAKE([-Wall -Wextra -Wconversion subdir-objects tar-ustar silent-rules])
AC_CONFIG_SRCDIR([configure.ac])
AC_CONFIG_MACRO_DIR([m4])

AC_ARG_VAR([LIBCXX_PATH], [path to libc++ and libc++abi])

if test -z "${WFLAGS+set}"; then
   WFLAGS=-Wall

   # Our large include path set makes for annoying warnings without this
   WFLAGS="$WFLAGS -Wno-unused-command-line-argument -Qunused-arguments"

   # Asio's headers have unused typedefs that flood the compilation
   # output without this
   WFLAGS="$WFLAGS -Wno-unused-local-typedef"

   # Also don't _further_ warn if the previous warning flag was unknown
   WFLAGS="$WFLAGS -Wno-unknown-warning-option"

   # We want to consider unused MUST_USE results as errors
   WFLAGS="$WFLAGS -Werror=unused-result"
fi

test "${CFLAGS+set}" || CFLAGS="-g -O2"
test "${CXXFLAGS+set}" || CXXFLAGS="$CFLAGS"

AC_PROG_CC([clang gcc cc])
AC_PROG_CXX([clang++ g++ c++])
AM_PROG_AR
AM_PROG_CC_C_O

LT_INIT([disable-shared])
AC_SUBST(LIBTOOL_DEPS)

AC_LANG(C++)
AX_CXX_COMPILE_STDCXX_14(noext,mandatory)
AX_FRESH_COMPILER
# -pthread seems to be required by -std=c++14 on some hosts
AX_APPEND_COMPILE_FLAGS([-pthread])

AC_MSG_CHECKING([whether defect report N4387 is resolved])
AC_COMPILE_IFELSE([AC_LANG_SOURCE([[#include <tuple>
std::tuple<int, int> f()
{
    return {1, 2};
}
]])], [AC_MSG_RESULT([yes])], [AC_MSG_RESULT([no]); AC_MSG_ERROR([defect report N4387 is not resolved])], AC_MSG_FAILURE)

AC_MSG_CHECKING([for c++14 compliant std::weak_ptr move-constructor])
AC_RUN_IFELSE([AC_LANG_PROGRAM([[#include <memory>]], [[std::shared_ptr<int> shared = std::make_shared<int>(0);
std::weak_ptr<int> weak1(shared);
std::weak_ptr<int> weak2(std::move(weak1));
return !((weak1.expired()) && (weak1.lock() == nullptr));
]])], [AC_MSG_RESULT([yes])], [AC_MSG_RESULT([no]); AC_MSG_ERROR([non-compliant std::weak_ptr move-constructor])], AC_MSG_FAILURE)

AC_MSG_CHECKING([for c++14 compliant std::weak_ptr move-assignment operator])
AC_RUN_IFELSE([AC_LANG_PROGRAM([[#include <memory>]], [[std::shared_ptr<int> shared = std::make_shared<int>(0);
std::weak_ptr<int> weak1(shared);
std::weak_ptr<int> weak2 = std::move(weak1);
return !((weak1.expired()) && (weak1.lock() == nullptr));
]])], [AC_MSG_RESULT([yes])], [AC_MSG_RESULT([no]); AC_MSG_ERROR([non-compliant std::weak_ptr move-assignment operator])], AC_MSG_FAILURE)

AC_ARG_ENABLE([sdfprefs],
    AS_HELP_STRING([--enable-sdfprefs],
        [Enable build settings preferred by Stellar developers]))

AS_IF([test xyes = "x$enable_sdfprefs"],
        [AM_SILENT_RULES([yes])
    WFLAGS="$WFLAGS -fcolor-diagnostics"])

AS_IF([test xyes != "x$enable_sdfprefs" -a xyes != "x$enable_silent_rules"],
        ac_configure_args="$ac_configure_args --disable-silent-rules")

AX_APPEND_COMPILE_FLAGS($WFLAGS)
AC_LANG_PUSH(C)
AX_APPEND_COMPILE_FLAGS($WFLAGS)
# ensure that we also enable pthread in C code
AX_APPEND_COMPILE_FLAGS([-pthread])
AC_LANG_POP(C)

unset sanitizeopts

AC_ARG_ENABLE([asan],
  AS_HELP_STRING([--enable-asan],
        [build with asan (address-sanitizer) instrumentation]))
AS_IF([test "x$enable_asan" = "xyes"], [
  AC_MSG_NOTICE([ Enabling asan, see https://clang.llvm.org/docs/AddressSanitizer.html ])
  sanitizeopts="address"
])

AC_ARG_ENABLE([memcheck],
  AS_HELP_STRING([--enable-memcheck],
        [build with memcheck (memory-sanitizer) instrumentation]))
AS_IF([test "x$enable_memcheck" = "xyes"], [
  AC_MSG_NOTICE([ enabling memory-sanitizer, see https://clang.llvm.org/docs/MemorySanitizer.html ])
  AC_MSG_NOTICE([ To completely enable poison destructor set MSAN_OPTIONS=poison_in_dtor=1 before running the program ])

  AS_IF([test x != "x$sanitizeopts"], [
    AC_MSG_ERROR(Cannot enable multiple checkers at once)
  ])
  sanitizeopts="memory -fsanitize-memory-track-origins=2 -fsanitize-memory-use-after-dtor"

  if test -z "$LIBCXX_PATH"; then
   AC_MSG_ERROR(LIBCXX_PATH must be set for memcheck to work)
  fi

  CXXFLAGS="$CXXFLAGS -DMSAN_ENABLED"
  LDFLAGS="$LDFLAGS -fsanitize=$sanitizeopts"
])

AS_IF([test x != "x$LIBCXX_PATH"], [
  # use custom libc++
  CXXFLAGS="$CXXFLAGS -stdlib=libc++"
  LDFLAGS="$LDFLAGS -L$LIBCXX_PATH -stdlib=libc++ -lc++abi -Wl,-rpath -Wl,$LIBCXX_PATH"
])

AC_ARG_ENABLE([undefinedcheck],
  AS_HELP_STRING([--enable-undefinedcheck],
        [build with undefinedcheck (undefined-behavior-sanitizer) instrumentation]))
AS_IF([test "x$enable_undefinedcheck" = "xyes"], [
  AC_MSG_NOTICE([ enabling undefined-behavior-sanitizer, see https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html ])
  AS_IF([test x != "x$sanitizeopts"], [
    AC_MSG_ERROR(Cannot enable multiple checkers at once)
  ])
  sanitizeopts="undefined"
])

AS_IF([test x != "x$sanitizeopts"], [
  # Compilation should fail if these options are not supported
  sanflags="-fsanitize=$sanitizeopts -fno-omit-frame-pointer"
  CFLAGS="$CFLAGS $sanflags"
  CXXFLAGS="$CXXFLAGS $sanflags"

  # compile our own libraries when sanitizers are enabled
  libsodium_INTERNAL=yes
  xdrpp_INTERNAL=yes

])

AC_ARG_ENABLE([extrachecks],
  AS_HELP_STRING([--enable-extrachecks],
        [build with additional debugging checks enabled]))
AS_IF([test "x$enable_extrachecks" = "xyes"], [
  # don't try to detect which c++ library we're using
  CXXFLAGS="$CXXFLAGS -D_GLIBCXX_DEBUG=1 -D_LIBCPP_DEBUG=1"
])

AC_ARG_ENABLE([ccache],
              AS_HELP_STRING([--enable-ccache], [build with ccache]))
AS_IF([test "x$enable_ccache" = "xyes"], [
  AC_CHECK_PROGS([CCACHE], [ccache])
  case "$CC" in
  *ccache\ *)
      ;;
  *)
      CC="ccache ${CC}"
      ;;
  esac
  case "$CXX" in
    *ccache\ *)
      ;;
    *)
      CXX="ccache ${CXX}"
      ;;
  esac
])

# Permit user to enable AFL instrumentation
AC_ARG_ENABLE([afl],
              AS_HELP_STRING([--enable-afl],
                             [build with AFL (fuzzer) instrumentation]))
AS_IF([test "x$enable_afl" = "xyes"], [
  AS_IF([test "x$sanitizeopts" != "x"], [
    AC_MSG_ERROR([AFL is presently incompatible with sanitizers])
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

# prefer 5.0 as it's the one we use
AC_CHECK_PROGS(CLANG_FORMAT, [clang-format-5.0 clang-format])
AM_CONDITIONAL([USE_CLANG_FORMAT], [test "x$CLANG_FORMAT" != "x"])

AX_VALGRIND_CHECK

if test yes != "$enable_shared"; then
   ac_configure_args="$ac_configure_args --disable-shared"
fi

# We use several features of sqlite that require not just a new version
# (eg. partial indexes, >=3.8.0; upserts, >= 3.24.0) but also the carray
# extension, which is compiled-out of most platform sqlites. We therefore
# always use our own bundled copy, version 3.26.0 at the time of this
# writing.
sqlite3_CFLAGS='-isystem $(top_srcdir)/lib/sqlite -DSQLITE_CORE -DSQLITE_OMIT_LOAD_EXTENSION=1'
sqlite3_LIBS=
AC_SUBST(sqlite3_CFLAGS)
AC_SUBST(sqlite3_LIBS)

PKG_CHECK_MODULES(libsodium, [libsodium >= 1.0.13], :, libsodium_INTERNAL=yes)

AX_PKGCONFIG_SUBDIR(lib/libsodium)
if test -n "$libsodium_INTERNAL"; then
   libsodium_LIBS='$(top_builddir)/lib/libsodium/src/libsodium/libsodium.la'
fi

AX_PKGCONFIG_SUBDIR(lib/xdrpp)
AC_MSG_CHECKING(for xdrc)
if test -n "$XDRC"; then
   :
elif test -n "$xdrpp_INTERNAL" -a x"$cross_compiling" != xyes; then
   XDRC='$(top_builddir)/lib/xdrpp/xdrc/xdrc$(EXEEXT)'
else
   AC_PATH_PROG(XDRC, [xdrc])
fi
if test -z "$XDRC"; then
   AC_MSG_ERROR(Cannot find xdrc)
fi
AC_MSG_RESULT($XDRC)
AC_SUBST(XDRC)

# Directory needed for xdrc output (won't exist in build directory)
mkdir -p src/xdr

if test -s "$srcdir/lib/medida.mk"; then
   libmedida_CFLAGS='-isystem $(top_srcdir)/lib/libmedida/src'
   libmedida_LIBS='$(top_builddir)/lib/libmedida.a'
   libmedida_INTERNAL=yes
else
   PKG_CHECK_MODULES(libmedida, libmedida)
   unset libmedida_INTERNAL
fi
AM_CONDITIONAL(LIBMEDIDA_INTERNAL, test -n "$libmedida_INTERNAL")
AC_SUBST(libmedida_CFLAGS)
AC_SUBST(libmedida_LIBS)

soci_CFLAGS='-isystem $(top_srcdir)/lib/soci/src/core'
soci_CFLAGS="$soci_CFLAGS "'-isystem $(top_srcdir)/lib/soci/src/backends/sqlite3'
soci_LIBS='$(top_builddir)/lib/libsoci.a'
AC_SUBST(soci_CFLAGS)
AC_SUBST(soci_LIBS)

libasio_CFLAGS='-DASIO_SEPARATE_COMPILATION=1 -DASIO_STANDALONE -isystem $(top_srcdir)/lib/asio/asio/include'
AC_SUBST(libasio_CFLAGS)

AC_ARG_ENABLE(postgres,
    AS_HELP_STRING([--disable-postgres],
        [Disable postgres support even when libpq available]))
unset have_postgres
if test x"$enable_postgres" != xno; then
    PKG_CHECK_MODULES(libpq, libpq, have_postgres=1)
    if test -n "$enable_postgres" -a -z "$have_postgres"; then
       AC_MSG_ERROR([Cannot find postgres library])
    fi
    if test -n "$have_postgres"; then
       backend=
       soci_CFLAGS="$soci_CFLAGS "'-isystem $(top_srcdir)/lib/soci/src/backends/postgresql'
    fi
fi
AM_CONDITIONAL(USE_POSTGRES, [test -n "$have_postgres"])

# Need this to pass through ccache for xdrpp, libsodium
esc() {
    out=
    for arg in "$@"; do
        out="$out${out+ }$(echo "$arg" | sed "s/'/'\\''/g; s/^/'/; s/\$/'/")"
    done
    echo $out
}

# explicitly propagate CFLAGS, CXXFLAGS and LDFLAGS in case they got modified by global options
ac_configure_args="$ac_configure_args $(esc "CC=$CC" "CXX=$CXX" "CFLAGS=$CFLAGS" "CXXFLAGS=$CXXFLAGS" "LDFLAGS=$LDFLAGS")"

AC_CONFIG_HEADERS(config.h)
AC_CONFIG_FILES(lib/Makefile src/Makefile Makefile)
AC_OUTPUT

