// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/CircularBuffer.h"
#include "util/Logging.h"

#include <memory>
#include <string>

namespace stellar
{

using LogEntry = std::unique_ptr<el::LogMessage>;
using LogBuffer = CircularBuffer<LogEntry>;

// NOTE the only purpose of this class is to make the dispatch method of the
// base class public.
// After some small changes it can be used for mocking in unit tests.
class DispatchCallback : public el::base::DefaultLogDispatchCallback
{
  public:
    void dispatch(const std::string& logLine, const el::LogDispatchData* data);
};

class LogFlushPredicate
{
  public:
    bool compareLessOrEqual(const el::Level pushLevel,
                            const el::Level logLevel) const;
};

class InMemoryLogHandler
{
  public:
    InMemoryLogHandler();

    explicit InMemoryLogHandler(LogBuffer&& buffer, LogFlushPredicate predicate,
                                el::Logger* memoryLogger);

    static InMemoryLogHandler& getInstance();

    void handle(el::LogDispatchData const* handlePtr);

    void setPushLevel(std::string const& pushLevel);

  private:
    bool checkForPush(el::LogDispatchData const& data) const;

    void pushLogs();

    std::string buildLogLine(el::LogDispatchData const& data) const;

    void printToLog(el::LogMessage& logMessage);

    void printToLog(std::string const& message);

    el::Level pushLevel;
    LogBuffer buffer;
    DispatchCallback dispatchCallback;
    el::Logger* logger;
    LogFlushPredicate flushPredicate;
};

class StaticMemoryHandler : public el::LogDispatchCallback
{
  public:
    static void setPushLevel(std::string const& pushLevel);

  protected:
    void handle(el::LogDispatchData const* handlePtr);
};
}
