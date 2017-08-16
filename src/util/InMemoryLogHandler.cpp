// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/InMemoryLogHandler.h"

namespace stellar
{

InMemoryLogHandler::InMemoryLogHandler(LogBuffer logBuffer,
                                       LogFlushPredicate predicate)
    : buffer{logBuffer}, flushPredicate(predicate)
{
    logger = el::Loggers::getLogger(Logging::inMemoryLoggerName);
    el::Configurations* config = logger->configurations();
    config->set(el::Level::Global, el::ConfigurationType::Enabled, "true");
    config->setGlobally(el::ConfigurationType::ToStandardOutput, "false");
    config->setGlobally(el::ConfigurationType::ToFile, "false");
    logger->configure(*config);
}

InMemoryLogHandler::InMemoryLogHandler()
    : InMemoryLogHandler(LogBuffer(100), LogFlushPredicate())
{
}

void
InMemoryLogHandler::handle(el::LogDispatchData const* handlePtr)
{
    std::string logLine = buildLogLine(*handlePtr);
    buffer.push(logLine);
    if (checkForPush(*handlePtr))
    {
        pushLogs();
    }
}

std::string
InMemoryLogHandler::buildLogLine(el::LogDispatchData const& data) const
{
    return data.logMessage()->logger()->logBuilder()->build(
        data.logMessage(),
        data.dispatchAction() == el::base::DispatchAction::NormalLog);
}

void
InMemoryLogHandler::pushLogs()
{
    bool notEmpty = !buffer.empty();
    if (notEmpty)
    {
        printToLog("<in-memory-logger>\n");
    }

    while (!buffer.empty())
    {
        auto logLine = buffer.pop();
        printToLog(logLine);
        logLine.clear();
    }

    if (notEmpty)
    {
        printToLog("</in-memory-logger>\n");
    }
}

void
InMemoryLogHandler::printToLog(std::string const& message)
{
    auto newMsg = el::LogMessage(el::Level::Trace, "", 0, "", 1, logger);
    auto data =
        el::LogDispatchData(&newMsg, el::base::DispatchAction::NormalLog);
    dispatchCallback.dispatch(message, &data);
}

bool
InMemoryLogHandler::checkForPush(el::LogDispatchData const& data) const
{
    auto level = data.logMessage()->level();
    return flushPredicate.compareLessOrEqual(pushLevel, level);
}

void
InMemoryLogHandler::setLogFilename(std::string const& logFilename)
{
    el::Configurations* config = logger->configurations();
    config->setGlobally(el::ConfigurationType::ToFile, "true");
    config->setGlobally(el::ConfigurationType::Filename, logFilename);
    logger->configure(*config);
}

void
InMemoryLogHandler::setPushLevel(std::string const& pushLevel)
{
    this->pushLevel = Logging::getLLfromString(pushLevel);
}

InMemoryLogHandler&
InMemoryLogHandler::getInstance()
{
    static InMemoryLogHandler instance;
    return instance;
}

void
DispatchCallback::dispatch(std::string const& logLine,
                           el::LogDispatchData const* data)
{
    el::base::DefaultLogDispatchCallback::dispatch(std::string(logLine), data);
}

void
StaticMemoryHandler::handle(el::LogDispatchData const* handlePtr)
{
    InMemoryLogHandler::getInstance().handle(handlePtr);
}

void
StaticMemoryHandler::setLogFilename(std::string const& logFilename)
{
    InMemoryLogHandler::getInstance().setLogFilename(logFilename);
}

void
StaticMemoryHandler::setPushLevel(std::string const& pushLevel)
{
    InMemoryLogHandler::getInstance().setPushLevel(pushLevel);
}

bool
LogFlushPredicate::compareLessOrEqual(const el::Level pushLevel,
                                      const el::Level logLevel) const
{
    using el::Level;
    switch (pushLevel)
    {
    case Level::Global:
    case Level::Verbose:
    case Level::Unknown:
    {
        return false;
    }
    case Level::Trace:
    case Level::Debug:
    {
        return logLevel >= pushLevel;
    }
    case Level::Info:
    case Level::Warning:
    case Level::Error:
    case Level::Fatal:
    {
        return logLevel >= Level::Fatal && logLevel <= pushLevel;
    }
    }
    return false;
}
}
