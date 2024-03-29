// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/LogSlowExecution.h"
#include "util/Logging.h"
#include <fmt/format.h>
#include <mutex>

namespace
{
std::mutex gLogSlowExecMutex;

// For each log level, keep track of when the last message was and
// the number of skipped messages.
std::map<stellar::LogLevel,
         std::pair<std::chrono::system_clock::time_point, size_t>>
    gLastLogMessage;
}

namespace stellar
{
LogSlowExecution::LogSlowExecution(std::string eventName, Mode mode,
                                   std::string message,
                                   std::chrono::milliseconds threshold)
    : mStart(std::chrono::system_clock::now())
    , mName(std::move(eventName))
    , mMode(mode)
    , mMessage(std::move(message))
    , mThreshold(threshold){};

LogSlowExecution::~LogSlowExecution()
{
    if (mMode == Mode::AUTOMATIC_RAII)
    {
        checkElapsedTime();
    }
}

std::chrono::milliseconds
LogSlowExecution::checkElapsedTime() const
{
    auto finish = std::chrono::system_clock::now();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(finish - mStart);

    if (!mThreshold.count() || elapsed > mThreshold)
    {
        std::lock_guard<std::mutex> guard(gLogSlowExecMutex);

        // Check whether we're 10 times over the threshold to decide the log
        // level.
        LogLevel ll = (elapsed > mThreshold * 10 || !mThreshold.count())
                          ? LogLevel::LVL_WARNING
                          : LogLevel::LVL_DEBUG;

        auto& lastMsgTime = gLastLogMessage[ll].first;
        auto& skippedCnt = gLastLogMessage[ll].second;

        // Only emit a new log message once-per-second (optionally preceded
        // by a summary of how many messages have been dropped since last time).
        if ((finish - lastMsgTime) > std::chrono::seconds(1))
        {
            std::string skipped;
            if (skippedCnt > 0)
            {
                skipped =
                    fmt::format(FMT_STRING(" [skipped: {:d}]"), skippedCnt);
                skippedCnt = 0;
            }
            std::string msg;
            if (mThreshold.count())
            {
                msg = fmt::format(
                    FMT_STRING("'{:s}' {:s} {:f} s{:s}"), mName, mMessage,
                    static_cast<float>(elapsed.count()) / 1000, skipped);
            }
            else
            {
                msg = fmt::format(FMT_STRING("'{:s}' {:s}{:s}"), mName,
                                  mMessage, skipped);
            }
            switch (ll)
            {
            case LogLevel::LVL_WARNING:
                CLOG_WARNING(Perf, "{}", msg);
                break;
            case LogLevel::LVL_DEBUG:
                CLOG_DEBUG(Perf, "{}", msg);
                break;
            default:
                throw std::runtime_error(
                    "Unexpected log level in LogSlowExecution");
            }
            lastMsgTime = finish;
        }

        // If we've emitted this second already, just bump a counter that
        // will be flushed in a summary message at the next second-boundary.
        else
        {
            skippedCnt++;
        }
    }
    return elapsed;
}

RateLimitedLog::RateLimitedLog(std::string eventName, std::string message)
    // No threshold to emit logs regardless
    : LogSlowExecution(eventName, Mode::AUTOMATIC_RAII, message,
                       std::chrono::milliseconds::zero())
{
}
}
