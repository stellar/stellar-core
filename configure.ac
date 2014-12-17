# -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([stellard],[0.1],[],[],[http://www.stellar.org])
AM_INIT_AUTOMAKE([-Wall subdir-objects])
AC_CONFIG_SRCDIR([configure.ac])
AC_CONFIG_MACRO_DIR([m4])

dnl AC_PROG_INSTALL

test "${WFLAGS+set}" || WFLAGS="-Wall"
AC_PROG_CC
AC_PROG_CXX([clang++ eg++ g++ c++ gpp aCC CC cxx cc++ cl.exe FCC KCC RCC xlC_r xlC])
AX_CXX_COMPILE_STDCXX_11(noext,mandatory)
test -z "$WFLAGS" || CFLAGS="$CFLAGS $WFLAGS" CXXFLAGS="$CXXFLAGS $WFLAGS"
AC_LANG(C++)
# -pthread Seems to be required by g++ -stc=c++1[14]
AX_APPEND_COMPILE_FLAGS([-pthread])

AC_CHECK_PROGS(CLANG_FORMAT, [clang-format clang-format-3.6])
AM_CONDITIONAL([USE_CLANG_FORMAT], [test "x$CLANG_FORMAT" != "x"])

AC_ARG_ENABLE([address-sanitizer], AS_HELP_STRING([--enable-address-sanitizer], \
              [enable AddressSanitizer support]), \
              [AC_DEFINE(HAVE_ADDRESS_SANITIZER, 1, [enable AddressSanitizer])
              CFLAGS="$CFLAGS -fsanitize=address"
              CXXFLAGS="$CXXFLAGS -fsanitize=address"
              LIBS="$LIBS -lasan"
              AC_TRY_COMPILE([],[const int i=0;],[AC_MSG_NOTICE([Address Sanitizer Enabled])],
                                                 [AC_MSG_ERROR([Address Sanitizer not available])])
              ])


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

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT

