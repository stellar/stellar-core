#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <array>
#include <iostream>

// Provide support for fmt-strings formatting objects that have
// an overloaded operator<< defined on them.
#include <fmt/ostream.h>

#if defined(USE_SPDLOG)

#include <map>
#include <memory>
#include <spdlog/spdlog.h>

#define LOG_CHECK(logger, level, action) \
    do \
    { \
        auto lg = (logger); \
        if (lg->should_log(level) || lg->should_backtrace()) \
        { \
            action; \
        } \
    } while (false)

#define CLOG_TRACE(partition, f, ...) \
    LOG_CHECK(Logging::get##partition##LogPtr(), spdlog::level::trace, \
              SPDLOG_LOGGER_TRACE(lg, FMT_STRING(f), ##__VA_ARGS__))

#define CLOG_DEBUG(partition, f, ...) \
    LOG_CHECK(Logging::get##partition##LogPtr(), spdlog::level::debug, \
              SPDLOG_LOGGER_DEBUG(lg, FMT_STRING(f), ##__VA_ARGS__))

#define CLOG_INFO(partition, f, ...) \
    LOG_CHECK(Logging::get##partition##LogPtr(), spdlog::level::info, \
              SPDLOG_LOGGER_INFO(lg, FMT_STRING(f), ##__VA_ARGS__))

#define CLOG_WARNING(partition, f, ...) \
    LOG_CHECK(Logging::get##partition##LogPtr(), spdlog::level::warn, \
              SPDLOG_LOGGER_WARN(lg, FMT_STRING(f), ##__VA_ARGS__))

#define CLOG_ERROR(partition, f, ...) \
    LOG_CHECK(Logging::get##partition##LogPtr(), spdlog::level::err, \
              SPDLOG_LOGGER_ERROR(lg, FMT_STRING(f), ##__VA_ARGS__))

#define CLOG_FATAL(partition, f, ...) \
    LOG_CHECK(Logging::get##partition##LogPtr(), spdlog::level::critical, \
              SPDLOG_LOGGER_CRITICAL(lg, FMT_STRING(f), ##__VA_ARGS__))

#define LOG_TRACE(lg, fmt, ...) \
    LOG_CHECK(lg, spdlog::level::trace, \
              SPDLOG_LOGGER_TRACE(lg, FMT_STRING(fmt), ##__VA_ARGS__))
#define LOG_DEBUG(lg, fmt, ...) \
    LOG_CHECK(lg, spdlog::level::debug, \
              SPDLOG_LOGGER_DEBUG(lg, FMT_STRING(fmt), ##__VA_ARGS__))
#define LOG_INFO(lg, fmt, ...) \
    LOG_CHECK(lg, spdlog::level::info, \
              SPDLOG_LOGGER_INFO(lg, FMT_STRING(fmt), ##__VA_ARGS__))
#define LOG_WARNING(lg, fmt, ...) \
    LOG_CHECK(lg, spdlog::level::warn, \
              SPDLOG_LOGGER_WARN(lg, FMT_STRING(fmt), ##__VA_ARGS__))
#define LOG_ERROR(lg, fmt, ...) \
    LOG_CHECK(lg, spdlog::level::err, \
              SPDLOG_LOGGER_ERROR(lg, FMT_STRING(fmt), ##__VA_ARGS__))
#define LOG_FATAL(lg, fmt, ...) \
    LOG_CHECK(lg, spdlog::level::critical, \
              SPDLOG_LOGGER_CRITICAL(lg, FMT_STRING(fmt), ##__VA_ARGS__))

#define GET_LOG(name) spdlog::get(name)
#define DEFAULT_LOG spdlog::default_logger()
namespace stellar
{
typedef std::shared_ptr<spdlog::logger> LogPtr;
}

#else
// No spdlog either: delegate back to old logging interface, which will
// in turn either use easylogging or stellar::CoutLogger.
//
// Note: all this is temporary while evaluating; when we commit to a new logging
// interface we'll remove all this plumbing.

#define CLOG_TRACE(partition, f, ...) \
    CLOG(TRACE, #partition) << fmt::format(FMT_STRING(f), ##__VA_ARGS__)
#define CLOG_DEBUG(partition, f, ...) \
    CLOG(DEBUG, #partition) << fmt::format(FMT_STRING(f), ##__VA_ARGS__)
#define CLOG_INFO(partition, f, ...) \
    CLOG(INFO, #partition) << fmt::format(FMT_STRING(f), ##__VA_ARGS__)
#define CLOG_WARNING(partition, f, ...) \
    CLOG(WARNING, #partition) << fmt::format(FMT_STRING(f), ##__VA_ARGS__)
#define CLOG_ERROR(partition, f, ...) \
    CLOG(ERROR, #partition) << fmt::format(FMT_STRING(f), ##__VA_ARGS__)
#define CLOG_FATAL(partition, f, ...) \
    CLOG(FATAL, #partition) << fmt::format(FMT_STRING(f), ##__VA_ARGS__)

#define LOG_TRACE(logger, f, ...) \
    CLOG(TRACE, logger) << fmt::format(f, ##__VA_ARGS__)
#define LOG_DEBUG(logger, f, ...) \
    CLOG(DEBUG, logger) << fmt::format(f, ##__VA_ARGS__)
#define LOG_INFO(logger, f, ...) \
    CLOG(INFO, logger) << fmt::format(f, ##__VA_ARGS__)
#define LOG_WARNING(logger, f, ...) \
    CLOG(WARNING, logger) << fmt::format(f, ##__VA_ARGS__)
#define LOG_ERROR(logger, f, ...) \
    CLOG(ERROR, logger) << fmt::format(f, ##__VA_ARGS__)
#define LOG_FATAL(logger, f, ...) \
    CLOG(FATAL, logger) << fmt::format(f, ##__VA_ARGS__)
#define GET_LOG(name) name
#define DEFAULT_LOG ELPP_CURR_FILE_LOGGER_ID
namespace stellar
{
typedef void* LogPtr;
}

#endif

#ifndef USE_EASYLOGGING

#define INITIALIZE_EASYLOGGINGPP
#define CLOG(LEVEL, ...) stellar::CoutLogger(el::Level::LEVEL)
#define LOG(LEVEL) CLOG(LEVEL)
#define ELPP_CURR_FILE_LOGGER_ID nullptr

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
    static el::Level mGlobalLogLevel;
    static std::map<std::string, el::Level> mPartitionLogLevels;
    static std::recursive_mutex mLogMutex;
    static bool mInitialized;
#if defined(USE_SPDLOG)
    static bool mColor;
    static std::string mLastPattern;
    static std::string mLastFilename;
#define LOG_PARTITION(name) static LogPtr name##LogPtr;
#include "util/LogPartitions.def"
#undef LOG_PARTITION
#endif

  public:
    static void init();
    static void deinit();
    static void setFmt(std::string const& peerID, bool timestamps = true);
    static void setLoggingToFile(std::string const& filename);
    static void setLoggingColor(bool color);
    static void setLogLevel(el::Level level, const char* partition);
    static el::Level getLLfromString(std::string const& levelName);
    static el::Level getLogLevel(std::string const& partition);
    static std::string getStringFromLL(el::Level level);
    static bool logDebug(std::string const& partition);
    static bool logTrace(std::string const& partition);
    static void rotate();
    static std::string normalizePartition(std::string const& partition);

    static std::array<std::string const, 14> const kPartitionNames;

#if defined(USE_SPDLOG)
#define LOG_PARTITION(name) static LogPtr get##name##LogPtr();
#include "util/LogPartitions.def"
#undef LOG_PARTITION
#endif
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
    static el::Level mLogLevel;
    static el::Configurations gDefaultConf;
    static bool gAnyDebug;
    static bool gAnyTrace;

  public:
    static void init();
    static void setFmt(std::string const& peerID, bool timestamps = true);
    static void setLoggingToFile(std::string const& filename);
    static void setLoggingColor(bool color);
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
