// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/InMemoryLogHandler.h"
#include "util/make_unique.h"

#include <utility>

namespace stellar
{

InMemoryLogHandler::InMemoryLogHandler(LogBuffer&& logBuffer,
                                       LogFlushPredicate predicate,
                                       el::Logger* memoryLogger)
    : buffer(std::move(logBuffer))
    , logger{memoryLogger}
    , flushPredicate(predicate)
{
}

InMemoryLogHandler::InMemoryLogHandler()
    : InMemoryLogHandler(LogBuffer(100), LogFlushPredicate(),
                         el::Loggers::getLogger(Logging::inMemoryLoggerName))
{
}

void
InMemoryLogHandler::handle(el::LogDispatchData const* handlePtr)
{
    const el::LogMessage* data = handlePtr->logMessage();
    buffer.push(make_unique<el::LogMessage>(
        data->level(), data->file(), data->line(), data->func(),
        data->verboseLevel(), data->logger()));
    if (checkForPush(*handlePtr))
    {
        pushLogs();
    }
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
        std::unique_ptr<el::LogMessage> logMessage = std::move(buffer.pop());
        printToLog(*logMessage);
        logMessage.reset(nullptr);
    }

    if (notEmpty)
    {
        printToLog("</in-memory-logger>\n");
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
InMemoryLogHandler::printToLog(el::LogMessage& logMessage)
{
    auto data =
        el::LogDispatchData(&logMessage, el::base::DispatchAction::NormalLog);
    std::string logLine = buildLogLine(data);
    auto newMessage =
        el::LogMessage(logMessage.level(), logMessage.file(), logMessage.line(),
                       logMessage.func(), logMessage.verboseLevel(), logger);
    auto newData =
        el::LogDispatchData(&newMessage, el::base::DispatchAction::NormalLog);

    dispatchCallback.dispatch(logLine, &newData);
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
