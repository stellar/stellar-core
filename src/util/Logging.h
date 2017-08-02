#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#define ELPP_THREAD_SAFE
#define ELPP_DISABLE_DEFAULT_CRASH_HANDLING
#define ELPP_NO_DEFAULT_LOG_FILE
#define ELPP_FEATURE_PERFORMANCE_TRACKING

// NOTE: Nothing else should include easylogging directly
//  include this file instead
#include "lib/util/easylogging++.h"
#include <memory>
#include <utility>

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
    static void enableInMemoryLogging();
};

class DispatchCallback : public el::base::DefaultLogDispatchCallback
{
public:
    void handle(const el::LogDispatchData* handlePtr);
};

class MemoryHandler : public el::LogDispatchCallback
{
public:
    MemoryHandler();

    static MemoryHandler& getInstance();

    void handle(const el::LogDispatchData* handlePtr);

private:
    size_t postIncrementIndex();

    bool checkForPush(el::LogMessage& message);

    void push();

    void clear();

    std::unique_ptr< std::pair< el::base::DispatchAction, std::unique_ptr<el::LogMessage> >[] > buffer;
    size_t start;
    size_t size;
    size_t count;
    el::Level pushLevel;
    DispatchCallback dispatchCallback;
};

class StaticMemoryHandler : public el::LogDispatchCallback
{
protected:
    void handle(const el::LogDispatchData* handlePtr);
};

}
