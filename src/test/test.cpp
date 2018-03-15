// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#define CATCH_CONFIG_RUNNER

#include "util/asio.h"

#include "main/Config.h"
#include "main/StellarCoreVersion.h"
#include "test.h"
#include "test/TestUtils.h"
#include "util/Logging.h"
#include "util/TmpDir.h"
#include "util/make_unique.h"
#include <cstdlib>
#include <numeric>
#include <time.h>

#ifdef _WIN32
#include <process.h>
#define GETPID _getpid
#include <direct.h>
#else
#include <unistd.h>
#define GETPID getpid
#include <sys/stat.h>
#endif

#include "test/SimpleTestReporter.h"

namespace Catch
{

SimpleTestReporter::~SimpleTestReporter()
{
}
}

namespace stellar
{

static std::vector<std::string> gTestMetrics;
static std::vector<std::unique_ptr<Config>> gTestCfg[Config::TESTDB_MODES];
static std::vector<TmpDir> gTestRoots;
static bool gTestAllVersions{false};

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
        // mode = Config::TESTDB_POSTGRESQL;
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

        thisConfig.INVARIANT_CHECKS = {"AccountSubEntriesCountIsValid",
                                       "BucketListIsConsistentWithDatabase",
                                       "CacheIsConsistentWithDatabase",
                                       "ConservationOfLumens",
                                       "LedgerEntryIsValid",
                                       "MinimumAccountBalance"};

        thisConfig.ALLOW_LOCALHOST_FOR_TESTING = true;

        // this forces to pick up any other potential upgrades
        thisConfig.TESTING_UPGRADE_DATETIME = VirtualClock::from_time_t(1);

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
        thisConfig.UNSAFE_QUORUM = true;

        thisConfig.NETWORK_PASSPHRASE = "(V) (;,,;) (V)";

        // disable NTP - travis-ci does not allow network access:
        // The container-based, OSX, and GCE (both Precise and Trusty) builds do
        // not currently have IPv6 connectivity.
        thisConfig.NTP_SERVER.clear();

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
        thisConfig.DATABASE = SecretValue{dbname.str()};
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

    using namespace Catch;
    Session session{};
    auto r =
        session.applyCommandLine(argc, argv, Session::OnUnusedOptions::Ignore);
    if (r != 0)
        return r;

    auto unusedTokens = session.unusedTokens();
    using namespace Clara;

    for (auto const& token : unusedTokens)
    {
        if (token.type == Parser::Token::LongOpt &&
            token.data == "all-versions")
        {
            gTestAllVersions = true;
            continue;
        }

        session.showHelp(session.configData().processName);
        return -1;
    }

    r = session.run();
    gTestRoots.clear();
    gTestCfg->clear();
    return r;
}

void
for_versions_to(uint32 to, Application& app, std::function<void(void)> const& f)
{
    for_versions(1, to, app, f);
}

void
for_versions_from(uint32 from, Application& app,
                  std::function<void(void)> const& f)
{
    for_versions(from, Config::CURRENT_LEDGER_PROTOCOL_VERSION, app, f);
}

void
for_versions_from(std::vector<uint32> const& versions, Application& app,
                  std::function<void(void)> const& f)
{
    for_versions(versions, app, f);
    for_versions_from(versions.back() + 1, app, f);
}

void
for_all_versions(Application& app, std::function<void(void)> const& f)
{
    for_versions(1, Config::CURRENT_LEDGER_PROTOCOL_VERSION, app, f);
}

void
for_versions(uint32 from, uint32 to, Application& app,
             std::function<void(void)> const& f)
{
    if (from > to)
    {
        return;
    }
    auto versions = std::vector<uint32>{};
    versions.resize(to - from + 1);
    std::iota(std::begin(versions), std::end(versions), from);

    for_versions(versions, app, f);
}

void
for_versions(std::vector<uint32> const& versions, Application& app,
             std::function<void(void)> const& f)
{
    auto previousVersion = app.getLedgerManager().getCurrentLedgerVersion();
    for (auto v : versions)
    {
        if (!gTestAllVersions && v != Config::CURRENT_LEDGER_PROTOCOL_VERSION)
        {
            continue;
        }
        SECTION("protocol version " + std::to_string(v))
        {
            testutil::setCurrentLedgerVersion(app.getLedgerManager(), v);
            f();
        }
    }
    testutil::setCurrentLedgerVersion(app.getLedgerManager(), previousVersion);
}

void
for_all_versions_except(std::vector<uint32> const& versions, Application& app,
                        std::function<void(void)> const& f)
{
    uint32 lastExcept = 0;
    for (uint32 except : versions)
    {
        for_versions(lastExcept + 1, except - 1, app, f);
        lastExcept = except;
    }
    for_versions_from(lastExcept + 1, app, f);
}
}
