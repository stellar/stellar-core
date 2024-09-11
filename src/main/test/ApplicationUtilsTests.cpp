// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Random.h"
#include "history/HistoryArchiveManager.h"
#include "history/HistoryManagerImpl.h"
#include "history/test/HistoryTestsUtils.h"
#include "invariant/BucketListIsConsistentWithDatabase.h"
#include "ledger/LedgerTxn.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/ApplicationUtils.h"
#include "main/CommandHandler.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "simulation/Simulation.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include <filesystem>
#include <fstream>

using namespace stellar;
using namespace stellar::historytestutils;

class TemporaryFileDamager
{
  protected:
    std::filesystem::path mVictim;
    std::filesystem::path mVictimSaved;

  public:
    TemporaryFileDamager(std::filesystem::path victim) : mVictim(victim)
    {
        mVictimSaved = mVictim;
        mVictimSaved.replace_filename(mVictim.filename().string() + "-saved");
        std::filesystem::copy_file(mVictim, mVictimSaved);
    }
    virtual void
    damageVictim()
    {
        // Default damage just truncates the file.
        std::ofstream out(mVictim);
    }
    ~TemporaryFileDamager()
    {
        std::filesystem::remove(mVictim);
        std::filesystem::rename(mVictimSaved, mVictim);
    }
};

class TemporarySQLiteDBDamager : public TemporaryFileDamager
{
    Config mConfig;
    static std::filesystem::path
    getSQLiteDBPath(Config const& cfg)
    {
        auto str = cfg.DATABASE.value;
        std::string prefix = "sqlite3://";
        REQUIRE(str.find(prefix) == 0);
        str = str.substr(prefix.size());
        REQUIRE(!str.empty());
        std::filesystem::path path(str);
        REQUIRE(std::filesystem::exists(path));
        return path;
    }

  public:
    TemporarySQLiteDBDamager(Config const& cfg)
        : TemporaryFileDamager(getSQLiteDBPath(cfg)), mConfig(cfg)
    {
    }
    void
    damageVictim() override
    {
        // Damage a database by bumping the root account's last-modified.
        VirtualClock clock;
        auto app = createTestApplication(clock, mConfig, /*newDB=*/false);
        LedgerTxn ltx(app->getLedgerTxnRoot(),
                      /*shouldUpdateLastModified=*/false);
        {
            auto rootKey = accountKey(
                stellar::txtest::getRoot(app->getNetworkID()).getPublicKey());
            auto rootLe = ltx.load(rootKey);
            rootLe.current().lastModifiedLedgerSeq += 1;
        }
        ltx.commit();
    }
};

// Logic to check the state of the bucket list with the state of the DB
static bool
checkState(Application& app)
{
    BucketListIsConsistentWithDatabase blc(app);
    bool blcOk = true;
    try
    {
        blc.checkEntireBucketlist();
    }
    catch (std::runtime_error& e)
    {
        LOG_ERROR(DEFAULT_LOG, "Error during bucket-list consistency check: {}",
                  e.what());
        blcOk = false;
    }

    if (app.getConfig().isUsingBucketListDB())
    {
        auto checkBucket = [&blcOk](auto b) {
            if (!b->isEmpty() && !b->isIndexed())
            {
                LOG_ERROR(DEFAULT_LOG,
                          "Error during bucket-list consistency check: "
                          "unindexed bucket in BucketList");
                blcOk = false;
            }
        };

        auto& bm = app.getBucketManager();
        for (uint32_t i = 0; i < bm.getBucketList().kNumLevels && blcOk; ++i)
        {
            auto& level = bm.getBucketList().getLevel(i);
            checkBucket(level.getCurr());
            checkBucket(level.getSnap());
            auto& nextFuture = level.getNext();
            if (nextFuture.hasOutputHash())
            {
                auto hash = hexToBin256(nextFuture.getOutputHash());
                checkBucket(bm.getBucketByHash(hash));
            }
        }
    }

    return blcOk;
}

// Sets up a network with a main validator node that publishes checkpoints to
// a test node. Tests startup behavior of the test node when up to date with
// validator and out of sync.
class SimulationHelper
{
    Simulation::pointer mSimulation;
    Application::pointer mMainNode;
    Config& mMainCfg;
    Config& mTestCfg;
    PublicKey mMainNodeID;
    PublicKey mTestNodeID;
    SCPQuorumSet mQuorum;
    TmpDirHistoryConfigurator mHistCfg;

  public:
    SimulationHelper(Config& mainCfg, Config& testCfg)
        : mMainCfg(mainCfg), mTestCfg(testCfg)
    {
        auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
        mSimulation =
            std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);

        // Main node never shuts down, publishes checkpoints for test node
        mMainNodeID = mMainCfg.NODE_SEED.getPublicKey();

        // Test node may shutdown, lose sync, etc.
        mTestNodeID = testCfg.NODE_SEED.getPublicKey();

        mQuorum.threshold = 1;
        mQuorum.validators.push_back(mMainNodeID);

        // Setup validator to publish
        mMainCfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
        mMainCfg = mHistCfg.configure(mMainCfg, /* writable */ true);
        mMainCfg.MAX_SLOTS_TO_REMEMBER = 50;
        mMainCfg.USE_CONFIG_FOR_GENESIS = false;
        mMainCfg.TESTING_UPGRADE_DATETIME = VirtualClock::from_time_t(0);

        mTestCfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
        mTestCfg.NODE_IS_VALIDATOR = false;
        mTestCfg.FORCE_SCP = false;
        mTestCfg.INVARIANT_CHECKS = {};
        mTestCfg.MODE_AUTO_STARTS_OVERLAY = false;
        mTestCfg.MODE_STORES_HISTORY_LEDGERHEADERS = true;
        mTestCfg.USE_CONFIG_FOR_GENESIS = false;
        mTestCfg.TESTING_UPGRADE_DATETIME = VirtualClock::from_time_t(0);

        // Test node points to a read-only archive maintained by the
        // main validator
        mTestCfg = mHistCfg.configure(mTestCfg, /* writable */ false);

        mMainNode =
            mSimulation->addNode(mMainCfg.NODE_SEED, mQuorum, &mMainCfg);
        mMainNode->getHistoryArchiveManager().initializeHistoryArchive(
            mHistCfg.getArchiveDirName());

        mSimulation->addNode(testCfg.NODE_SEED, mQuorum, &mTestCfg);
        mSimulation->addPendingConnection(mMainNodeID, mTestNodeID);
        mSimulation->startAllNodes();

        mSimulation->crankUntil(
            [&simulation = mSimulation]() {
                return simulation->haveAllExternalized(3, 5);
            },
            10 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
    }

    void
    generateLoad(bool soroban)
    {
        auto& loadGen = mMainNode->getLoadGenerator();
        auto& loadGenDone = mMainNode->getMetrics().NewMeter(
            {"loadgen", "run", "complete"}, "run");

        // Generate a bit of load, and crank for some time
        if (soroban)
        {
            loadGen.generateLoad(GeneratedLoadConfig::txLoad(
                LoadGenMode::SOROBAN_UPLOAD, /* nAccounts */ 50, /* nTxs */ 10,
                /*txRate*/ 1));
        }
        else
        {
            loadGen.generateLoad(GeneratedLoadConfig::txLoad(
                LoadGenMode::PAY, /* nAccounts */ 50, /* nTxs */ 10,
                /*txRate*/ 1));
        }

        auto currLoadGenCount = loadGenDone.count();

        mSimulation->crankUntil(
            [&]() {
                bool loadDone = loadGenDone.count() > currLoadGenCount;
                return loadDone && mSimulation->getNode(mTestNodeID)
                                       ->getLedgerManager()
                                       .isSynced();
            },
            10 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
    }

    // Publish checkpoints until selected checkpoint reached. Returns seqno and
    // hash for a ledger within selected checkpoint
    std::pair<uint32_t, std::string>
    publishCheckpoints(uint32_t selectedCheckpoint)
    {
        uint32_t selectedLedger = 0;
        std::string selectedHash;

        auto& loadGen = mMainNode->getLoadGenerator();
        auto& loadGenDone = mMainNode->getMetrics().NewMeter(
            {"loadgen", "run", "complete"}, "run");

        loadGen.generateLoad(
            GeneratedLoadConfig::createAccountsLoad(/* nAccounts */ 50,
                                                    /* txRate */ 1));
        auto currLoadGenCount = loadGenDone.count();

        auto checkpoint =
            mMainNode->getHistoryManager().getCheckpointFrequency();

        // Make sure validator publishes something
        mSimulation->crankUntil(
            [&]() {
                bool loadDone = loadGenDone.count() > currLoadGenCount;
                auto lcl =
                    mMainNode->getLedgerManager().getLastClosedLedgerHeader();
                // Pick some ledger in the selected checkpoint to run
                // catchup against later
                if (lcl.header.ledgerSeq < selectedCheckpoint * checkpoint)
                {
                    selectedLedger = lcl.header.ledgerSeq;
                    selectedHash = binToHex(lcl.hash);
                }

                // Validator should publish up to and including the selected
                // checkpoint
                return loadDone &&
                       mSimulation->haveAllExternalized(
                           (selectedCheckpoint + 1) * checkpoint, 5);
            },
            50 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

        REQUIRE(selectedLedger != 0);
        REQUIRE(!selectedHash.empty());
        return std::make_pair(selectedLedger, selectedHash);
    }

    LedgerHeaderHistoryEntry const&
    getMainNodeLCL()
    {
        return mSimulation->getNode(mMainNodeID)
            ->getLedgerManager()
            .getLastClosedLedgerHeader();
    }

    LedgerHeaderHistoryEntry const&
    getTestNodeLCL()
    {
        return mSimulation->getNode(mTestNodeID)
            ->getLedgerManager()
            .getLastClosedLedgerHeader();
    }

    void
    shutdownTestNode()
    {
        mSimulation->removeNode(mTestNodeID);
    }

    void
    runStartupTest(bool triggerCatchup, uint32_t startFromLedger,
                   std::string startFromHash, uint32_t lclLedgerSeq)
    {
        bool isInMemoryMode = startFromLedger != 0 && !startFromHash.empty();
        if (isInMemoryMode)
        {
            REQUIRE(canRebuildInMemoryLedgerFromBuckets(startFromLedger,
                                                        lclLedgerSeq));
        }

        uint32_t checkpointFrequency = 8;

        // Depending on how many ledgers we buffer during bucket
        // apply, core might trim some and only keep checkpoint
        // ledgers. In this case, after bucket application, normal
        // catchup will be triggered.
        uint32_t delayBuckets = triggerCatchup ? (2 * checkpointFrequency)
                                               : (checkpointFrequency / 2);
        mTestCfg.ARTIFICIALLY_DELAY_BUCKET_APPLICATION_FOR_TESTING =
            std::chrono::seconds(delayBuckets);

        // Start test app
        auto app = mSimulation->addNode(mTestCfg.NODE_SEED, mQuorum, &mTestCfg,
                                        false, startFromLedger, startFromHash);
        mSimulation->addPendingConnection(mMainNodeID, mTestNodeID);
        REQUIRE(app);
        mSimulation->startAllNodes();

        // Ensure nodes are connected
        if (!app->getConfig().MODE_AUTO_STARTS_OVERLAY)
        {
            app->getOverlayManager().start();
        }

        if (isInMemoryMode)
        {
            REQUIRE(app->getLedgerManager().getState() ==
                    LedgerManager::LM_CATCHING_UP_STATE);
        }

        auto downloaded =
            app->getCatchupManager().getCatchupMetrics().mCheckpointsDownloaded;

        Upgrades::UpgradeParameters scheduledUpgrades;
        scheduledUpgrades.mUpgradeTime =
            VirtualClock::from_time_t(mMainNode->getLedgerManager()
                                          .getLastClosedLedgerHeader()
                                          .header.scpValue.closeTime);
        scheduledUpgrades.mProtocolVersion =
            static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION);
        mMainNode->getHerder().setUpgrades(scheduledUpgrades);

        generateLoad(false);
        generateLoad(true);

        // State has been rebuilt and node is properly in sync
        REQUIRE(checkState(*app));
        REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() ==
                getMainNodeLCL().header.ledgerSeq);
        REQUIRE(app->getLedgerManager().isSynced());

        if (triggerCatchup)
        {
            REQUIRE(downloaded < app->getCatchupManager()
                                     .getCatchupMetrics()
                                     .mCheckpointsDownloaded);
        }
        else
        {
            REQUIRE(downloaded == app->getCatchupManager()
                                      .getCatchupMetrics()
                                      .mCheckpointsDownloaded);
        }
    }
};

TEST_CASE("verify checkpoints command - wait condition", "[applicationutils]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);

    SIMULATION_CREATE_NODE(Node1); // Validator
    SIMULATION_CREATE_NODE(Node2); // Captive core

    SCPQuorumSet qSet;
    qSet.threshold = 1;
    qSet.validators.push_back(vNode1NodeID);

    Config cfg1 = getTestConfig(1);
    Config cfg2 = getTestConfig(2, Config::TESTDB_IN_MEMORY_NO_OFFERS);
    cfg2.FORCE_SCP = false;
    cfg2.NODE_IS_VALIDATOR = false;
    cfg2.MODE_DOES_CATCHUP = false;

    auto validator = simulation->addNode(vNode1SecretKey, qSet, &cfg1);
    Application::pointer watcher;

    SECTION("in sync with the network")
    {
        watcher = simulation->addNode(vNode2SecretKey, qSet, &cfg2);
        simulation->addPendingConnection(vNode1NodeID, vNode2NodeID);
        simulation->startAllNodes();
    }
    SECTION("buffer ledgers")
    {
        simulation->startAllNodes();
        simulation->crankForAtLeast(std::chrono::minutes(2), false);
        watcher = simulation->addNode(vNode2SecretKey, qSet, &cfg2);
        simulation->addPendingConnection(vNode1NodeID, vNode2NodeID);
        simulation->startAllNodes();
    }

    LedgerNumHashPair authPair;
    while (!authPair.second)
    {
        simulation->crankForAtMost(std::chrono::seconds(1), false);
        setAuthenticatedLedgerHashPair(watcher, authPair, 0, "");
    }

    REQUIRE(authPair.second);
    REQUIRE(authPair.first);
}

TEST_CASE("offline self-check works", "[applicationutils][selfcheck]")
{
    // Step 1: set up history archives and publish to them.
    Config chkConfig;
    CatchupSimulation catchupSimulation{};
    auto l1 = catchupSimulation.getLastCheckpointLedger(2);
    auto l2 = catchupSimulation.getLastCheckpointLedger(4);
    catchupSimulation.ensureOfflineCatchupPossible(l2);
    std::filesystem::path victimBucketPath;
    {
        // Step 2: make a new application and catch it up part-way to the
        // archives (but behind).
        auto app = catchupSimulation.createCatchupApplication(
            std::numeric_limits<uint32_t>::max(), Config::TESTDB_ON_DISK_SQLITE,
            "client");
        catchupSimulation.catchupOffline(app, l1);
        chkConfig = app->getConfig();
        victimBucketPath = app->getBucketManager()
                               .getBucketList()
                               .getLevel(0)
                               .getCurr()
                               ->getFilename();
    }

    // Step 3: run offline self-check on that application's state, see that it's
    // agreeable.
    REQUIRE(selfCheck(chkConfig) == 0);

    std::filesystem::path archPath =
        catchupSimulation.getHistoryConfigurator().getArchiveDirName();

    // Step 4: require various self-check failures are caught.
    {
        // Damage the well-known HAS path in the archive.
        auto path = archPath;
        path /= HistoryArchiveState::wellKnownRemoteName();
        TemporaryFileDamager damage(path);
        damage.damageVictim();
        REQUIRE(selfCheck(chkConfig) == 1);
    }
    {
        // Damage the target ledger in the archive.
        auto path = archPath;
        path /= fs::remoteName(typeString(FileType::HISTORY_FILE_TYPE_LEDGER),
                               fs::hexStr(l1), "xdr.gz");
        TemporaryFileDamager damage(path);
        damage.damageVictim();
        REQUIRE(selfCheck(chkConfig) == 1);
    }
    {
        // Damage a bucket file.
        TemporaryFileDamager damage(victimBucketPath);
        damage.damageVictim();
        REQUIRE(selfCheck(chkConfig) == 1);
    }
    {
        // Damage the SQL ledger.
        TemporarySQLiteDBDamager damage(chkConfig);
        damage.damageVictim();
        REQUIRE(selfCheck(chkConfig) == 1);
    }
}

TEST_CASE("application setup", "[applicationutils]")
{
    VirtualClock clock;

    SECTION("SQL DB mode")
    {
        auto cfg = getTestConfig();
        auto app = setupApp(cfg, clock, 0, "");
        REQUIRE(checkState(*app));
    }

    auto testInMemoryMode = [&](Config& cfg1, Config& cfg2) {
        // Publish a few checkpoints then shut down test node
        auto simulation = SimulationHelper(cfg1, cfg2);
        auto [startFromLedger, startFromHash] =
            simulation.publishCheckpoints(2);
        auto lcl = simulation.getTestNodeLCL();
        simulation.shutdownTestNode();

        SECTION("minimal DB setup")
        {
            SECTION("not found")
            {
                // Remove `buckets` dir completely
                fs::deltree(cfg2.BUCKET_DIR_PATH);

                // Initialize new minimal DB from scratch
                auto app = setupApp(cfg2, clock, 0, "");
                REQUIRE(app);
                REQUIRE(checkState(*app));
            }
            SECTION("found")
            {
                // Found existing minimal DB, reset to genesis
                auto app = setupApp(cfg2, clock, 0, "");
                REQUIRE(app);
                REQUIRE(checkState(*app));
            }
        }
        SECTION("rebuild state")
        {
            SECTION("from buckets")
            {
                auto selectedLedger = lcl.header.ledgerSeq;
                auto selectedHash = binToHex(lcl.hash);

                SECTION("replay buffered ledgers")
                {
                    simulation.runStartupTest(false, selectedLedger,
                                              selectedHash,
                                              lcl.header.ledgerSeq);
                }
                SECTION("trigger catchup")
                {
                    simulation.runStartupTest(true, selectedLedger,
                                              selectedHash,
                                              lcl.header.ledgerSeq);
                }
                SECTION("start from future ledger")
                {
                    // Validator publishes more checkpoints while the
                    // captive-core instance is shutdown
                    auto [selectedLedger2, selectedHash2] =
                        simulation.publishCheckpoints(4);
                    simulation.runStartupTest(true, selectedLedger2,
                                              selectedHash2,
                                              lcl.header.ledgerSeq);
                }
            }
            SECTION("via catchup")
            {
                // startAtLedger is behind LCL, reset to genesis and catchup
                REQUIRE(!canRebuildInMemoryLedgerFromBuckets(
                    startFromLedger, lcl.header.ledgerSeq));
                auto app =
                    setupApp(cfg2, clock, startFromLedger, startFromHash);
                REQUIRE(app);
                REQUIRE(checkState(*app));
                REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() ==
                        startFromLedger);
                REQUIRE(app->getLedgerManager().getState() ==
                        LedgerManager::LM_CATCHING_UP_STATE);
            }

            SECTION("bad hash")
            {
                // Create mismatch between start-from ledger and hash
                auto app =
                    setupApp(cfg2, clock, startFromLedger + 1, startFromHash);
                REQUIRE(!app);
            }
        }
        SECTION("set meta stream")
        {
            TmpDirManager tdm(std::string("streamtmp-") +
                              binToHex(randomBytes(8)));
            TmpDir td = tdm.tmpDir("streams");
            std::string path = td.getName() + "/stream.xdr";

            // Remove `buckets` dir completely to ensure multiple apps are
            // initialized during setup
            fs::deltree(cfg2.BUCKET_DIR_PATH);
            SECTION("file path")
            {
                cfg2.METADATA_OUTPUT_STREAM = path;

                auto app = setupApp(cfg2, clock, 0, "");
                REQUIRE(app);
                REQUIRE(checkState(*app));
            }
#ifdef _WIN32
#else
            SECTION("fd")
            {
                int fd = ::open(path.c_str(), O_CREAT | O_WRONLY, 0644);
                REQUIRE(fd != -1);
                cfg2.METADATA_OUTPUT_STREAM = fmt::format("fd:{}", fd);

                auto app = setupApp(cfg2, clock, 0, "");
                REQUIRE(app);
                REQUIRE(checkState(*app));
            }
#endif
        }
    };
    SECTION("in memory mode")
    {
        Config cfg1 = getTestConfig(1);
        Config cfg2 = getTestConfig(2, Config::TESTDB_IN_MEMORY_NO_OFFERS);
        cfg2.DATABASE = SecretValue{minimalDBForInMemoryMode(cfg2)};
        testInMemoryMode(cfg1, cfg2);
    }
}

TEST_CASE("application major version numbers", "[applicationutils]")
{
    CHECK(getStellarCoreMajorReleaseVersion("v19.0.0") ==
          std::make_optional<uint32_t>(19));
    CHECK(getStellarCoreMajorReleaseVersion("v19.1.3") ==
          std::make_optional<uint32_t>(19));
    CHECK(getStellarCoreMajorReleaseVersion("v19.1.3rc0") ==
          std::make_optional<uint32_t>(19));
    CHECK(getStellarCoreMajorReleaseVersion("v20.0.0rc1") ==
          std::make_optional<uint32_t>(20));
    CHECK(getStellarCoreMajorReleaseVersion("v20.0.0HOT4") ==
          std::make_optional<uint32_t>(20));
    CHECK(getStellarCoreMajorReleaseVersion("v19.1.2-10") == std::nullopt);
    CHECK(getStellarCoreMajorReleaseVersion("v19.9.0-30-g726eabdea-dirty") ==
          std::nullopt);
}

TEST_CASE("standalone quorum intersection check", "[applicationutils]")
{
    Config cfg = getTestConfig();
    const std::string JSON_ROOT = "testdata/check-quorum-intersection-json/";

    SECTION("enjoys quorum intersection")
    {
        REQUIRE(checkQuorumIntersectionFromJson(
            JSON_ROOT + "enjoys-intersection.json", cfg));
    }

    SECTION("does not enjoy quorum intersection")
    {
        REQUIRE(!checkQuorumIntersectionFromJson(
            JSON_ROOT + "no-intersection.json", cfg));
    }

    SECTION("malformed JSON")
    {
        // Test various bad JSON inputs

        // Malformed key
        REQUIRE_THROWS_AS(
            checkQuorumIntersectionFromJson(JSON_ROOT + "bad-key.json", cfg),
            KeyUtils::InvalidStrKey);

        // Wrong datatype
        REQUIRE_THROWS_AS(checkQuorumIntersectionFromJson(
                              JSON_ROOT + "bad-threshold-type.json", cfg),
                          std::runtime_error);

        // No such file
        REQUIRE_THROWS_AS(
            checkQuorumIntersectionFromJson(JSON_ROOT + "no-file.json", cfg),
            std::runtime_error);
    }
}