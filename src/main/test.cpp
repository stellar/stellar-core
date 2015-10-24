// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <cstdlib>
#include "test.h"
#include "StellarCoreVersion.h"
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

static std::vector<std::string> gTestMetrics;
static std::vector<std::unique_ptr<Config>> gTestCfg[Config::TESTDB_MODES];
static std::vector<TmpDir> gTestRoots;

bool force_sqlite = (std::getenv("STELLAR_FORCE_SQLITE") != nullptr);

Config const&
getTestConfig(int instanceNumber, Config::TestDbMode mode)
{
    if (mode == Config::TESTDB_DEFAULT)
    {
        // by default, tests should be run with in memory SQLITE as it's faster
        // you can change this by enabling the appropriate line below
        mode = Config::TESTDB_IN_MEMORY_SQLITE;
        // mode = Config::TESTDB_ON_DISK_SQLITE;
        // mode = Config::TESTDB_UNIX_LOCAL_POSTGRESQL;
        // mode = Config::TESTDB_TCP_LOCALHOST_POSTGRESQL;
    }
    auto& cfgs = gTestCfg[mode];
    if (cfgs.size() <= static_cast<size_t>(instanceNumber))
    {
        cfgs.resize(instanceNumber + 1);
    }

    if (!cfgs[instanceNumber])
    {
        gTestRoots.emplace_back("stellar-core-test");

        std::string rootDir = gTestRoots.back().getName();
        rootDir += "/";

        cfgs[instanceNumber] = stellar::make_unique<Config>();
        Config& thisConfig = *cfgs[instanceNumber];

        std::ostringstream sstream;

        sstream << "stellar" << instanceNumber << ".log";
        thisConfig.LOG_FILE_PATH = sstream.str();
        thisConfig.BUCKET_DIR_PATH = rootDir + "bucket";
        thisConfig.TMP_DIR_PATH = rootDir + "tmp";

        thisConfig.PARANOID_MODE = true;
        thisConfig.ALLOW_LOCALHOST_FOR_TESTING = true;

        // Tests are run in standalone by default, meaning that no external
        // listening interfaces are opened (all sockets must be manually created
        // and connected loopback sockets), no external connections are
        // attempted.
        thisConfig.RUN_STANDALONE = true;
        thisConfig.FORCE_SCP = true;

        thisConfig.PEER_PORT =
            static_cast<unsigned short>(DEFAULT_PEER_PORT + instanceNumber * 2);
        thisConfig.HTTP_PORT = static_cast<unsigned short>(
            DEFAULT_PEER_PORT + instanceNumber * 2 + 1);

        // We set a secret key by default as FORCE_SCP is true by
        // default and we do need a NODE_SEED to start a new network
        thisConfig.NODE_SEED = SecretKey::random();
        thisConfig.NODE_IS_VALIDATOR = true;

        // single node setup
        thisConfig.QUORUM_SET.validators.push_back(
            thisConfig.NODE_SEED.getPublicKey());
        thisConfig.QUORUM_SET.threshold = 1;

        thisConfig.NETWORK_PASSPHRASE = "(V) (;,,;) (V)";

        std::ostringstream dbname;
        switch (mode)
        {
        case Config::TESTDB_IN_MEMORY_SQLITE:
            dbname << "sqlite3://:memory:";
            break;
        case Config::TESTDB_ON_DISK_SQLITE:
            dbname << "sqlite3://" << rootDir << "test" << instanceNumber
                   << ".db";
            break;
#ifdef USE_POSTGRES
        case Config::TESTDB_POSTGRESQL:
            dbname << "postgresql://dbname=test" << instanceNumber;
            break;
#endif
        default:
            abort();
        }
        thisConfig.DATABASE = dbname.str();
        thisConfig.REPORT_METRICS = gTestMetrics;
    }
    return *cfgs[instanceNumber];
}

int
test(int argc, char* const* argv, el::Level ll,
     std::vector<std::string> const& metrics)
{
    gTestMetrics = metrics;
    Config const& cfg = getTestConfig();
    Logging::setFmt("<test>");
    Logging::setLoggingToFile(cfg.LOG_FILE_PATH);
    Logging::setLogLevel(ll, nullptr);
    LOG(INFO) << "Testing stellar-core " << STELLAR_CORE_VERSION;
    LOG(INFO) << "Logging to " << cfg.LOG_FILE_PATH;

    return Catch::Session().run(argc, argv);
}
}
