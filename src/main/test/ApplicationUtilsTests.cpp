// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Random.h"
#include "history/HistoryArchiveManager.h"
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
        auto filenameOp = app->getBucketManager()
                              .getBucketList()
                              .getLevel(0)
                              .getCurr()
                              ->getFilename(BucketSortOrder::SortByType);
        REQUIRE(filenameOp.has_value());
        victimBucketPath = *filenameOp;
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
        path /=
            fs::remoteName(HISTORY_FILE_TYPE_LEDGER, fs::hexStr(l1), "xdr.gz");
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
    // Logic to check the state of the bucket list with the state of the DB
    auto checkState = [](Application& app) {
        BucketListIsConsistentWithDatabase blc(app);
        bool blcOk = true;
        try
        {
            blc.checkEntireBucketlist();
        }
        catch (std::runtime_error& e)
        {
            LOG_ERROR(DEFAULT_LOG,
                      "Error during bucket-list consistency check: {}",
                      e.what());
            blcOk = false;
        }
        return blcOk;
    };

    VirtualClock clock;

    SECTION("normal mode")
    {
        auto cfg = getTestConfig();
        auto app = setupApp(cfg, clock, 0, "");
        REQUIRE(checkState(*app));
    }
    SECTION("in memory mode")
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
        cfg1.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
        TmpDirHistoryConfigurator histCfg;
        // Setup validator to publish
        cfg1 = histCfg.configure(cfg1, /* writable */ true);
        cfg1.MAX_SLOTS_TO_REMEMBER = 50;

        Config cfg2 = getTestConfig(2);
        cfg2.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
        cfg2.NODE_IS_VALIDATOR = false;
        cfg2.FORCE_SCP = false;
        cfg2.INVARIANT_CHECKS = {};
        cfg2.setInMemoryMode();
        cfg2.MODE_AUTO_STARTS_OVERLAY = false;
        cfg2.DATABASE = SecretValue{minimalDBForInMemoryMode(cfg2)};
        cfg2.MODE_STORES_HISTORY_LEDGERHEADERS = true;
        // Captive core points to a read-only archive maintained by the
        // validator
        cfg2 = histCfg.configure(cfg2, /* writable */ false);

        auto validator = simulation->addNode(vNode1SecretKey, qSet, &cfg1);
        validator->getHistoryArchiveManager().initializeHistoryArchive(
            histCfg.getArchiveDirName());

        simulation->addNode(vNode2SecretKey, qSet, &cfg2);
        simulation->addPendingConnection(vNode1NodeID, vNode2NodeID);
        simulation->startAllNodes();

        simulation->crankUntil(
            [&]() { return simulation->haveAllExternalized(3, 5); },
            10 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

        // Generate a bit of load, and crank for some time
        auto& loadGen = validator->getLoadGenerator();
        loadGen.generateLoad(LoadGenMode::CREATE, /* nAccounts */ 10, 0, 0,
                             /*txRate*/ 1,
                             /*batchSize*/ 1, std::chrono::seconds(0), 0);

        auto& loadGenDone = validator->getMetrics().NewMeter(
            {"loadgen", "run", "complete"}, "run");
        auto currLoadGenCount = loadGenDone.count();

        auto checkpoint =
            validator->getHistoryManager().getCheckpointFrequency();

        uint32_t selectedLedger = 0;
        std::string selectedHash;

        // Make sure validator publishes something
        simulation->crankUntil(
            [&]() {
                bool loadDone = loadGenDone.count() > currLoadGenCount;
                auto lcl =
                    validator->getLedgerManager().getLastClosedLedgerHeader();
                // Pick some ledger in the second checkpoint to run catchup
                // against later
                if (lcl.header.ledgerSeq < 2 * checkpoint)
                {
                    selectedLedger = lcl.header.ledgerSeq;
                    selectedHash = binToHex(lcl.hash);
                }

                // Validator should publish at least two checkpoints
                return loadDone &&
                       simulation->haveAllExternalized(3 * checkpoint, 5);
            },
            50 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

        REQUIRE(selectedLedger != 0);
        REQUIRE(!selectedHash.empty());

        auto lcl = simulation->getNode(vNode2NodeID)
                       ->getLedgerManager()
                       .getLastClosedLedgerHeader();

        // Shutdown captive core and test various startup paths
        simulation->removeNode(vNode2NodeID);

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
                selectedLedger = lcl.header.ledgerSeq;
                selectedHash = binToHex(lcl.hash);

                auto runTest = [&](bool triggerCatchup) {
                    REQUIRE(canRebuildInMemoryLedgerFromBuckets(
                        selectedLedger, lcl.header.ledgerSeq));
                    uint32_t checkpointFrequency = 8;

                    // Depending on how many ledgers we buffer during bucket
                    // apply, core might trim some and only keep checkpoint
                    // ledgers. In this case, after bucket application, normal
                    // catchup will be triggered.
                    uint32_t delayBuckets = triggerCatchup
                                                ? (2 * checkpointFrequency)
                                                : (checkpointFrequency / 2);
                    cfg2.ARTIFICIALLY_DELAY_BUCKET_APPLICATION_FOR_TESTING =
                        std::chrono::seconds(delayBuckets);
                    auto app =
                        simulation->addNode(vNode2SecretKey, qSet, &cfg2, false,
                                            selectedLedger, selectedHash);
                    simulation->addPendingConnection(vNode1NodeID,
                                                     vNode2NodeID);
                    REQUIRE(app);

                    simulation->startAllNodes();
                    // Ensure nodes are connected
                    if (!app->getConfig().MODE_AUTO_STARTS_OVERLAY)
                    {
                        app->getOverlayManager().start();
                    }
                    REQUIRE(app->getLedgerManager().getState() ==
                            LedgerManager::LM_CATCHING_UP_STATE);

                    auto downloaded = app->getCatchupManager()
                                          .getCatchupMetrics()
                                          .mCheckpointsDownloaded;

                    // Generate a bit of load, and crank for some time
                    loadGen.generateLoad(
                        LoadGenMode::PAY, /* nAccounts */ 10, 0, 10,
                        /*txRate*/ 1,
                        /*batchSize*/ 1, std::chrono::seconds(0), 0);

                    auto currLoadGenCount = loadGenDone.count();

                    simulation->crankUntil(
                        [&]() {
                            bool loadDone =
                                loadGenDone.count() > currLoadGenCount;
                            auto lcl = validator->getLedgerManager()
                                           .getLastClosedLedgerHeader();
                            return loadDone &&
                                   app->getLedgerManager().isSynced();
                        },
                        50 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

                    // State has been rebuilt and node is properly in sync
                    REQUIRE(checkState(*app));

                    REQUIRE(
                        app->getLedgerManager().getLastClosedLedgerNum() ==
                        validator->getLedgerManager().getLastClosedLedgerNum());
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
                };

                SECTION("replay buffered ledgers")
                {
                    runTest(false);
                }
                SECTION("trigger catchup")
                {
                    runTest(true);
                }
            }
            SECTION("via catchup")
            {
                // startAtLedger is behind LCL, reset to genesis and catchup
                REQUIRE(!canRebuildInMemoryLedgerFromBuckets(
                    selectedLedger, lcl.header.ledgerSeq));
                auto app = setupApp(cfg2, clock, selectedLedger, selectedHash);
                REQUIRE(app);
                REQUIRE(checkState(*app));
                REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() ==
                        selectedLedger);
                REQUIRE(app->getLedgerManager().getState() ==
                        LedgerManager::LM_CATCHING_UP_STATE);
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
    }
}
