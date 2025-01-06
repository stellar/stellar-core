// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/ShortHash.h"
#include "util/Decoder.h"
#include "util/GlobalChecks.h"
#include <filesystem>
#include <json/json.h>
#include <sstream>
#define CATCH_CONFIG_RUNNER

#include "util/asio.h"
#include <autocheck/autocheck.hpp>

#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Config.h"
#include "main/StellarCoreVersion.h"
#include "main/dumpxdr.h"
#include "test.h"
#include "test/TestUtils.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/MetaUtils.h"
#include "util/TmpDir.h"
#include "util/XDRCereal.h"

#include <cstdlib>
#include <fmt/format.h>
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

// We use a Catch event-listener to re-seed all the PRNGs we know about on every
// test, to minimize nondeterministic bleed from one test to the next.
struct ReseedPRNGListener : Catch::TestEventListenerBase
{
    using TestEventListenerBase::TestEventListenerBase;
    static unsigned int sCommandLineSeed;
    virtual void
    testCaseStarting(Catch::TestCaseInfo const& testInfo) override
    {
        reinitializeAllGlobalStateWithSeed(sCommandLineSeed);
    }
};

unsigned int ReseedPRNGListener::sCommandLineSeed = 0;

CATCH_REGISTER_LISTENER(ReseedPRNGListener)

// We also use a Catch event-listener to capture a global "current test context"
// string that we can retrieve elsewhere (eg. in tx tests that record and
// compare metadata).

enum class TestTxMetaMode
{
    META_TEST_IGNORE,
    META_TEST_RECORD,
    META_TEST_CHECK
};

static TestTxMetaMode gTestTxMetaMode{TestTxMetaMode::META_TEST_IGNORE};

struct TestContextListener : Catch::TestEventListenerBase
{
    using TestEventListenerBase::TestEventListenerBase;

    static std::optional<Catch::TestCaseInfo> sTestCtx;
    static std::vector<Catch::SectionInfo> sSectCtx;

    void
    testCaseStarting(Catch::TestCaseInfo const& testInfo) override
    {
        if (gTestTxMetaMode != TestTxMetaMode::META_TEST_IGNORE)
        {
            releaseAssert(threadIsMain());
            releaseAssert(!sTestCtx.has_value());
            sTestCtx.emplace(testInfo);
        }
    }
    void
    testCaseEnded(Catch::TestCaseStats const& testCaseStats) override
    {
        if (gTestTxMetaMode != TestTxMetaMode::META_TEST_IGNORE)
        {
            releaseAssert(threadIsMain());
            releaseAssert(sTestCtx.has_value());
            releaseAssert(sSectCtx.empty());
            sTestCtx.reset();
        }
    }
    void
    sectionStarting(Catch::SectionInfo const& sectionInfo) override
    {
        if (gTestTxMetaMode != TestTxMetaMode::META_TEST_IGNORE)
        {
            releaseAssert(threadIsMain());
            sSectCtx.emplace_back(sectionInfo);
        }
    }
    void
    sectionEnded(Catch::SectionStats const& sectionStats) override
    {
        if (gTestTxMetaMode != TestTxMetaMode::META_TEST_IGNORE)
        {
            releaseAssert(threadIsMain());
            sSectCtx.pop_back();
        }
    }
};

CATCH_REGISTER_LISTENER(TestContextListener)

namespace stdfs = std::filesystem;
std::optional<Catch::TestCaseInfo> TestContextListener::sTestCtx;
std::vector<Catch::SectionInfo> TestContextListener::sSectCtx;

static std::map<stdfs::path,
                std::map<std::string, std::pair<bool, std::vector<uint64_t>>>>
    gTestTxMetadata;
static std::optional<std::ofstream> gDebugTestTxMeta;
static std::vector<std::string> gTestMetrics;
static std::vector<std::unique_ptr<Config>> gTestCfg[Config::TESTDB_MODES];
static std::vector<TmpDir> gTestRoots;
static bool gTestAllVersions{false};
static std::vector<uint32> gVersionsToTest;
int gBaseInstance{0};
static bool gMustUseTestVersionsWrapper{false};
static uint32_t gTestingVersion{Config::CURRENT_LEDGER_PROTOCOL_VERSION};

static void
clearConfigs()
{
    for (auto& a : gTestCfg)
    {
        a.clear();
    }
}

void
test_versions_wrapper(std::function<void(void)> f)
{
    gMustUseTestVersionsWrapper = true;
    for (auto v : gVersionsToTest)
    {
        clearConfigs();
        gTestingVersion = v;
        SECTION("protocol version " + std::to_string(v))
        {
            f();
        }
        clearConfigs();
    }
    gMustUseTestVersionsWrapper = false;
    gTestingVersion = Config::CURRENT_LEDGER_PROTOCOL_VERSION;
}

bool force_sqlite = (std::getenv("STELLAR_FORCE_SQLITE") != nullptr);

static void saveTestTxMeta(stdfs::path const& dir);
static void loadTestTxMeta(stdfs::path const& dir);
static void reportTestTxMeta();

// if this method is used outside of the catch test cases, gTestRoots needs to
// be manually cleared using cleanupTmpDirs. If this isn't done, gTestRoots will
// try to use the logger when it is destructed, which is an issue because the
// logger will have been destroyed.
Config const&
getTestConfig(int instanceNumber, Config::TestDbMode mode)
{
    instanceNumber += gBaseInstance;
    if (mode == Config::TESTDB_DEFAULT)
    {
        // by default, tests should be run with volatile BucketList as it's
        // faster. You can change this by enabling the appropriate line below
        // mode = Config::TESTDB_IN_MEMORY;
        // mode = Config::TESTDB_BUCKET_DB_PERSISTENT;
        // mode = Config::TESTDB_POSTGRESQL;
        mode = Config::TESTDB_BUCKET_DB_VOLATILE;
    }
    auto& cfgs = gTestCfg[mode];
    if (cfgs.size() <= static_cast<size_t>(instanceNumber))
    {
        cfgs.resize(instanceNumber + 1);
    }

    if (!cfgs[instanceNumber])
    {
        if (gTestRoots.empty())
        {
            gTestRoots.emplace_back(
                fmt::format("stellar-core-test-{}", gBaseInstance));
        }
        auto const& testBase = gTestRoots[0].getName();
        gTestRoots.emplace_back(
            fmt::format("{}/test-{}", testBase, instanceNumber));

        std::string rootDir = gTestRoots.back().getName();
        rootDir += "/";

        cfgs[instanceNumber] = std::make_unique<Config>();
        Config& thisConfig = *cfgs[instanceNumber];
        thisConfig.USE_CONFIG_FOR_GENESIS = true;
        thisConfig.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = gTestingVersion;
        LOG_INFO(DEFAULT_LOG, "Making config for {}",
                 thisConfig.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION);

        thisConfig.BUCKET_DIR_PATH = rootDir + "bucket";

        thisConfig.INVARIANT_CHECKS = {".*"};

        thisConfig.ALLOW_LOCALHOST_FOR_TESTING = true;

        // this forces to pick up any other potential upgrades
        thisConfig.TESTING_UPGRADE_DATETIME = VirtualClock::from_time_t(1);

        // Tests are run in standalone by default, meaning that no external
        // listening interfaces are opened (all sockets must be manually created
        // and connected loopback sockets), no external connections are
        // attempted.
        thisConfig.RUN_STANDALONE = true;
        thisConfig.FORCE_SCP = true;

        thisConfig.MANUAL_CLOSE = true;

        thisConfig.TEST_CASES_ENABLED = true;

        thisConfig.PEER_PORT =
            static_cast<unsigned short>(DEFAULT_PEER_PORT + instanceNumber * 2);
        thisConfig.HTTP_PORT = static_cast<unsigned short>(
            DEFAULT_PEER_PORT + instanceNumber * 2 + 1);

        // We set a secret key by default as FORCE_SCP is true by
        // default and we do need a NODE_SEED to start a new network.
        //
        // Because test configs are built lazily and cached / persist across
        // tests, we do _not_ use the reset-per-test global PRNG to derive their
        // secret keys (this would produce inter-test coupling, including
        // collisions). Instead we derive each from command-line seed and test
        // config instance number, and try to avoid zero, one, or other default
        // seeds the global PRNG might have been seeded with by default (which
        // could thereby collide).
        thisConfig.NODE_SEED = SecretKey::pseudoRandomForTestingFromSeed(
            0xFFFF0000 + (instanceNumber ^ getLastGlobalStateSeed()));
        thisConfig.NODE_IS_VALIDATOR = true;

        // single node setup
        thisConfig.QUORUM_SET.validators.push_back(
            thisConfig.NODE_SEED.getPublicKey());
        thisConfig.QUORUM_SET.threshold = 1;
        thisConfig.UNSAFE_QUORUM = true;

        // Bucket durability significantly slows down tests and is not necessary
        // in most cases.
        thisConfig.DISABLE_XDR_FSYNC = true;

        thisConfig.NETWORK_PASSPHRASE = "(V) (;,,;) (V)";

        std::ostringstream dbname;
        switch (mode)
        {
        case Config::TESTDB_BUCKET_DB_VOLATILE:
        case Config::TESTDB_IN_MEMORY:
            dbname << "sqlite3://:memory:";
            break;
        case Config::TESTDB_BUCKET_DB_PERSISTENT:
            dbname << "sqlite3://" << rootDir << "test.db";
            thisConfig.DISABLE_XDR_FSYNC = false;
            break;
#ifdef USE_POSTGRES
        case Config::TESTDB_POSTGRESQL:
            dbname << "postgresql://dbname=test" << instanceNumber;
            thisConfig.DISABLE_XDR_FSYNC = false;
            break;
#endif
        default:
            abort();
        }

        if (mode == Config::TESTDB_IN_MEMORY)
        {
            thisConfig.MODE_USES_IN_MEMORY_LEDGER = true;
        }

        thisConfig.DATABASE = SecretValue{dbname.str()};

        thisConfig.REPORT_METRICS = gTestMetrics;
        // disable maintenance
        thisConfig.AUTOMATIC_MAINTENANCE_COUNT = 0;
        // disable self-check
        thisConfig.AUTOMATIC_SELF_CHECK_PERIOD = std::chrono::seconds(0);
        // only spin up a small number of worker threads
        thisConfig.WORKER_THREADS = 3;
        thisConfig.QUORUM_INTERSECTION_CHECKER = false;
        thisConfig.METADATA_DEBUG_LEDGERS = 0;

        thisConfig.PEER_READING_CAPACITY = 20;
        thisConfig.PEER_FLOOD_READING_CAPACITY = 20;
        thisConfig.FLOW_CONTROL_SEND_MORE_BATCH_SIZE = 10;

        // Disable RPC endpoint in tests
        thisConfig.HTTP_QUERY_PORT = 0;
        thisConfig.QUERY_SNAPSHOT_LEDGERS = 0;

#ifdef BEST_OFFER_DEBUGGING
        thisConfig.BEST_OFFER_DEBUGGING_ENABLED = true;
#endif
    }
    return *cfgs[instanceNumber];
}

int
runTest(CommandLineArgs const& args)
{
    LogLevel logLevel{LogLevel::LVL_INFO};

    Catch::Session session{};

    auto& seed = session.configData().rngSeed;

    // rotate the seed every 24 hours
    seed = static_cast<unsigned int>(std::time(nullptr)) / (24 * 3600);

    std::string recordTestTxMeta;
    std::string checkTestTxMeta;
    std::string debugTestTxMeta;

    auto parser = session.cli();
    parser |= Catch::clara::Opt(
        [&](std::string const& arg) {
            logLevel = Logging::getLLfromString(arg);
        },
        "LEVEL")["--ll"]("set the log level");
    parser |= Catch::clara::Opt(gTestMetrics, "METRIC-NAME")["--metric"](
        "report metric METRIC-NAME on exit");
    parser |= Catch::clara::Opt(gTestAllVersions)["--all-versions"](
        "test all versions");
    parser |= Catch::clara::Opt(gVersionsToTest, "version")["--version"](
        "test specific version(s)");
    parser |= Catch::clara::Opt(gBaseInstance, "offset")["--base-instance"](
        "instance number offset so multiple instances of "
        "stellar-core can run tests concurrently");
    parser |=
        Catch::clara::Opt(recordTestTxMeta, "DIRNAME")["--record-test-tx-meta"](
            "record baseline TxMeta from all tests");
    parser |=
        Catch::clara::Opt(checkTestTxMeta, "DIRNAME")["--check-test-tx-meta"](
            "check TxMeta from all tests against recorded baseline");
    parser |=
        Catch::clara::Opt(debugTestTxMeta, "FILENAME")["--debug-test-tx-meta"](
            "dump full TxMeta from all tests to FILENAME");

    session.cli(parser);

    auto result = session.cli().parse(
        args.mCommandName, Catch::clara::detail::TokenStream{
                               std::begin(args.mArgs), std::end(args.mArgs)});
    if (!result)
    {
        writeWithTextFlow(std::cerr, result.errorMessage());
        writeWithTextFlow(std::cerr, args.mCommandDescription);
        session.cli().writeToStream(std::cerr);
        return 1;
    }

    if (session.configData().showHelp)
    {
        writeWithTextFlow(std::cout, args.mCommandDescription);
        session.cli().writeToStream(std::cout);
        return 0;
    }

    if (session.configData().libIdentify)
    {
        session.libIdentify();
        return 0;
    }

    ReseedPRNGListener::sCommandLineSeed = seed;
    reinitializeAllGlobalStateWithSeed(seed);

    if (gTestAllVersions)
    {
        gVersionsToTest.resize(Config::CURRENT_LEDGER_PROTOCOL_VERSION + 1);
        std::iota(std::begin(gVersionsToTest), std::end(gVersionsToTest), 0);
    }
    else if (gVersionsToTest.empty())
    {
        gVersionsToTest.emplace_back(Config::CURRENT_LEDGER_PROTOCOL_VERSION);
    }

    if (!recordTestTxMeta.empty())
    {
        if (!checkTestTxMeta.empty())
        {
            LOG_ERROR(DEFAULT_LOG,
                      "Options --record-test-tx-meta and --check-test-tx-meta "
                      "are mutually exclusive");
            return 1;
        }
        gTestTxMetaMode = TestTxMetaMode::META_TEST_RECORD;
    }
    if (!checkTestTxMeta.empty())
    {
        gTestTxMetaMode = TestTxMetaMode::META_TEST_CHECK;
        loadTestTxMeta(checkTestTxMeta);
    }
    if (!debugTestTxMeta.empty())
    {
        gDebugTestTxMeta.emplace(debugTestTxMeta);
        releaseAssert(gDebugTestTxMeta.value().good());
    }

    // Note: Have to setLogLevel twice here to ensure --list-test-names-only is
    // not mixed with stellar-core logging.
    Logging::setFmt("<test>");
    Logging::setLogLevel(logLevel, nullptr);
    // use base instance for logging as we're guaranteed to not have conflicting
    // instances of stellar-core running at the same time with the same base
    // instance
    auto logFile = fmt::format("stellar{}.log", gBaseInstance);
    Logging::setLoggingToFile(logFile);
    Logging::setLogLevel(logLevel, nullptr);

    LOG_INFO(DEFAULT_LOG, "Testing stellar-core {}", STELLAR_CORE_VERSION);
    LOG_INFO(DEFAULT_LOG, "Logging to {}", logFile);

    auto r = session.run();
    // In the 'list' modes Catch returns the number of tests listed. We don't
    // want to treat this value as and error code.
    if (session.configData().listTests ||
        session.configData().listTestNamesOnly ||
        session.configData().listTags || session.configData().listReporters)
    {
        r = 0;
    }
    gTestRoots.clear();
    clearConfigs();

    if (r != 0)
    {
        LOG_ERROR(DEFAULT_LOG, "Nonzero test result with --rng-seed {}", seed);
    }
    if (gTestTxMetaMode == TestTxMetaMode::META_TEST_RECORD)
    {
        saveTestTxMeta(recordTestTxMeta);
    }
    else if (gTestTxMetaMode == TestTxMetaMode::META_TEST_CHECK)
    {
        reportTestTxMeta();
    }
    return r;
}

void
cleanupTmpDirs()
{
    gTestRoots.clear();
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
for_versions_from(uint32 from, Config const& cfg,
                  std::function<void(Config const&)> const& f)
{
    for_versions(from, Config::CURRENT_LEDGER_PROTOCOL_VERSION, cfg, f);
}

void
for_all_versions(Application& app, std::function<void(void)> const& f)
{
    for_versions(1, Config::CURRENT_LEDGER_PROTOCOL_VERSION, app, f);
}

void
for_all_versions(Config const& cfg, std::function<void(Config const&)> const& f)
{
    for_versions(1, Config::CURRENT_LEDGER_PROTOCOL_VERSION, cfg, f);
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
for_versions(uint32 from, uint32 to, Config const& cfg,
             std::function<void(Config const&)> const& f)
{
    if (from > to)
    {
        return;
    }
    auto versions = std::vector<uint32>{};
    versions.resize(to - from + 1);
    std::iota(std::begin(versions), std::end(versions), from);

    for_versions(versions, cfg, f);
}

void
for_versions(uint32 from, uint32 to, Config const& cfg,
             std::function<void(Config&)> const& f)
{
    if (from > to)
    {
        return;
    }
    auto versions = std::vector<uint32>{};
    versions.resize(to - from + 1);
    std::iota(std::begin(versions), std::end(versions), from);

    for_versions(versions, cfg, f);
}

void
for_versions(std::vector<uint32> const& versions, Application& app,
             std::function<void(void)> const& f)
{
    REQUIRE(gMustUseTestVersionsWrapper);

    if (std::find(versions.begin(), versions.end(), gTestingVersion) !=
        versions.end())
    {
        {
            LedgerTxn ltx(app.getLedgerTxnRoot());
            REQUIRE(ltx.loadHeader().current().ledgerVersion ==
                    gTestingVersion);
        }
        f();
    }
}

void
for_versions(std::vector<uint32> const& versions, Config const& cfg,
             std::function<void(Config const&)> const& f)
{
    REQUIRE(gMustUseTestVersionsWrapper);

    if (std::find(versions.begin(), versions.end(), gTestingVersion) !=
        versions.end())
    {
        REQUIRE(cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION == gTestingVersion);
        Config vcfg = cfg;
        vcfg.LEDGER_PROTOCOL_VERSION = gTestingVersion;
        f(vcfg);
    }
}

void
for_versions(std::vector<uint32> const& versions, Config const& cfg,
             std::function<void(Config&)> const& f)
{
    REQUIRE(gMustUseTestVersionsWrapper);

    if (std::find(versions.begin(), versions.end(), gTestingVersion) !=
        versions.end())
    {
        REQUIRE(cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION == gTestingVersion);
        Config vcfg = cfg;
        vcfg.LEDGER_PROTOCOL_VERSION = gTestingVersion;
        f(vcfg);
    }
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

static void
logFatalAndThrow(std::string const& msg)
{
    LOG_FATAL(DEFAULT_LOG, "{}", msg);
    throw std::runtime_error(msg);
}

static std::pair<stdfs::path, std::string>
getCurrentTestContext()
{
    releaseAssert(threadIsMain());

    releaseAssert(TestContextListener::sTestCtx.has_value());
    auto& tc = TestContextListener::sTestCtx.value();
    stdfs::path file(tc.lineInfo.file);
    file = file.filename().stem();

    std::stringstream oss;
    bool first = true;
    for (auto const& sc : TestContextListener::sSectCtx)
    {
        if (!first)
        {
            oss << '|';
        }
        else
        {
            first = false;
        }
        oss << sc.name;
    }

    return std::make_pair(file, oss.str());
}

void
recordOrCheckGlobalTestTxMetadata(TransactionMeta const& txMetaIn)
{
    if (gTestTxMetaMode == TestTxMetaMode::META_TEST_IGNORE)
    {
        return;
    }
    TransactionMeta txMeta = txMetaIn;
    normalizeMeta(txMeta);
    auto ctx = getCurrentTestContext();
    if (gDebugTestTxMeta.has_value())
    {
        gDebugTestTxMeta.value()
            << "=== " << ctx.first << " : " << ctx.second << " ===" << std::endl
            << xdrToCerealString(txMeta, "TransactionMeta", false) << std::endl;
    }
    uint64_t gotTxMetaHash = shortHash::xdrComputeHash(txMeta);
    if (gTestTxMetaMode == TestTxMetaMode::META_TEST_RECORD)
    {
        auto& pair = gTestTxMetadata[ctx.first][ctx.second];
        auto& testRan = pair.first;
        auto& testHashes = pair.second;
        testRan = false;
        testHashes.emplace_back(gotTxMetaHash);
    }
    else
    {
        releaseAssert(gTestTxMetaMode == TestTxMetaMode::META_TEST_CHECK);
        auto i = gTestTxMetadata.find(ctx.first);
        CHECK(i != gTestTxMetadata.end());
        if (i == gTestTxMetadata.end())
        {
            return;
        }
        auto j = i->second.find(ctx.second);
        CHECK(j != i->second.end());
        if (j == i->second.end())
        {
            return;
        }
        bool& testRan = j->second.first;
        testRan = true;
        std::vector<uint64_t>& vec = j->second.second;
        CHECK(!vec.empty());
        if (vec.empty())
        {
            return;
        }
        uint64_t& expectedTxMetaHash = vec.back();
        CHECK(expectedTxMetaHash == gotTxMetaHash);
        vec.pop_back();
    }
}

static char const* TESTKEY_PROTOCOL_VERSION = "!cfg protocol version";
static char const* TESTKEY_RNG_SEED = "!rng seed";
static char const* TESTKEY_ALL_VERSIONS = "!test all versions";
static char const* TESTKEY_VERSIONS_TO_TEST = "!versions to test";

template <typename T>
void
checkTestKeyVal(std::string const& k, T const& expected, T const& got,
                stdfs::path const& path)
{
    if (expected != got)
    {
        throw std::runtime_error(fmt::format("Expected '{}' = {}, got {} in {}",
                                             k, expected, got, path));
    }
}

template <typename T>
void
checkTestKeyVals(std::string const& k, T const& expected, T const& got,
                 stdfs::path const& path)
{
    if (expected != got)
    {
        throw std::runtime_error(fmt::format("Expected '{}' = {}, got {} in {}",
                                             k, fmt::join(expected, ", "),
                                             fmt::join(got, ", "), path));
    }
}

static void
loadTestTxMeta(stdfs::path const& dir)
{
    if (!stdfs::is_directory(dir))
    {
        logFatalAndThrow(fmt::format("{} is not a directory", dir));
    }
    size_t n = 0;
    for (auto const& dirent : stdfs::directory_iterator{dir})
    {
        auto path = dirent.path();
        if (path.extension() != ".json")
        {
            continue;
        }
        std::ifstream in(path);
        if (!in)
        {
            logFatalAndThrow(fmt::format("Failed to open {}", path));
        }
        in.exceptions(std::ios::failbit | std::ios::badbit);
        Json::Value root;
        in >> root;
        auto basename = path.filename().stem();
        auto& fileTestCases = gTestTxMetadata[basename];
        for (auto entry = root.begin(); entry != root.end(); ++entry)
        {
            std::string name = entry.key().asString();
            if (!name.empty() && name.at(0) == '!')
            {
                if (name == TESTKEY_PROTOCOL_VERSION)
                {
                    checkTestKeyVal(name,
                                    Config::CURRENT_LEDGER_PROTOCOL_VERSION,
                                    entry->asUInt(), path);
                }
                else if (name == TESTKEY_RNG_SEED)
                {
                    checkTestKeyVal(name, ReseedPRNGListener::sCommandLineSeed,
                                    entry->asUInt(), path);
                }
                else if (name == TESTKEY_ALL_VERSIONS)
                {
                    checkTestKeyVal(name, gTestAllVersions, entry->asBool(),
                                    path);
                }
                else if (name == TESTKEY_VERSIONS_TO_TEST)
                {
                    std::vector<uint32> versions;
                    for (auto v : *entry)
                    {
                        versions.emplace_back(v.asUInt());
                    }
                    checkTestKeyVals(name, gVersionsToTest, versions, path);
                }
                continue;
            }
            std::vector<uint64_t> hashes;
            for (auto const& h : *entry)
            {
                ++n;
                // To keep the baseline files small, we store each
                // 64-bit SIPHash as a base64 encoded string.
                std::vector<uint8_t> buf;
                decoder::decode_b64(h.asString(), buf);
                uint64_t tmp = 0;
                for (size_t i = 0; i < sizeof(uint64_t); ++i)
                {
                    tmp <<= 8;
                    tmp |= buf.at(i);
                }
                hashes.emplace_back(tmp);
            }
            std::reverse(hashes.begin(), hashes.end());
            auto pair =
                fileTestCases.emplace(name, std::make_pair(false, hashes));
            if (!pair.second)
            {
                logFatalAndThrow(
                    fmt::format("Duplicate test TxMeta found for key: {}:{}",
                                basename, name));
            }
        }
    }
    LOG_INFO(DEFAULT_LOG, "Loaded {} TxMetas to check during replay", n);
}

static void
saveTestTxMeta(stdfs::path const& dir)
{
    for (auto const& filePair : gTestTxMetadata)
    {
        Json::Value fileRoot;
        fileRoot[TESTKEY_PROTOCOL_VERSION] =
            Config::CURRENT_LEDGER_PROTOCOL_VERSION;
        fileRoot[TESTKEY_RNG_SEED] = ReseedPRNGListener::sCommandLineSeed;
        fileRoot[TESTKEY_ALL_VERSIONS] = gTestAllVersions;
        {
            Json::Value versions;
            for (auto v : gVersionsToTest)
            {
                versions.append(v);
            }
            fileRoot[TESTKEY_VERSIONS_TO_TEST] = versions;
        }
        for (auto const& testCasePair : filePair.second)
        {
            Json::Value& hashes = fileRoot[testCasePair.first];
            for (auto const& h : testCasePair.second.second)
            {
                uint64_t tmp = h;
                std::vector<uint8_t> buf;
                for (size_t i = sizeof(uint64_t); i > 0; --i)
                {
                    buf.emplace_back(uint8_t(0xff & (tmp >> (8 * (i - 1)))));
                }
                hashes.append(decoder::encode_b64(buf));
            }
        }
        stdfs::path path = dir / filePair.first;
        path.replace_extension(".json");
        std::ofstream out(path, std::ios_base::trunc);
        if (!out)
        {
            logFatalAndThrow(fmt::format("Failed to open {}", path));
        }
        out.exceptions(std::ios::failbit | std::ios::badbit);
        out << fileRoot;
    }
}

static void
reportTestTxMeta()
{
    size_t contexts = 0, nonempty = 0, hashes = 0;
    for (auto const& filePair : gTestTxMetadata)
    {
        for (auto const& testCasePair : filePair.second)
        {
            ++contexts;
            auto const& testRan = testCasePair.second.first;
            auto const& testHashes = testCasePair.second.second;
            if (testRan && !testHashes.empty())
            {
                LOG_FATAL(DEFAULT_LOG,
                          "Tests did not check {} TxMeta hashes in test "
                          "context '{}' in file '{}'",
                          testHashes.size(), testCasePair.first,
                          filePair.first);
                ++nonempty;
                hashes += testHashes.size();
            }
        }
    }
    if (nonempty == 0)
    {
        LOG_INFO(DEFAULT_LOG,
                 "Checked all expected TxMeta for {} test contexts.", contexts);
    }
    else
    {
        logFatalAndThrow(fmt::format(
            "Found {} un-checked TxMeta hashes in {} of {} test contexts.",
            hashes, nonempty, contexts));
    }
}
}
