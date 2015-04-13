#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#define _ELPP_THREAD_SAFE
#define _ELPP_DISABLE_DEFAULT_CRASH_HANDLING
#define _ELPP_NO_DEFAULT_LOG_FILE

// NOTE: Nothing else should include easylogging directly
//  include this file instead
#include "lib/util/easylogging++.h"

namespace stellar
{
class Logging
{
    static el::Configurations gDefaultConf;

  public:
    static void init();
    static void setFmt(std::string const& peerID);
    static void setLoggingToFile(std::string const& filename);
    static void setLogLevel(el::Level level, const char* partition);
    static el::Level getLLfromString(std::string const& levelName);
    static el::Level getLogLevel(std::string const& partition);
    static std::string getStringFromLL(el::Level);
};
}
