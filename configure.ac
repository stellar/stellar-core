# -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([stellard],[0.1],[],[],[http://www.stellar.org])
AM_INIT_AUTOMAKE([-Wall subdir-objects])
AC_CONFIG_SRCDIR([configure.ac])
AC_CONFIG_MACRO_DIR([m4])

test "${WFLAGS+set}" || WFLAGS="-Wall"
AC_PROG_CC([clang gcc cc])
AC_PROG_CXX([clang++ g++ c++])
AM_PROG_AR

LT_INIT([disable-shared])
AC_SUBST(LIBTOOL_DEPS)

LIBSODIUM_MIN_VERSION="1.0.0"

dnl AC_PROG_INSTALL

test -z "$WFLAGS" || CFLAGS="$CFLAGS $WFLAGS" CXXFLAGS="$CXXFLAGS $WFLAGS"

AC_LANG(C++)
AX_CXX_COMPILE_STDCXX_11(noext,mandatory)

# libstdc++ doesn't really seem complete enough for using c++11, need libc++
AX_APPEND_COMPILE_FLAGS([-stdlib=libc++])
if test "${CXXFLAGS#*stdlib}" == "${CXXFLAGS}"
then
    AC_MSG_ERROR([need -stdlib=libc++ for full c++11])
fi

# Our large include path set makes for annoying warnings without this
AX_APPEND_COMPILE_FLAGS([-Wno-unused-command-line-argument])

# -lc++abi seems to be required by -stdlib=libc++ on some hosts
AX_APPEND_LINK_FLAGS([-lc++abi])

# -pthread seems to be required by -std=c++11 on some hosts
AX_APPEND_COMPILE_FLAGS([-pthread])


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
            ./configure $ac_configure_args --srcdir=$ac_srcdir $2 CXX="${CXX}" CC="${CC}" CXXFLAGS="${CXXFLAGS}" LDFLAGS="${LDFLAGS}"
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
PKG_CHECK_MODULES([libpq],[libpq],[use_POSTGRESQL=1],[use_POSTGRESQL=])
AM_CONDITIONAL(USE_POSTGRESQL, [test "x$use_POSTGRESQL" != "x"])

CPPFLAGS="$CPPFLAGS "'$(libsodium_CFLAGS) $(xdrpp_CFLAGS) $(sqlite_CFLAGS) $(libpq_CFLAGS) $(libmedida_CFLAGS)'
LIBS="$LIBS "'$(libsodium_LIBS) $(xdrpp_LIBS) $(sqlite_LIBS) $(libpq_LIBS) $(libmedida_LIBS)'

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT

