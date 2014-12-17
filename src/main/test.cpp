// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellardVersion.h"
#include "main/Config.h"
#include "util/make_unique.h"
#include "util/Logging.h"
#include <time.h>

#ifdef _MSC_VER
#include <process.h>
#define GETPID _getpid
#else
#include <unistd.h>
#define GETPID getpid
#endif

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
        std::ostringstream oss;
        oss << "stellard-test-" << time(nullptr) << "-" << GETPID() << ".log";
        gTestCfg = stellar::make_unique<Config>();
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
