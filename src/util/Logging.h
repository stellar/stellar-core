#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <array>
#include <iostream>
#include <map>

// Provide support for fmt-strings formatting objects that have
// an overloaded operator<< defined on them.
#include <fmt/ostream.h>

#if defined(USE_SPDLOG)

// Must include this _before_ spdlog.h
#include "util/SpdlogTweaks.h"

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
// in turn use stellar::CoutLogger.

#define CLOG(LEVEL, ...) stellar::CoutLogger(stellar::LogLevel::LEVEL)
#define LOG(LEVEL) CLOG(LEVEL)

#define CLOG_TRACE(partition, f, ...) \
    CLOG(LVL_TRACE, #partition) << fmt::format(FMT_STRING(f), ##__VA_ARGS__)
#define CLOG_DEBUG(partition, f, ...) \
    CLOG(LVL_DEBUG, #partition) << fmt::format(FMT_STRING(f), ##__VA_ARGS__)
#define CLOG_INFO(partition, f, ...) \
    CLOG(LVL_INFO, #partition) << fmt::format(FMT_STRING(f), ##__VA_ARGS__)
#define CLOG_WARNING(partition, f, ...) \
    CLOG(LVL_WARNING, #partition) << fmt::format(FMT_STRING(f), ##__VA_ARGS__)
#define CLOG_ERROR(partition, f, ...) \
    CLOG(LVL_ERROR, #partition) << fmt::format(FMT_STRING(f), ##__VA_ARGS__)
#define CLOG_FATAL(partition, f, ...) \
    CLOG(LVL_FATAL, #partition) << fmt::format(FMT_STRING(f), ##__VA_ARGS__)

#define LOG_TRACE(logger, f, ...) \
    CLOG(LVL_TRACE, logger) << fmt::format(f, ##__VA_ARGS__)
#define LOG_DEBUG(logger, f, ...) \
    CLOG(LVL_DEBUG, logger) << fmt::format(f, ##__VA_ARGS__)
#define LOG_INFO(logger, f, ...) \
    CLOG(LVL_INFO, logger) << fmt::format(f, ##__VA_ARGS__)
#define LOG_WARNING(logger, f, ...) \
    CLOG(LVL_WARNING, logger) << fmt::format(f, ##__VA_ARGS__)
#define LOG_ERROR(logger, f, ...) \
    CLOG(LVL_ERROR, logger) << fmt::format(f, ##__VA_ARGS__)
#define LOG_FATAL(logger, f, ...) \
    CLOG(LVL_FATAL, logger) << fmt::format(f, ##__VA_ARGS__)
#define GET_LOG(name) name
#define DEFAULT_LOG nullptr
namespace stellar
{
typedef void* LogPtr;
}

#endif

namespace stellar
{

enum class LogLevel
{
    LVL_FATAL = 0,
    LVL_ERROR = 1,
    LVL_WARNING = 2,
    LVL_INFO = 3,
    LVL_DEBUG = 4,
    LVL_TRACE = 5
};

class CoutLogger
{
    bool const mShouldLog;

  public:
    explicit CoutLogger(LogLevel l);

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
    static LogLevel mGlobalLogLevel;
    static std::map<std::string, LogLevel> mPartitionLogLevels;
    static std::recursive_mutex mLogMutex;
    static bool mInitialized;
#if defined(USE_SPDLOG)
    static bool mColor;
    static std::string mLastPattern;
    static std::string mLastFilenamePattern;
#define LOG_PARTITION(name) static LogPtr name##LogPtr;
#include "util/LogPartitions.def"
#undef LOG_PARTITION
#endif

  public:
    static void init(bool truncate = false);
    static void deinit();
    static void setFmt(std::string const& peerID, bool timestamps = true);
    static void setLoggingToFile(std::string const& filename);
    static void setLoggingColor(bool color);
    static void setLogLevel(LogLevel level, const char* partition);
    static LogLevel getLLfromString(std::string const& levelName);
    static LogLevel getLogLevel(std::string const& partition);
    static std::string getStringFromLL(LogLevel level);
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
