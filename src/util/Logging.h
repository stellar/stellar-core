#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

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
    static el::Configurations mDefaultConf;

  public:
    static void init();
    static void setLoggingToFile(std::string const& filename);
    static void setLogLevel(el::Level level, const char* partition);
    static el::Level getLLfromString(std::string const& levelName);
    static std::string getStringFromLL(el::Level);
};
}
