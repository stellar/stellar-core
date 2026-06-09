// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Logging.h"
#include "util/types.h"

#if defined(USE_SPDLOG)
#include "util/Timer.h"
#include <chrono>
#include <fmt/chrono.h>
#include <fstream>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>
#endif

#include "rust/RustBridge.h"

namespace stellar
{

std::array<std::string const, 15> const Logging::kPartitionNames = {
#define LOG_PARTITION(name) #name,
#include "util/LogPartitions.def"
#undef LOG_PARTITION
};

LogLevel Logging::mGlobalLogLevel = LogLevel::LVL_INFO;
std::map<std::string, LogLevel> Logging::mPartitionLogLevels;

#if defined(USE_SPDLOG)
bool Logging::mInitialized = false;
bool Logging::mColor = false;
std::string Logging::mLastPattern;
std::string Logging::mLastFilenamePattern;
bool Logging::mLogToConsole = true;
#endif

// Right now this is hard-coded to log messages at least as important as INFO
CoutLogger::CoutLogger(LogLevel l) : mShouldLog(l <= Logging::getLogLevel(""))
{
}

CoutLogger::~CoutLogger()
{
    if (mShouldLog)
    {
        std::cout << std::endl;
    }
}

#if defined(USE_SPDLOG)
static spdlog::level::level_enum
convert_loglevel(LogLevel level)
{
    auto slev = spdlog::level::info;
    switch (level)
    {
    case LogLevel::LVL_FATAL:
        slev = spdlog::level::critical;
        break;
    case LogLevel::LVL_ERROR:
        slev = spdlog::level::err;
        break;
    case LogLevel::LVL_WARNING:
        slev = spdlog::level::warn;
        break;
    case LogLevel::LVL_INFO:
        slev = spdlog::level::info;
        break;
    case LogLevel::LVL_DEBUG:
        slev = spdlog::level::debug;
        break;
    case LogLevel::LVL_TRACE:
        slev = spdlog::level::trace;
        break;
    default:
        break;
    }
    return slev;
}

namespace
{
// Each partition (plus "default") has a single *permanent* logger, created
// lazily and never destroyed or re-created for the lifetime of the process.
// The logger's only sink is a dist_sink_mt whose child sinks (console/file)
// are swapped in place, under that sink's own mutex, whenever logging is
// (re)configured by init()/deinit() and friends.
//
// This makes the per-partition getters (and thus every CLOG_* call site)
// lock-free: they return a reference to a shared_ptr that never changes, so
// the hot "is this level enabled" check costs two loads and touches no
// shared mutable state. The previous design memoized pointers to loggers
// that were dropped and re-created on every reconfiguration, which required
// taking a global recursive mutex (and copying a contended shared_ptr) on
// every logging macro invocation, even for disabled levels - measurably
// serializing concurrently-running threads.
struct PermanentLogger
{
    std::shared_ptr<spdlog::sinks::dist_sink_mt> mSink;
    LogPtr mLogger;
};

PermanentLogger
makePermanentLogger(std::string const& name, bool isDefault)
{
    auto sink = std::make_shared<spdlog::sinks::dist_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>(name, sink);
    if (isDefault)
    {
        // Logging through DEFAULT_LOG before Logging::init() goes to the
        // console, matching spdlog's own auto-created default logger.
        sink->add_sink(
            std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        // This also registers the logger in the spdlog registry.
        spdlog::set_default_logger(logger);
    }
    else
    {
        spdlog::register_logger(logger);
    }
    return PermanentLogger{std::move(sink), std::move(logger)};
}

PermanentLogger&
defaultPermanentLogger()
{
    static PermanentLogger pl = makePermanentLogger("default", true);
    return pl;
}

#define LOG_PARTITION(name) \
    PermanentLogger& name##PermanentLogger() \
    { \
        static PermanentLogger pl = makePermanentLogger(#name, false); \
        return pl; \
    }
#include "util/LogPartitions.def"
#undef LOG_PARTITION

// NB: forces creation (and spdlog-registry registration) of every permanent
// logger, so registry-wide operations like spdlog::set_pattern and
// spdlog::set_level cover all of them deterministically.
std::vector<PermanentLogger*>
allPermanentLoggers()
{
    std::vector<PermanentLogger*> loggers;
    loggers.push_back(&defaultPermanentLogger());
#define LOG_PARTITION(name) loggers.push_back(&name##PermanentLogger());
#include "util/LogPartitions.def"
#undef LOG_PARTITION
    return loggers;
}
}
#endif

void
Logging::init(bool truncate)
{
#if defined(USE_SPDLOG)
    std::lock_guard<std::recursive_mutex> guard(mLogMutex);
    if (!mInitialized)
    {
        using namespace spdlog::sinks;
        using std::make_shared;
        using std::shared_ptr;
        std::vector<shared_ptr<sink>> sinks;

        if (mLogToConsole)
        {
            auto console = (mColor ? static_cast<shared_ptr<sink>>(
                                         make_shared<stderr_color_sink_mt>())
                                   : static_cast<shared_ptr<sink>>(
                                         make_shared<stderr_sink_mt>()));
            sinks.emplace_back(console);
        }

        if (!mLastFilenamePattern.empty())
        {
            VirtualClock clock(VirtualClock::REAL_TIME);
            std::time_t time = VirtualClock::to_time_t(clock.system_now());
            auto filename =
                fmt::format(fmt::runtime(mLastFilenamePattern),
                            fmt::arg("datetime", *std::localtime(&time)));

            // NB: We do _not_ pass 'truncate' through to spdlog here -- spdlog
            // interprets 'truncate=true' as a request to open the file in "wb"
            // mode rather than "ab" mode. Opening in "wb" mode does truncate
            // the file but also sets it to a fundamentally different _mode_
            // than "ab" mode.
            //
            // In particular, in "ab" mode the underlying file descriptor is
            // opened with O_APPEND and this makes the kernel atomically preface
            // any write(fd, buf, n) with an lseek(fd, SEEK_END, 0), adjusting
            // the fd's offset to the current end of file, _even if the file
            // shrank_ since the last write.
            //
            // In contrast, in "wb" mode the underlying file descriptor just
            // increments its offset after each write, and if the file shrinks
            // between writes the space between the actual end of file and the
            // offset at the time of writing will be zero-filled!
            //
            // Why would a file shrink? Because users might choose to use an
            // _external_ log-rotation program such as logrotate(1) rather than
            // our internal log rotation command. This command happens to call
            // truncate(2) on the log file after it's made a copy for rotation
            // purposes. This is fine if we're writing to an fd with O_APPEND
            // but it's a recipe for giant zero-filled files if we're not.
            //
            // So instead we just truncate ourselves here if it was requested,
            // and always pass 'truncate=false' to spdlog, so it always opens in
            // "ab" == O_APPEND mode. The "portable" way to truncate is to open
            // an ofstream in out|trunc mode, then immediately close it.
            std::ofstream out;
            if (truncate)
            {
                out.open(filename, std::ios_base::out | std::ios_base::trunc);
            }
            else
            {
                out.open(filename, std::ios_base::out | std::ios_base::app);
            }

            if (out.fail())
            {
                throw std::runtime_error(fmt::format(
                    FMT_STRING(
                        "Could not open log file {}, check access rights"),
                    filename));
            }
            else
            {
                out.close();
            }

            sinks.emplace_back(
                make_shared<basic_file_sink_mt>(filename, /*truncate=*/false));
        }

        // Attach the configured sinks to the permanent loggers in place
        // (under each dist sink's own mutex); this is safe to do while other
        // threads are concurrently logging through them.
        for (auto* permanentLogger : allPermanentLoggers())
        {
            permanentLogger->mSink->set_sinks(sinks);
        }
        if (mLastPattern.empty())
        {
            mLastPattern = "%Y-%m-%dT%H:%M:%S.%e [%^%n %l%$] %v";
        }
        auto maxLevel = mGlobalLogLevel;
        spdlog::set_pattern(mLastPattern);
        spdlog::set_level(convert_loglevel(mGlobalLogLevel));
        for (auto const& pair : mPartitionLogLevels)
        {
            maxLevel = std::max(maxLevel, pair.second);
            spdlog::get(pair.first)->set_level(convert_loglevel(pair.second));
        }
        spdlog::flush_every(std::chrono::seconds(1));
        spdlog::flush_on(spdlog::level::err);
        rust_bridge::init_logging(maxLevel);
        mInitialized = true;
    }
#endif
}

void
Logging::deinit()
{
#if defined(USE_SPDLOG)
    std::lock_guard<std::recursive_mutex> guard(mLogMutex);
    if (mInitialized)
    {
        // Detach the console/file sinks from the permanent loggers (which
        // closes the log file once the last reference drops). The loggers
        // themselves are never destroyed - call sites may hold cached
        // pointers to them - they just stop writing anywhere until the next
        // init().
        for (auto* permanentLogger : allPermanentLoggers())
        {
            permanentLogger->mSink->flush();
            permanentLogger->mSink->set_sinks({});
        }
        mInitialized = false;
    }
#endif
}

void
Logging::setFmt(std::string const& peerID, bool timestamps)
{
#if defined(USE_SPDLOG)
    auto pattern =
        fmt::format("%Y-%m-%dT%H:%M:%S.%e {} [%^%n %l%$] %v", peerID);
    std::lock_guard<std::recursive_mutex> guard(mLogMutex);
    if (pattern != mLastPattern)
    {
        init();
        mLastPattern = std::move(pattern);
        spdlog::set_pattern(mLastPattern);
    }
#endif
}

void
Logging::setLoggingToConsole(bool console)
{
#if defined(USE_SPDLOG)
    std::lock_guard<std::recursive_mutex> guard(mLogMutex);
    mLogToConsole = console;
    deinit();
    init();
#endif
}

void
Logging::setLoggingToFile(std::string const& filename)
{
#if defined(USE_SPDLOG)
    std::lock_guard<std::recursive_mutex> guard(mLogMutex);
    mLastFilenamePattern = filename;
    deinit();
    try
    {
        init();
    }
    catch (std::runtime_error const&)
    {
        // Could not initialize logging to file, fallback on
        // console-only logging and throw
        mLastFilenamePattern.clear();
        mLogToConsole = true;
        deinit();
        init();
        throw;
    }
#endif
}

void
Logging::setLoggingColor(bool color)
{
#if defined(USE_SPDLOG)
    std::lock_guard<std::recursive_mutex> guard(mLogMutex);
    mColor = true;
    deinit();
    init();
#endif
}

void
Logging::setLogLevel(LogLevel level, char const* partition)
{
    std::lock_guard<std::recursive_mutex> guard(mLogMutex);
    if (partition)
    {
        mPartitionLogLevels[partition] = level;
    }
    else
    {
        mGlobalLogLevel = level;
        mPartitionLogLevels.clear();
    }
#if defined(USE_SPDLOG)
    deinit();
    init();
    auto slev = convert_loglevel(level);
    if (partition)
    {
        spdlog::get(partition)->set_level(slev);
    }
    else
    {
        spdlog::set_level(slev);
    }
#endif
}

LogLevel
Logging::getLLfromString(std::string const& levelName)
{
    if (iequals(levelName, "fatal"))
    {
        return LogLevel::LVL_FATAL;
    }

    if (iequals(levelName, "error"))
    {
        return LogLevel::LVL_ERROR;
    }

    if (iequals(levelName, "warning"))
    {
        return LogLevel::LVL_WARNING;
    }

    if (iequals(levelName, "debug"))
    {
        return LogLevel::LVL_DEBUG;
    }

    if (iequals(levelName, "trace"))
    {
        return LogLevel::LVL_TRACE;
    }

    return LogLevel::LVL_INFO;
}

LogLevel
Logging::getLogLevel(std::string const& partition)
{
    std::lock_guard<std::recursive_mutex> guard(mLogMutex);
    auto p = mPartitionLogLevels.find(partition);
    if (p != mPartitionLogLevels.end())
    {
        return p->second;
    }
    return mGlobalLogLevel;
}

std::string
Logging::getStringFromLL(LogLevel level)
{
    switch (level)
    {
    case LogLevel::LVL_FATAL:
        return "Fatal";
    case LogLevel::LVL_ERROR:
        return "Error";
    case LogLevel::LVL_WARNING:
        return "Warning";
    case LogLevel::LVL_INFO:
        return "Info";
    case LogLevel::LVL_DEBUG:
        return "Debug";
    case LogLevel::LVL_TRACE:
        return "Trace";
    }
    return "????";
}

bool
Logging::logDebug(std::string const& partition)
{
    return isLogLevelAtLeast(partition, LogLevel::LVL_DEBUG);
}

bool
Logging::logTrace(std::string const& partition)
{
    return isLogLevelAtLeast(partition, LogLevel::LVL_TRACE);
}

bool
Logging::isLogLevelAtLeast(std::string const& partition, LogLevel level)
{
#if defined(USE_SPDLOG)
    // Read the (atomic) level of the permanent partition logger instead of
    // consulting the level maps under the global mutex: this function is
    // called from hot paths on concurrently-running threads. The logger
    // levels are kept in sync with the maps by setLogLevel()/init().
    auto slev = convert_loglevel(level);
#define LOG_PARTITION(name) \
    if (partition == #name) \
    { \
        return name##PermanentLogger().mLogger->should_log(slev); \
    }
#include "util/LogPartitions.def"
#undef LOG_PARTITION
    return defaultPermanentLogger().mLogger->should_log(slev);
#else
    std::lock_guard<std::recursive_mutex> guard(mLogMutex);
    auto it = mPartitionLogLevels.find(partition);
    if (it != mPartitionLogLevels.end())
    {
        return it->second >= level;
    }
    return mGlobalLogLevel >= level;
#endif
}

void
Logging::rotate()
{
    std::lock_guard<std::recursive_mutex> guard(mLogMutex);
    deinit();
    init(/*truncate=*/true);
}

// throws if partition name is not recognized
std::string
Logging::normalizePartition(std::string const& partition)
{
    for (auto& p : kPartitionNames)
    {
        if (iequals(partition, p))
        {
            return p;
        }
    }
    throw std::invalid_argument("not a valid partition");
}

std::recursive_mutex Logging::mLogMutex;

#if defined(USE_SPDLOG)
// NB: these are called by every CLOG_* macro invocation (including ones for
// disabled levels) and must remain lock-free and write-free: they return a
// reference to a shared_ptr that is set up once and never changes.
LogPtr const&
Logging::getDefaultLogPtr()
{
    return defaultPermanentLogger().mLogger;
}
#define LOG_PARTITION(name) \
    LogPtr const& Logging::get##name##LogPtr() \
    { \
        return name##PermanentLogger().mLogger; \
    }
#include "util/LogPartitions.def"
#undef LOG_PARTITION
#endif

void
Logging::logAtPartitionAndLevel(std::string const& partition, LogLevel level,
                                std::string const& msg)
{
#if defined(USE_SPDLOG)
    auto lev = convert_loglevel(level);
#define LOG_PARTITION(name) \
    if (partition == #name) \
    { \
        LOG_CHECK(Logging::get##name##LogPtr(), lev, lg->log(lev, msg)); \
        return; \
    }
#include "util/LogPartitions.def"
#undef LOG_PARTITION
    LOG_CHECK(spdlog::default_logger(), lev, lg->log(lev, msg));
#else
    CoutLogger logger(level) << msg;
#endif
}
}
