#ifndef __LOGGING__
#define __LOGGING__

#include "lib/util/easylogging++.h"

namespace stellar
{
    class Logging
    {
        static el::Configurations mDefaultConf;
    public:
        static void setUpLogging();
        static void setLogLevel(el::Level level, const char* partition);
    };
}


#endif
