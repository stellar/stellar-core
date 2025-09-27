// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/HistoryArchiveManager.h"
#include "history/HistoryManagerImpl.h"
#include "history/test/HistoryTestsUtils.h"
#include "invariant/BucketListIsConsistentWithDatabase.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "main/ApplicationUtils.h"
#include "main/CommandHandler.h"
#include "main/Config.h"
#include "simulation/Simulation.h"
#include "test/Catch2.h"
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
    auto JSON_ROOT = getSrcTestDataPath("check-quorum-intersection-json");

    SECTION("enjoys quorum intersection")
    {
        REQUIRE(checkQuorumIntersectionFromJson(
            JSON_ROOT / "enjoys-intersection.json", cfg));
    }

    SECTION("does not enjoy quorum intersection")
    {
        REQUIRE(!checkQuorumIntersectionFromJson(
            JSON_ROOT / "no-intersection.json", cfg));
    }

    SECTION("malformed JSON")
    {
        // Test various bad JSON inputs

        // Malformed key
        REQUIRE_THROWS_AS(
            checkQuorumIntersectionFromJson(JSON_ROOT / "bad-key.json", cfg),
            KeyUtils::InvalidStrKey);

        // Wrong datatype
        REQUIRE_THROWS_AS(checkQuorumIntersectionFromJson(
                              JSON_ROOT / "bad-threshold-type.json", cfg),
                          std::runtime_error);

        // No such file
        REQUIRE_THROWS_AS(
            checkQuorumIntersectionFromJson(JSON_ROOT / "no-file.json", cfg),
            std::runtime_error);
    }
}
