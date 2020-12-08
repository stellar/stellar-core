// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "GlobalChecks.h"
#include "Backtrace.h"

#ifdef _WIN32
#include <Windows.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <thread>

namespace stellar
{
static std::thread::id mainThread = std::this_thread::get_id();

bool
threadIsMain()
{
    return mainThread == std::this_thread::get_id();
}

void
assertThreadIsMain()
{
    dbgAssert(threadIsMain());
}

void
dbgAbort()
{
#ifdef _WIN32
    DebugBreak();
#else
    std::abort();
#endif
}

void
printErrorAndAbort(const char* s1)
{
    std::fprintf(stderr, "%s\n", s1);
    std::fflush(stderr);
    printCurrentBacktrace();
    dbgAbort();
    std::abort();
}

void
printErrorAndAbort(const char* s1, const char* s2)
{
    std::fprintf(stderr, "%s%s\n", s1, s2);
    std::fflush(stderr);
    printCurrentBacktrace();
    dbgAbort();
    std::abort();
}

void
printAssertFailureAndAbort(const char* s1, const char* file, int line)
{
    std::fprintf(stderr, "%s at %s:%d\n", s1, file, line);
    std::fflush(stderr);
    printCurrentBacktrace();
    dbgAbort();
    std::abort();
}

void
printAssertFailureAndThrow(const char* s1, const char* file, int line)
{
    std::fprintf(stderr, "%s at %s:%d\n", s1, file, line);
    std::fflush(stderr);
    printCurrentBacktrace();
    throw std::runtime_error(s1);
}
}
