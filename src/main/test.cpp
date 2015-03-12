// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "test.h"
#include "generated/StellardVersion.h"
#include "main/Config.h"
#include "util/make_unique.h"
#include <time.h>
#include "util/Logging.h"
#include "util/TmpDir.h"

#ifdef _WIN32
#include <process.h>
#define GETPID _getpid
#include <direct.h>
#else
#include <unistd.h>
#define GETPID getpid
#include <sys/stat.h>
#endif

#define CATCH_CONFIG_RUNNER
#include "lib/catch.hpp"

namespace stellar
{

static std::vector<std::unique_ptr<Config>> gTestCfg;
static std::vector<TmpDir> gTestRoots;

Config const& getTestConfig(int instanceNumber)
{
    if (gTestCfg.size() <= instanceNumber)
    {
        gTestCfg.resize(instanceNumber+1);
    }

    if (!gTestCfg[instanceNumber])
    {
        gTestRoots.emplace_back("stellard-test");

        std::string rootDir = gTestRoots.back().getName();
        rootDir += "/";

        gTestCfg[instanceNumber] = stellar::make_unique<Config>();
        Config &thisConfig = *gTestCfg[instanceNumber];

        std::ostringstream sstream;
        
        sstream << "stellar" << instanceNumber << ".log";
        thisConfig.LOG_FILE_PATH = sstream.str();
        thisConfig.BUCKET_DIR_PATH = rootDir + "bucket";
        thisConfig.TMP_DIR_PATH = rootDir + "tmp";

        // Tests are run in standalone by default, meaning that no external
        // listening interfaces are opened (all sockets must be manually created
        // and connected loopback sockets), no external connections are
        // attempted.
        thisConfig.RUN_STANDALONE = true;
        thisConfig.START_NEW_NETWORK = true;
        thisConfig.REBUILD_DB = true;

        thisConfig.PEER_PORT = DEFAULT_PEER_PORT + instanceNumber*2;
        thisConfig.HTTP_PORT = DEFAULT_PEER_PORT + instanceNumber*2-1;

        // We set a secret key by default as START_NEW_NETWORK is true by
        // default and we do need a VALIDATION_KEY to start a new network
        thisConfig.VALIDATION_KEY = SecretKey::random();

        // uncomment this when debugging test cases
        //std::ostringstream dbname;
        //dbname << "sqlite3://test" << instanceNumber << ".db";
        //dbname << "postgresql://host=localhost dbname=test" << instanceNumber << " user=test password=test";
        //thisConfig.DATABASE = dbname.str();
        
        
    }
    return *gTestCfg[instanceNumber];
}

int
test(int argc, char* const* argv, el::Level ll)
{
    Config const& cfg = getTestConfig();
    Logging::setLoggingToFile(cfg.LOG_FILE_PATH);
    Logging::setLogLevel(ll, nullptr);
    LOG(INFO) << "Testing stellard-hayashi " << STELLARD_VERSION;
    LOG(INFO) << "Logging to " << cfg.LOG_FILE_PATH;

    return Catch::Session().run(argc, argv);
}
}
