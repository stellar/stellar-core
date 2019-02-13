// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "util/Logging.h"

#include <chrono>

namespace stellar
{

std::chrono::milliseconds
logSlowExecution(std::chrono::system_clock::time_point start,
                 std::string eventName, std::string message = "hung for",
                 std::chrono::milliseconds threshold = std::chrono::seconds(1));

class LogSlowExecution
{
  public:
    LogSlowExecution(
        std::string eventName,
        std::chrono::milliseconds threshold = std::chrono::seconds(1))
        : mStart(std::chrono::system_clock::now())
        , mEventName(std::move(eventName))
        , mThreshold(threshold)
    {
    }

    ~LogSlowExecution()
    {
        logSlowExecution(mStart, mEventName, "hung for", mThreshold);
    }

  private:
    std::chrono::system_clock::time_point mStart;
    std::string mEventName;
    std::chrono::milliseconds mThreshold;
};
}
