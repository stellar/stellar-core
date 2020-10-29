// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <chrono>
#include <string>

namespace stellar
{
class LogSlowExecution
{
  public:
    enum class Mode
    {
        AUTOMATIC_RAII,
        MANUAL // In this mode, it is the caller's responsibility to check
               // elapsed time
    };

    LogSlowExecution(
        std::string eventName, Mode mode = Mode::AUTOMATIC_RAII,
        std::string message = "took",
        std::chrono::milliseconds threshold = std::chrono::seconds(1));
    ~LogSlowExecution();
    std::chrono::milliseconds checkElapsedTime() const;

  private:
    std::chrono::system_clock::time_point mStart;
    std::string mName;
    Mode mMode;
    std::string mMessage;
    std::chrono::milliseconds mThreshold;
};
}
