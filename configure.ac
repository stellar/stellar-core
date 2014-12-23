# -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([stellard],[0.1],[],[],[http://www.stellar.org])
AM_INIT_AUTOMAKE([-Wall subdir-objects])
AC_CONFIG_SRCDIR([configure.ac])
AC_CONFIG_MACRO_DIR([m4])

LT_INIT
AC_SUBST(LIBTOOL_DEPS)

LIBSODIUM_MIN_VERSION="1.0.0"

dnl AC_PROG_INSTALL

test "${WFLAGS+set}" || WFLAGS="-Wall"
AC_PROG_CC([clang egcc gcc cc aCC CC cc cl.exe FCC KCC RCC xlC_r xlC])
AC_PROG_CXX([clang++ eg++ g++ c++ gpp aCC CC cxx cc++ cl.exe FCC KCC RCC xlC_r xlC])
AX_CXX_COMPILE_STDCXX_11(noext,mandatory)
test -z "$WFLAGS" || CFLAGS="$CFLAGS $WFLAGS" CXXFLAGS="$CXXFLAGS $WFLAGS"
AC_LANG(C++)
# -pthread Seems to be required by g++ -stc=c++1[14]
AX_APPEND_COMPILE_FLAGS([-pthread])

AC_ARG_WITH([libsodium],
    AS_HELP_STRING([--with-libsodium=[system/internal]]
        [Link against the system supplied or the built-in libsodium.]),
    [], with_libsodium="internal")

AC_ARG_ENABLE([address-sanitizer], AS_HELP_STRING([--enable-address-sanitizer],
              [enable AddressSanitizer support]),
              [AC_DEFINE(HAVE_ADDRESS_SANITIZER, 1, [enable AddressSanitizer])
              CFLAGS="$CFLAGS -fsanitize=address -fno-omit-frame-pointer"
              CXXFLAGS="$CXXFLAGS -fsanitize=address -fno-omit-frame-pointer"
              AC_TRY_COMPILE([],[const int i=0;],[AC_MSG_NOTICE([Address Sanitizer Enabled])],
                                                 [AC_MSG_ERROR([Address Sanitizer not available])])
              ])

AC_CHECK_PROGS(CLANG_FORMAT, [clang-format clang-format-3.6])
AM_CONDITIONAL([USE_CLANG_FORMAT], [test "x$CLANG_FORMAT" != "x"])


for dir in ../xdrpp/build ../xdrpp ./xdrpp; do
    if test -d "$dir"; then
       export PKG_CONFIG_PATH="${dir}:${PKG_CONFIG_PATH}"
       break
    fi
done
PKG_CHECK_MODULES([xdrpp], [xdrpp])
AC_MSG_CHECKING(for xdrc)
XDRC=$(pkg-config --variable=xdrc xdrpp)
AC_MSG_RESULT($XDRC)
AC_SUBST(XDRC)

CPPFLAGS="$CPPFLAGS "'$(xdrpp_CFLAGS)'
LIBS="$LIBS "'$(xdrpp_LIBS)'

AX_VALGRIND_CHECK


# Permit using internal or system variants of libsodium
#
# Note: We can't just use pkg-config here because libsodium does not
# ship with a libsodium-uninstalled.pc variant for linking-to in-tree.
if test -f "$srcdir/src/lib/libsodium/configure.ac"
then
    AC_CONFIG_SUBDIRS([src/lib/libsodium])
    LIBSODIUM_SUBDIRS=src/lib/libsodium
    export PKG_CONFIG_PATH="src/lib/libsodium:${PKG_CONFIG_PATH}"
fi
PKG_CHECK_MODULES(libsodium, libsodium >= $LIBSODIUM_MIN_VERSION,,)


CPPFLAGS="$CPPFLAGS "'$(libsodium_CFLAGS)'
LIBS="$LIBS "'$(libsodium_LIBS)'


AM_CONDITIONAL(LIBSODIUM_INTERNAL, [test "x$LIBSODIUM_SUBDIRS" != "x"])
AC_SUBST(LIBSODIUM_SUBDIRS)

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT

