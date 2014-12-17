#ifndef __LOGGING__
#define __LOGGING__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#define _ELPP_THREAD_SAFE
#define _ELPP_DISABLE_DEFAULT_CRASH_HANDLING

#include "lib/util/easylogging++.h"

namespace stellar
{
    class Logging
    {
        static el::Configurations mDefaultConf;
    public:
        static void setUpLogging(std::string const& filename);
        static void setLogLevel(el::Level level, const char* partition);
    };
}


#endif
