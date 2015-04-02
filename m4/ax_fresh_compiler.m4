# Copyright 2015 Stellar Development Foundation and contributors. Licensed
# under the Apache License, Version 2.0. See the COPYING file at the root
# of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

AC_DEFUN([AX_FRESH_COMPILER],
[
    # Check for compiler versions. We need g++ >= 4.9 and/or clang++ >= 3.5
    AC_LANG_PUSH([C++])
    case "${CXX}" in
        *clang*)
            AC_MSG_CHECKING([for clang >= 3.5])
            AC_COMPILE_IFELSE([AC_LANG_PROGRAM(
                #if __clang_major__ < 3 || (__clang_major__ == 3 && __clang_minor__ < 5)
                #error old clang++
                #endif
                #if defined(__apple_build_version__)
                    #if __clang_major__ < 6
                    #error old clang++
                    #endif
                #endif
            )], [AC_MSG_RESULT(yes)], [AC_MSG_ERROR([clang++ is < required version 3.5])])
        ;;

        *g++*)
            AC_MSG_CHECKING([for g++ >= 4.9])
            AC_COMPILE_IFELSE([AC_LANG_PROGRAM(
                #if __GNUC__ < 4 || (__GNUC__ == 4 && __GNUC_MINOR__ < 9)
                #error old g++
                #endif
            )], [AC_MSG_RESULT(yes)], [AC_MSG_ERROR([g++ is < required version 4.9])])
        ;;
    esac

    # Even if we have clang++ >= 3.5 we need to be sure we don't have an
    # old/broken libstdc++, which we often get if there's an old g++ install
    # kicking around. Unfortunately there's no way to test this directly; we
    # try a handful of tell-tale C++11 features completed in 4.9 instead.
    AC_MSG_CHECKING([for a sufficiently-new libstdc++])
    AC_COMPILE_IFELSE([
        AC_LANG_PROGRAM(
            [
                [#include <cstddef>]
                [#include <new>]
                [#include <exception>]
            ],
            [
                auto a = sizeof(std::max_align_t);
                auto b = std::get_new_handler();
                auto c = std::get_terminate();
                auto d = std::get_unexpected();
            ]
        )],
        [AC_MSG_RESULT(yes)],
        [AC_MSG_ERROR([missing new C++11 features, probably libstdc++ < 4.9])
    ])
    AC_LANG_POP([C++])
])
