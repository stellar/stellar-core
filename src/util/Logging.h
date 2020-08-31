#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <array>
#include <iostream>

#ifndef USE_EASYLOGGING

#define INITIALIZE_EASYLOGGINGPP
#define CLOG(LEVEL, ...) stellar::CoutLogger(el::Level::LEVEL)
#define LOG(LEVEL) CLOG(LEVEL)

namespace el
{

enum class Level
{
    FATAL = 0,
    ERROR = 1,
    WARNING = 2,
    INFO = 3,
    DEBUG = 4,
    TRACE = 5,

    // Needed for some existing code
    Info = 3
};
}

namespace stellar
{

class CoutLogger
{
    bool const mShouldLog;

  public:
    explicit CoutLogger(el::Level l);

    ~CoutLogger();

    template <typename T>
    CoutLogger&
    operator<<(T const& val)
    {
        if (mShouldLog)
        {
            std::cout << val;
        }
        return *this;
    }
};

class Logging
{
    static el::Level mLogLevel;

  public:
    static void init();
    static void setFmt(std::string const& peerID, bool timestamps = true);
    static void setLoggingToFile(std::string const& filename);
    static void setLogLevel(el::Level level, const char* partition);
    static el::Level getLLfromString(std::string const& levelName);
    static el::Level getLogLevel(std::string const& partition);
    static std::string getStringFromLL(el::Level level);
    static bool logDebug(std::string const& partition);
    static bool logTrace(std::string const& partition);
    static void rotate();
    static std::string normalizePartition(std::string const& partition);

    static std::array<std::string const, 14> const kPartitionNames;
};
}

#else // USE_EASYLOGGING defined

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
}

#endif // USE_EASYLOGGING
