#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#define ELPP_THREAD_SAFE
#define ELPP_DISABLE_DEFAULT_CRASH_HANDLING
#define ELPP_NO_DEFAULT_LOG_FILE
#define ELPP_NO_CHECK_MACROS
#define ELPP_NO_DEBUG_MACROS
#define ELPP_DISABLE_PERFORMANCE_TRACKING
#define ELPP_WINSOCK2
#define ELPP_DEBUG_ERRORS

// NOTE: Nothing else should include easylogging directly include this file
// instead Please think carefully modifying this file, and potentially using
// synchronization primitives. It is easy to introduce data races and deadlocks,
// so it is recommended to use valgrind --tool=helgrind to detect potential
// problems.
#include "lib/util/easylogging++.h"

namespace stellar
{
class Logging
{
    static el::Configurations gDefaultConf;

  public:
    static void init();
    static void setFmt(std::string const& peerID, bool timestamps = true);
    static void setLoggingToFile(std::string const& filename);
    static void setLogLevel(el::Level level, const char* partition);
    static el::Level getLLfromString(std::string const& levelName);
    static el::Level getLogLevel(std::string const& partition);
    static std::string getStringFromLL(el::Level);
    static bool logDebug(std::string const& partition);
    static bool logTrace(std::string const& partition);
    static void rotate();
    // throws if partition name is not recognized
    static std::string normalizePartition(std::string const& partition);

    static std::array<std::string const, 14> const kPartitionNames;
};

// Local wrapper that caches (for a fixed number of lookups) global log-level
// state queries, to avoid taking the global mutex and doing a std::map lookup
// every time you want to check whether a given log level is enabled. Typically
// you want to use this for log-level queries at TRACE level if they are showing
// up in profiles.

class CachedLogLevel
{
    size_t mQueryFreq;
    size_t mQueryCount;
    std::string mPartition;
    el::Level mLastQueryAnswer;

  public:
    CachedLogLevel(std::string const& partition, size_t queryFreq = 1000)
        : mQueryFreq(queryFreq)
        , mQueryCount(0)
        , mPartition(partition)
        , mLastQueryAnswer(Logging::getLogLevel(mPartition))
    {
    }

    el::Level
    getLevel()
    {
        if (++mQueryCount > mQueryFreq)
        {
            mLastQueryAnswer = Logging::getLogLevel(mPartition);
            mQueryCount = 0;
        }
        return mLastQueryAnswer;
    }

    bool
    logDebug()
    {
        auto lev = getLevel();
        return lev == el::Level::Debug || lev == el::Level::Trace;
    }

    bool
    logTrace()
    {
        return getLevel() == el::Level::Trace;
    }
};
}
