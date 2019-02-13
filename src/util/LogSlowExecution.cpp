// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/LogSlowExecution.h"

namespace stellar
{

std::chrono::milliseconds
logSlowExecution(std::chrono::system_clock::time_point start,
                 std::string eventName, std::string message,
                 std::chrono::milliseconds threshold)
{
    auto finish = std::chrono::system_clock::now();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(finish - start);
    auto tooSlow = elapsed > threshold;

    LOG_IF(tooSlow, INFO) << "'" << eventName << "' " << message << " "
                          << static_cast<float>(elapsed.count()) / 1000 << " s";
    return elapsed;
}
}
