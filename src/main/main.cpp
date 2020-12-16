// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/CommandLine.h"
#include "util/Backtrace.h"
#include "util/Logging.h"

#include "crypto/ShortHash.h"
#include <cstdlib>
#include <exception>
#include <sodium/core.h>
#include <xdrpp/marshal.h>

namespace stellar
{
static void
printBacktraceAndAbort()
{
    printCurrentBacktrace();
    std::abort();
}

static void
outOfMemory()
{
    std::fprintf(stderr, "Unable to allocate memory\n");
    std::fflush(stderr);
    printBacktraceAndAbort();
}
}

int
main(int argc, char* const* argv)
{
    using namespace stellar;
    BacktraceManager btGuard;

    // Abort when out of memory
    std::set_new_handler(outOfMemory);
    // At least print a backtrace in any circumstance
    // that would call std::terminate
    std::set_terminate(printBacktraceAndAbort);

    Logging::init();
    if (sodium_init() != 0)
    {
        LOG_FATAL(DEFAULT_LOG, "Could not initialize crypto");
        return 1;
    }
    shortHash::initialize();

    xdr::marshaling_stack_limit = 1000;

    return handleCommandLine(argc, argv);
}
