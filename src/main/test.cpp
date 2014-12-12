#include "util/StellardVersion.h"
#include "main/Config.h"
#include "util/make_unique.h"
#include "lib/util/Logging.h"
#include <time.h>

#define CATCH_CONFIG_RUNNER
#include "lib/catch.hpp"

namespace stellar
{

static std::unique_ptr<Config> gTestCfg;

Config const&
getTestConfig()
{
    if (!gTestCfg)
    {
#ifdef _MSC_VER
#define GETPID _getpid
#else
#define GETPID getpid
#endif
        std::ostringstream oss;
        oss << "stellard-test-" << time(nullptr) << "-" << GETPID() << ".log";
        gTestCfg = make_unique<Config>();
        gTestCfg->LOG_FILE_PATH = oss.str();

        // Tests are run in standalone by default, meaning that no external
        // listening interfaces are opened (all sockets must be manually created
        // and connected loopback sockets), no external connections are
        // attempted.
        gTestCfg->RUN_STANDALONE = true;
    }
    return *gTestCfg;
}

int
test(int argc, char* const* argv)
{
    Config const& cfg = getTestConfig();
    Logging::setUpLogging(cfg.LOG_FILE_PATH);
    LOG(INFO) << "Testing stellard-hayashi " << STELLARD_VERSION;
    LOG(INFO) << "Logging to " << cfg.LOG_FILE_PATH;

    return Catch::Session().run(argc, argv);
}
}
