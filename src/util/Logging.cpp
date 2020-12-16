// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#ifndef USE_EASYLOGGING

#include "util/Logging.h"
#include "util/types.h"

#if defined(USE_SPDLOG)
#include "util/Timer.h"
#include <chrono>
#include <fmt/chrono.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>
#endif

namespace stellar
{

std::array<std::string const, 14> const Logging::kPartitionNames = {
#define LOG_PARTITION(name) #name,
#include "util/LogPartitions.def"
#undef LOG_PARTITION
};

el::Level Logging::mGlobalLogLevel = el::Level::INFO;
std::map<std::string, el::Level> Logging::mPartitionLogLevels;

#if defined(USE_SPDLOG)
bool Logging::mInitialized = false;
bool Logging::mColor = false;
std::string Logging::mLastPattern;
std::string Logging::mLastFilename;
#endif

// Right now this is hard-coded to log messages at least as important as INFO
CoutLogger::CoutLogger(el::Level l) : mShouldLog(l <= Logging::getLogLevel(""))
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
convert_loglevel(el::Level level)
{
    auto slev = spdlog::level::info;
    switch (level)
    {
    case el::Level::FATAL:
        slev = spdlog::level::critical;
        break;
    case el::Level::ERROR:
        slev = spdlog::level::err;
        break;
    case el::Level::WARNING:
        slev = spdlog::level::warn;
        break;
    case el::Level::INFO:
        slev = spdlog::level::info;
        break;
    case el::Level::DEBUG:
        slev = spdlog::level::debug;
        break;
    case el::Level::TRACE:
        slev = spdlog::level::trace;
        break;
    default:
        break;
    }
    return slev;
}
#endif

void
Logging::init()
{
#if defined(USE_SPDLOG)
    std::lock_guard<std::recursive_mutex> guard(mLogMutex);
    if (!mInitialized)
    {
        using namespace spdlog::sinks;
        using std::make_shared;
        using std::shared_ptr;
        std::string filename = mLastFilename;
        if (filename.empty())
        {
            VirtualClock clock(VirtualClock::REAL_TIME);
            std::time_t time = VirtualClock::to_time_t(clock.system_now());
            filename = fmt::format("stellar-core.{:%Y.%m.%d-%H:%M:%S}.log",
                                   fmt::localtime(time));
        }

        auto console = (mColor ? static_cast<shared_ptr<sink>>(
                                     make_shared<stdout_color_sink_mt>())
                               : static_cast<shared_ptr<sink>>(
                                     make_shared<stdout_sink_mt>()));

        auto file =
            make_shared<basic_file_sink_mt>(filename, /*truncate=*/true);

        std::vector<shared_ptr<sink>> sinks{console, file};

        auto makeLogger =
            [&](std::string const& name) -> shared_ptr<spdlog::logger> {
            auto logger =
                make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
            spdlog::register_logger(logger);
            return logger;
        };

        spdlog::set_default_logger(makeLogger("default"));
        for (auto const& partition : stellar::Logging::kPartitionNames)
        {
            makeLogger(partition);
        }
        if (mLastPattern.empty())
        {
            mLastPattern = "%Y-%m-%dT%H:%M:%S.%e [%^%n %l%$] %v";
        }
        spdlog::set_pattern(mLastPattern);
        spdlog::set_level(convert_loglevel(mGlobalLogLevel));
        for (auto const& pair : mPartitionLogLevels)
        {
            spdlog::get(pair.first)->set_level(convert_loglevel(pair.second));
        }
        spdlog::flush_every(std::chrono::seconds(1));
        spdlog::flush_on(spdlog::level::err);
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
#define LOG_PARTITION(name) Logging::name##LogPtr = nullptr;
#include "util/LogPartitions.def"
#undef LOG_PARTITION
        spdlog::drop_all();
        mInitialized = false;
    }
#endif
}

void
Logging::setFmt(std::string const& peerID, bool timestamps)
{
#if defined(USE_SPDLOG)
    std::lock_guard<std::recursive_mutex> guard(mLogMutex);
    init();
    mLastPattern = std::string("%Y-%m-%dT%H:%M:%S.%e ") + peerID +
                   std::string(" [%^%n %l%$] %v");
    spdlog::set_pattern(mLastPattern);
#endif
}

void
Logging::setLoggingToFile(std::string const& filename)
{
#if defined(USE_SPDLOG)
    std::lock_guard<std::recursive_mutex> guard(mLogMutex);
    mLastFilename = filename;
    deinit();
    init();
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
Logging::setLogLevel(el::Level level, const char* partition)
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

el::Level
Logging::getLLfromString(std::string const& levelName)
{
    if (iequals(levelName, "fatal"))
    {
        return el::Level::FATAL;
    }

    if (iequals(levelName, "error"))
    {
        return el::Level::ERROR;
    }

    if (iequals(levelName, "warning"))
    {
        return el::Level::WARNING;
    }

    if (iequals(levelName, "debug"))
    {
        return el::Level::DEBUG;
    }

    if (iequals(levelName, "trace"))
    {
        return el::Level::TRACE;
    }

    return el::Level::INFO;
}

el::Level
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
Logging::getStringFromLL(el::Level level)
{
    switch (level)
    {
    case el::Level::FATAL:
        return "Fatal";
    case el::Level::ERROR:
        return "Error";
    case el::Level::WARNING:
        return "Warning";
    case el::Level::INFO:
        return "Info";
    case el::Level::DEBUG:
        return "Debug";
    case el::Level::TRACE:
        return "Trace";
    }
    return "????";
}

bool
Logging::logDebug(std::string const& partition)
{
    std::lock_guard<std::recursive_mutex> guard(mLogMutex);
    return mGlobalLogLevel <= el::Level::DEBUG;
}

bool
Logging::logTrace(std::string const& partition)
{
    std::lock_guard<std::recursive_mutex> guard(mLogMutex);
    return mGlobalLogLevel <= el::Level::TRACE;
}

void
Logging::rotate()
{
    std::lock_guard<std::recursive_mutex> guard(mLogMutex);
    deinit();
    init();
}

// throws if partition name is not recognized
std::string
Logging::normalizePartition(std::string const& partition)
{
    return partition;
}

std::recursive_mutex Logging::mLogMutex;

#if defined(USE_SPDLOG)
#define LOG_PARTITION(name) \
    LogPtr Logging::name##LogPtr = nullptr; \
    LogPtr Logging::get##name##LogPtr() \
    { \
        std::lock_guard<std::recursive_mutex> guard(mLogMutex); \
        if (!name##LogPtr) \
        { \
            name##LogPtr = spdlog::get(#name); \
        } \
        return name##LogPtr; \
    }
#include "util/LogPartitions.def"
#undef LOG_PARTITION
#endif
}

#else // USE_EASYLOGGING defined

#include "main/Application.h"
#include "util/Logging.h"
#include "util/types.h"
#include <list>

/*
Levels:
    TRACE
    DEBUG
    FATAL
    ERROR
    WARNING
    INFO
*/

namespace stellar
{

std::array<std::string const, 14> const Logging::kPartitionNames = {
#define LOG_PARTITION(name) #name,
#include "util/LogPartitions.def"
#undef LOG_PARTITION
};

el::Configurations Logging::gDefaultConf;
bool Logging::gAnyDebug = false;
bool Logging::gAnyTrace = false;

template <typename T> class LockElObject : public NonMovableOrCopyable
{
    T* const mItem;

  public:
    explicit LockElObject(T* elObj) : mItem{elObj}
    {
        assert(mItem);
        static_assert(std::is_base_of<el::base::threading::ThreadSafe,
                                      std::remove_cv_t<T>>::value,
                      "ThreadSafe (easylogging) param required");
        mItem->acquireLock();
    }

    ~LockElObject()
    {
        mItem->releaseLock();
    }
};

class LockHelper
{
    // The declaration order is important, as this is reverse
    // destruction order (loggers release locks first, followed
    // by "registered loggers" object)
    std::unique_ptr<LockElObject<el::base::RegisteredLoggers>>
        mRegisteredLoggersLock;
    std::list<LockElObject<el::Logger>> mLoggersLocks;

  public:
    explicit LockHelper(std::vector<std::string> const& loggers)
    {
        mRegisteredLoggersLock =
            std::make_unique<LockElObject<el::base::RegisteredLoggers>>(
                el::Helpers::storage()->registeredLoggers());
        for (auto const& logger : loggers)
        {
            // `getLogger` will either return an existing logger, or nullptr
            auto l = el::Loggers::getLogger(logger, false);
            if (l)
            {
                mLoggersLocks.emplace_back(l);
            }
        }
    }
};

void
Logging::setFmt(std::string const& peerID, bool timestamps)
{
    std::string datetime;
    if (timestamps)
    {
        datetime = "%datetime{%Y-%M-%dT%H:%m:%s.%g}";
    }
    const std::string shortFmt =
        datetime + " " + peerID + " [%logger %level] %msg";
    const std::string longFmt = shortFmt + " [%fbase:%line]";

    gDefaultConf.setGlobally(el::ConfigurationType::Format, shortFmt);
    gDefaultConf.set(el::Level::Error, el::ConfigurationType::Format, longFmt);
    gDefaultConf.set(el::Level::Trace, el::ConfigurationType::Format, shortFmt);
    gDefaultConf.set(el::Level::Fatal, el::ConfigurationType::Format, longFmt);
    el::Loggers::reconfigureAllLoggers(gDefaultConf);
}

void
Logging::init()
{
    // el::Loggers::addFlag(el::LoggingFlag::HierarchicalLogging);
    el::Loggers::addFlag(el::LoggingFlag::DisableApplicationAbortOnFatalLog);

    for (auto const& logger : kPartitionNames)
    {
        el::Loggers::getLogger(logger);
    }

    gDefaultConf.setToDefault();
    gDefaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "true");
    gDefaultConf.setGlobally(el::ConfigurationType::ToFile, "false");
    setFmt("<startup>");
}

void
Logging::setLoggingToFile(std::string const& filename)
{
    gDefaultConf.setGlobally(el::ConfigurationType::ToFile, "true");
    gDefaultConf.setGlobally(el::ConfigurationType::Filename, filename);
    el::Loggers::reconfigureAllLoggers(gDefaultConf);
}

void
Logging::setLoggingColor(bool color)
{
}

el::Level
Logging::getLogLevel(std::string const& partition)
{
    el::Logger* logger = el::Loggers::getLogger(partition);
    if (logger->typedConfigurations()->enabled(el::Level::Trace))
        return el::Level::Trace;
    if (logger->typedConfigurations()->enabled(el::Level::Debug))
        return el::Level::Debug;
    if (logger->typedConfigurations()->enabled(el::Level::Info))
        return el::Level::Info;
    if (logger->typedConfigurations()->enabled(el::Level::Warning))
        return el::Level::Warning;
    if (logger->typedConfigurations()->enabled(el::Level::Error))
        return el::Level::Error;
    if (logger->typedConfigurations()->enabled(el::Level::Fatal))
        return el::Level::Fatal;
    return el::Level::Unknown;
}

bool
Logging::logDebug(std::string const& partition)
{
    if (!gAnyDebug)
    {
        // Checking debug in a hot path can get surprisingly hot.
        return false;
    }
    auto lev = Logging::getLogLevel(partition);
    return lev == el::Level::Debug || lev == el::Level::Trace;
}

bool
Logging::logTrace(std::string const& partition)
{
    if (!gAnyTrace)
    {
        // Checking trace in a hot path can get surprisingly hot.
        return false;
    }
    auto lev = Logging::getLogLevel(partition);
    return lev == el::Level::Trace;
}

// Trace < Debug < Info < Warning < Error < Fatal < None
void
Logging::setLogLevel(el::Level level, const char* partition)
{
    el::Configurations config = gDefaultConf;

    if (level == el::Level::Debug)
    {
        gAnyDebug = true;
        config.set(el::Level::Trace, el::ConfigurationType::Enabled, "false");
    }
    else if (level == el::Level::Info)
    {
        config.set(el::Level::Trace, el::ConfigurationType::Enabled, "false");
        config.set(el::Level::Debug, el::ConfigurationType::Enabled, "false");
    }
    else if (level == el::Level::Warning)
    {
        config.set(el::Level::Trace, el::ConfigurationType::Enabled, "false");
        config.set(el::Level::Debug, el::ConfigurationType::Enabled, "false");
        config.set(el::Level::Info, el::ConfigurationType::Enabled, "false");
    }
    else if (level == el::Level::Error)
    {
        config.set(el::Level::Trace, el::ConfigurationType::Enabled, "false");
        config.set(el::Level::Debug, el::ConfigurationType::Enabled, "false");
        config.set(el::Level::Info, el::ConfigurationType::Enabled, "false");
        config.set(el::Level::Warning, el::ConfigurationType::Enabled, "false");
    }
    else if (level == el::Level::Fatal)
    {
        config.set(el::Level::Trace, el::ConfigurationType::Enabled, "false");
        config.set(el::Level::Debug, el::ConfigurationType::Enabled, "false");
        config.set(el::Level::Info, el::ConfigurationType::Enabled, "false");
        config.set(el::Level::Warning, el::ConfigurationType::Enabled, "false");
        config.set(el::Level::Error, el::ConfigurationType::Enabled, "false");
    }
    else if (level == el::Level::Unknown)
    {
        config.set(el::Level::Trace, el::ConfigurationType::Enabled, "false");
        config.set(el::Level::Debug, el::ConfigurationType::Enabled, "false");
        config.set(el::Level::Info, el::ConfigurationType::Enabled, "false");
        config.set(el::Level::Warning, el::ConfigurationType::Enabled, "false");
        config.set(el::Level::Error, el::ConfigurationType::Enabled, "false");
        config.set(el::Level::Fatal, el::ConfigurationType::Enabled, "false");
    }
    else if (level == el::Level::Trace)
    {
        gAnyDebug = true;
        gAnyTrace = true;
    }

    if (partition)
        el::Loggers::reconfigureLogger(partition, config);
    else
        el::Loggers::reconfigureAllLoggers(config);
}

std::string
Logging::getStringFromLL(el::Level level)
{
    switch (level)
    {
    case el::Level::Global:
        return "Global";
    case el::Level::Trace:
        return "Trace";
    case el::Level::Debug:
        return "Debug";
    case el::Level::Fatal:
        return "Fatal";
    case el::Level::Error:
        return "Error";
    case el::Level::Warning:
        return "Warning";
    case el::Level::Verbose:
        return "Verbose";
    case el::Level::Info:
        return "Info";
    case el::Level::Unknown:
        return "Unknown";
    }
    return "????";
}

// default "info" if unrecognized
el::Level
Logging::getLLfromString(std::string const& levelName)
{
    if (iequals(levelName, "trace"))
    {
        return el::Level::Trace;
    }

    if (iequals(levelName, "debug"))
    {
        return el::Level::Debug;
    }

    if (iequals(levelName, "warning"))
    {
        return el::Level::Warning;
    }

    if (iequals(levelName, "fatal"))
    {
        return el::Level::Fatal;
    }

    if (iequals(levelName, "error"))
    {
        return el::Level::Error;
    }

    if (iequals(levelName, "none"))
    {
        return el::Level::Unknown;
    }

    return el::Level::Info;
}

void
Logging::rotate()
{
    std::vector<std::string> loggers(kPartitionNames.begin(),
                                     kPartitionNames.end());
    loggers.insert(loggers.begin(), "default");

    // Lock the loggers while we rotate; this is needed to
    // prevent race with worker threads which are trying to log
    LockHelper lock{loggers};

    for (auto const& logger : loggers)
    {
        auto loggerObj = el::Loggers::getLogger(logger);

        // Grab logger configuration maxLogFileSize; Unfortunately, we cannot
        // lock those, because during re-configuration easylogging assumes no
        // locks are acquired and performs a delete on configs.

        // This implies that worker threads are NOT expected to do anything
        // other than logging. If worker thread does not follow,
        // we may end up in a deadlock (e.g., main thread holds a lock on
        // logger, while worker thread holds a lock on configs)
        auto prevMaxFileSize =
            loggerObj->typedConfigurations()->maxLogFileSize(el::Level::Global);

        // Reconfigure the logger to enforce minimal filesize, to force
        // closing/re-opening the file. Note that easylogging re-locks the
        // logger inside reconfigure, which is okay, since we are using
        // recursive mutex.
        el::Loggers::reconfigureLogger(
            logger, el::ConfigurationType::MaxLogFileSize, "1");

        // Now return to the previous filesize value. It is important that no
        // logging occurs in between (to prevent loss), and is achieved by the
        // lock on logger we acquired in the beginning.
        el::Loggers::reconfigureLogger(logger,
                                       el::ConfigurationType::MaxLogFileSize,
                                       std::to_string(prevMaxFileSize));
    }
}

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
}

#endif // USE_EASYLOGGING
