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
std::chrono::system_clock::time_point gLastLogMessage;
size_t gDroppedLogMessagesSinceLast{0};
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

    if (elapsed > mThreshold)
    {
        std::lock_guard<std::mutex> guard(gLogSlowExecMutex);

        // Only emit a new log message once-per-second (optionally preceeded
        // by a summary of how many messages have been dropped since last time).
        if ((finish - gLastLogMessage) > std::chrono::seconds(1))
        {
            if (gDroppedLogMessagesSinceLast > 0)
            {
                CLOG_WARNING(Perf, "Dropped {} slow-execution warning messages",
                             gDroppedLogMessagesSinceLast);
                gDroppedLogMessagesSinceLast = 0;
            }
            auto msg = fmt::format("'{}' {} {} s", mName, mMessage,
                                   static_cast<float>(elapsed.count()) / 1000);
            // if we're 10 times over the threshold, log with INFO, otherwise
            // use DEBUG
            if (elapsed > mThreshold * 10)
            {
                CLOG_WARNING(Perf, "{}", msg);
            }
            else
            {
                CLOG_DEBUG(Perf, "{}", msg);
            }
            gLastLogMessage = finish;
        }

        // If we've emitted this second already, just bump a counter that
        // will be flushed in a summary message at the next second-boundary.
        else
        {
            gDroppedLogMessagesSinceLast++;
        }
    }
    return elapsed;
}
}
