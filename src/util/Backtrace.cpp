// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Backtrace.h"
#include "rust/RustBridge.h"
#include "util/GlobalChecks.h"
#include <cstdio>
#include <cstdlib>
#include <string>

namespace stellar
{
void
printCurrentBacktrace()
{
    if (!threadIsMain())
    {
        return;
    }

    if (getenv("STELLAR_NO_BACKTRACE") != nullptr)
    {
        return;
    }

    auto backtrace = rust_bridge::capture_cxx_backtrace();
    if (backtrace.empty())
    {
        fprintf(stderr, "backtrace unavailable\n");
        return;
    }

    fprintf(stderr, "backtrace:\n%s\n", backtrace.c_str());
    fflush(stderr);
}
}
