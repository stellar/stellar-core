#ifndef __LOGGING__
#define __LOGGING__

#include "lib/util/easylogging++.h"

namespace stellar
{
    class Application;
    typedef std::shared_ptr<Application> ApplicationPtr;

    class Logging
    {
        static el::Configurations mDefaultConf;
    public:
        static void setUpLogging(ApplicationPtr app);
        static void setLogLevel(el::Level level, const char* partition);
    };
}


#endif
