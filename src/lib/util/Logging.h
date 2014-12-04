#ifndef __LOGGING__
#define __LOGGING__

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
