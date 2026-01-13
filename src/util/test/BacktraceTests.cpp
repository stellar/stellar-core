// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "rust/RustBridge.h"
#include "test/Catch2.h"
#include "util/Backtrace.h"

TEST_CASE("backtraces work", "[backtrace]")
{
    auto backtrace = stellar::rust_bridge::capture_cxx_backtrace();
    auto const backtraceStr = std::string(backtrace.c_str());
    REQUIRE(backtraceStr.find("Catch::TestCase::invoke") != std::string::npos);
    stellar::printCurrentBacktrace();
}
