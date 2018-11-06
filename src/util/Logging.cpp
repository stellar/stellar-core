// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Logging.h"
#include "main/Application.h"
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

namespace
{

static const std::vector<std::string> kLoggers = {
    "Fs",      "SCP",    "Bucket", "Database", "History", "Process",  "Ledger",
    "Overlay", "Herder", "Tx",     "LoadGen",  "Work",    "Invariant"};
}

el::Configurations Logging::gDefaultConf;

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
    gDefaultConf.set(el::Level::Trace, el::ConfigurationType::Format, longFmt);
    gDefaultConf.set(el::Level::Fatal, el::ConfigurationType::Format, longFmt);
    el::Loggers::reconfigureAllLoggers(gDefaultConf);
}

void
Logging::init()
{
    // el::Loggers::addFlag(el::LoggingFlag::HierarchicalLogging);
    el::Loggers::addFlag(el::LoggingFlag::DisableApplicationAbortOnFatalLog);

    for (auto const& logger : kLoggers)
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
    auto lev = Logging::getLogLevel(partition);
    return lev == el::Level::Debug || lev == el::Level::Trace;
}

bool
Logging::logTrace(std::string const& partition)
{
    auto lev = Logging::getLogLevel(partition);
    return lev == el::Level::Trace;
}

// Trace < Debug < Info < Warning < Error < Fatal < None
void
Logging::setLogLevel(el::Level level, const char* partition)
{
    el::Configurations config = gDefaultConf;

    if (level == el::Level::Debug)
        config.set(el::Level::Trace, el::ConfigurationType::Enabled, "false");
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
    auto loggers = kLoggers;
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
}
