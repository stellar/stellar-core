// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Logging.h"
#include "main/Application.h"
#include "util/types.h"
#include "util/Fs.h"

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

static const std::vector<std::string> loggers = {
    "Fs",      "SCP",    "Bucket", "Database", "History", "Process",  "Ledger",
    "Overlay", "Herder", "Tx",     "LoadGen",  "Work",    "Invariant"};
}

el::Configurations Logging::gDefaultConf;

void
Logging::setFmt(std::string const& peerID, bool timestamps)
{
    std::string datetime;
    if (timestamps)
    {
        datetime = "%datetime{%Y-%M-%dT%H:%m:%s.%g}";
    }
    const std::string shortFmt = datetime + " " + peerID + " [%logger %level] %msg";
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

    for (auto const& logger : loggers)
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
    el::Loggers::getLogger("default")->reconfigure();
    for (auto const& logger : loggers)
    {
        el::Loggers::getLogger(logger)->reconfigure();
    }
}

void
Logging::enableInMemoryLogging()
{
    const std::string defaultMagicLogDispatchCallback = "DefaultLogDispatchCallback";
    const std::string inMemoryHandlerName = "InMemoryHandler";
    el::Helpers::uninstallLogDispatchCallback<el::base::DefaultLogDispatchCallback>(defaultMagicLogDispatchCallback);
    el::Helpers::installLogDispatchCallback<StaticMemoryHandler>(inMemoryHandlerName);
}

MemoryHandler::MemoryHandler() : start(0), size(10), count(0), pushLevel(el::Level::Fatal)
{
    using namespace std;
    buffer = unique_ptr<pair<el::base::DispatchAction, unique_ptr<el::LogMessage>>[]>(new pair<el::base::DispatchAction, unique_ptr<el::LogMessage> >[size]);
}

void
MemoryHandler::handle(const el::LogDispatchData* handlePtr)
{
    using namespace std;
    el::base::DispatchAction dispatchAction = handlePtr->dispatchAction();
    el::LogMessage* tmpMsg = const_cast<el::LogMessage*>(handlePtr->logMessage());
    auto messageData = new el::LogMessage(tmpMsg->level(),
                                          tmpMsg->file(),
                                          tmpMsg->line(),
                                          tmpMsg->func(),
                                          tmpMsg->verboseLevel(),
                                          tmpMsg->logger());
    size_t ix = postIncrementIndex();
    buffer[ix] = std::make_pair(dispatchAction, unique_ptr<el::LogMessage>(messageData));
    if (checkForPush(*messageData))
    {
        push();
        clear();
    }
}

void
MemoryHandler::push()
{
    using namespace std;
    for (size_t i = 0; i < count; ++i) {
        size_t ix = (start+i) % size;
        pair< el::base::DispatchAction, unique_ptr<el::LogMessage> >& recordData = buffer[ix];
        el::base::DispatchAction dispatchAction = recordData.first;
        unique_ptr<el::LogMessage>& message = recordData.second;
        el::LogDispatchData data(message.get(), dispatchAction);
        dispatchCallback.handle(&data);
        message.reset(nullptr);
    }
}

void
MemoryHandler::clear()
{
    start = 0;
    count = 0;
}


bool
MemoryHandler::checkForPush(el::LogMessage& message)
{
    if (message.level() == pushLevel)
    {
        return true;
    }
    else
    {
        return false;
    }
}

size_t
MemoryHandler::postIncrementIndex()
{
    size_t ix = (start + count) % size;
    if (count < size)
    {
        count += 1;
    }
    else
    {
        start = start+1 % size;
    }
    return ix;
}

MemoryHandler&
MemoryHandler::getInstance()
{
    static MemoryHandler instance;
    return instance;
}

void
StaticMemoryHandler::handle(const el::LogDispatchData* handlePtr)
{
    MemoryHandler::getInstance().handle(handlePtr);
}

void DispatchCallback::handle(const el::LogDispatchData* handlePtr)
{
    el::base::DefaultLogDispatchCallback::handle(handlePtr);
}

}
