// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

namespace stellar
{
bool threadIsMain();
void assertThreadIsMain();

void dbgAbort();

[[noreturn]] void printErrorAndAbort(const char* s1);
[[noreturn]] void printErrorAndAbort(const char* s1, const char* s2);
[[noreturn]] void printAssertFailureAndAbort(const char* s1, const char* file,
                                             int line);
[[noreturn]] void printAssertFailureAndThrow(const char* s1, const char* file,
                                             int line);

// This is like `assert()` but it is _not_ sensitive to the presence of
// NDEBUG. We don't compile with NDEBUG but "compiling out important asserts" is
// enough of a footgun that we want to avoid even the possibility.
// It will also print a backtrace (at least on unix platforms with libunwind).
#define releaseAssert(e) \
    (static_cast<bool>(e) \
         ? void(0) \
         : printAssertFailureAndAbort(#e, __FILE__, __LINE__))

// Same as above, but throwing rather than aborting.
#define releaseAssertOrThrow(e) \
    (static_cast<bool>(e) \
         ? void(0) \
         : printAssertFailureAndThrow(#e, __FILE__, __LINE__))

#ifdef NDEBUG

#define dbgAssert(expression) ((void)0)

#else

#define dbgAssert(expression) (void)((!!(expression)) || (dbgAbort(), 0))

#endif
}
