#include "main/Application.h"
#include "clf/BucketList.h"
#include "util/StellardVersion.h"
#include "lib/util/Logging.h"
#include "util/make_unique.h"
#include "xdrpp/autocheck.h"
#include <time.h>
#include <future>
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
        }
    }
}

void
testBucketList(Config const &cfg)
{
    try
    {
        Application app(cfg);
        BucketList bl;
        autocheck::generator<std::vector<Bucket::KVPair>> gen;
        for (uint64_t i = 1; !app.getMainIOService().stopped() && i < 300; ++i)
        {
            app.getMainIOService().poll_one();
            bl.addBatch(app, i, gen(100));
        }
    }
    catch (std::future_error &e)
    {
        LOG(DEBUG) << "Test caught std::future_error "
                   << e.code() << ": " << e.what();
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

    // Tests are run in standalone by default, meaning that no external
    // listening interfaces are opened (all sockets must be manually created and
    // connected loopback sockets), no external connections are attempted.
    cfg.RUN_STANDALONE = true;

    Logging::setUpLogging(cfg.LOG_FILE_PATH);
    LOG(INFO) << "Testing stellard-hayashi " << STELLARD_VERSION;
    LOG(INFO) << "Logging to " << cfg.LOG_FILE_PATH;

    testHelloGoodbye(cfg);
    testBucketList(cfg);

    return 0;
}
}
