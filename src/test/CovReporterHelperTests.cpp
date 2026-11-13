// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "CovReporterHelperTests.h"

#include <chrono>
#include <thread>

// disable optimizations as this is used for coverage testing
#ifdef __clang__
#pragma clang optimize off
#endif

std::string
fizzbuzz(int n, int sleepms)
{
    std::string res;
    if (n % 3 == 0)
    {
        res = "fizz";
    }
    if (n % 5 == 0)
    {
        res += "buzz";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepms));
    return res;
}
