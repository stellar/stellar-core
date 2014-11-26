# -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([stellard],[0.1],[],[],[http://www.stellar.org])
AC_CONFIG_SRCDIR([configure.ac])
AC_CONFIG_MACRO_DIR([m4])
AC_PROG_INSTALL

test "${WFLAGS+set}" || WFLAGS="-Wall"
AC_PROG_CXX([clang++ eg++ g++ c++ gpp aCC CC cxx cc++ cl.exe FCC KCC RCC xlC_r xlC])
AX_CXX_COMPILE_STDCXX_11(noext,mandatory)
test -z "$WFLAGS" || CXXFLAGS="$CXXFLAGS $WFLAGS"
AC_LANG(C++)
# -pthread Seems to be required by g++ -stc=c++1[14]
AX_APPEND_COMPILE_FLAGS([-pthread])

DIRS="\
      src/overlay                      \
	  src/generated                    \
	  src/history                      \
	  src/lib                          \
	  src/lib/crypto                   \
	  src/lib/database                 \
	  src/lib/util                     \
	  src/fba                          \
	  src/ledger                       \
	  src/txherder                     \
	  src/clf                          \
	  src/main                         \
	  src/transactions                 \
	  src/util                         \
"

for i in ${DIRS}; do mkdir -p $i; done

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT

