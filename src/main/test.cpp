#include "main/Application.h"
#include "util/StellardVersion.h"
#include "lib/util/Logging.h"
#include "util/make_unique.h"
#include <time.h>
#include "overlay/LoopbackPeer.h"

namespace stellar
{

typedef std::unique_ptr<Application> appPtr;

bool
allStopped(std::vector<appPtr> &apps)
{
    for (appPtr &app : apps)
    {
        if (!app->getMainIOService().stopped())
        {
            return false;
        }
    }
    return true;
}

void
testHelloGoodbye(Config const &cfg)
{

    std::vector<appPtr> apps;
    apps.emplace_back(make_unique<Application>(cfg));
    apps.emplace_back(make_unique<Application>(cfg));

    LoopbackPeerConnection conn(*apps[0], *apps[1]);

    while (!allStopped(apps))
    {
        for (appPtr &app : apps)
        {
            app->getMainIOService().poll_one();
            app->getWorkerIOService().poll_one();
        }
    }
}

int
test()
{
#ifdef _MSC_VER
#define GETPID _getpid
#else
#define GETPID getpid
#endif
    std::ostringstream oss;
    oss << "stellard-test-" << time(nullptr) << "-" << GETPID() << ".log";
    Config cfg;
    cfg.LOG_FILE_PATH = oss.str();

    // Tests are run in standalone and single-step mode by default, meaning that
    // no external listening interfaces are opened (all sockets must be manually
    // created and connected loopback sockets), no external connections are
    // attempted, and the event loops must be manually cranked.
    cfg.RUN_STANDALONE = true;
    cfg.SINGLE_STEP_MODE = true;

    Logging::setUpLogging(cfg.LOG_FILE_PATH);
    LOG(INFO) << "Testing stellard-hayashi " << STELLARD_VERSION;
    LOG(INFO) << "Logging to " << cfg.LOG_FILE_PATH;

    testHelloGoodbye(cfg);

    return 0;
}
}
