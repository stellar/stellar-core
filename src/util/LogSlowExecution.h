// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "util/Logging.h"
#include <chrono>

class LogSlowExecution
{

    std::chrono::system_clock::time_point mStart;
    std::string mName;
    int mThresholdInMs;

  public:
    LogSlowExecution(std::string eventName, int ms = 1000)
        : mStart(std::chrono::system_clock::now())
        , mName(std::move(eventName))
        , mThresholdInMs(ms){};

    ~LogSlowExecution()
    {
        auto finish = std::chrono::system_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            finish - mStart);
        auto tooSlow = elapsed.count() > mThresholdInMs;

        LOG_IF(tooSlow, INFO)
            << mName << " hung for "
            << static_cast<float>(elapsed.count()) / 1000 << " s";
    }
};
