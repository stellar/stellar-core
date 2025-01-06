// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

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
#include "simulation/Simulation.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"
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

        auto checkpoint = HistoryManager::getCheckpointFrequency(mMainCfg);

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

    LedgerHeaderHistoryEntry
    getMainNodeLCL()
    {
        return mSimulation->getNode(mMainNodeID)
            ->getLedgerManager()
            .getLastClosedLedgerHeader();
    }

    LedgerHeaderHistoryEntry
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
    Config cfg2 = getTestConfig(2, Config::TESTDB_IN_MEMORY);
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
            std::numeric_limits<uint32_t>::max(),
            Config::TESTDB_BUCKET_DB_PERSISTENT, "client");
        catchupSimulation.catchupOffline(app, l1);
        chkConfig = app->getConfig();
        victimBucketPath = app->getBucketManager()
                               .getLiveBucketList()
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