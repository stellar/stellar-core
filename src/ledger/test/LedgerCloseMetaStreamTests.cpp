// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketManager.h"
#include "bucket/test/BucketTestUtils.h"
#include "catchup/ReplayDebugMetaWork.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "crypto/SecretKey.h"
#include "history/HistoryArchiveManager.h"
#include "history/test/HistoryTestsUtils.h"
#include "ledger/FlushAndRotateMetaDebugWork.h"
#include "ledger/LedgerTxn.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/ApplicationUtils.h"
#include "simulation/Simulation.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/test/SorobanTxTestUtils.h"
#include "util/DebugMetaUtils.h"
#include "util/Logging.h"
#include "util/MetaUtils.h"
#include "util/ProtocolVersion.h"
#include "util/XDRCereal.h"
#include "util/XDRStream.h"
#include "work/WorkScheduler.h"
#include "xdr/Stellar-ledger.h"
#include "xdr/Stellar-transaction.h"
#include <fmt/format.h>
#include <fstream>
#include <iterator>

using namespace stellar;

TEST_CASE("LedgerCloseMetaStream file descriptor - LIVE_NODE",
          "[ledgerclosemetastreamlive]")
{
    // Live requires a multinode simulation, as we're not allowed to run a
    // validator and record metadata streams at the same time (to avoid the
    // unbounded-latency stream-write step): N nodes participating in consensus,
    // and one watching and streaming metadata

    Hash expectedLastUnsafeHash, expectedLastSafeHash;
    TmpDirManager tdm(std::string("streamtmp-") + binToHex(randomBytes(8)));
    TmpDir td = tdm.tmpDir("streams");
    std::string metaPath = td.getName() + "/stream.xdr";
    std::string metaPathSafe = td.getName() + "/streamSafe.xdr";

    uint32 const ledgerToWaitFor = 10;

    bool const induceOneLedgerFork = GENERATE(false, true);
    CAPTURE(induceOneLedgerFork);
    auto const ledgerToCorrupt = 5;
    static_assert(ledgerToCorrupt < ledgerToWaitFor,
                  "must wait beyond corrupt ledger");

    auto const expectedLastWatcherLedger =
        induceOneLedgerFork ? ledgerToCorrupt : ledgerToWaitFor;

    {
        // Step 1: Set up a 5 node simulation with 3 validators and 2 watchers.
        auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
        auto simulation =
            std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);

        SIMULATION_CREATE_NODE(Node1); // Validator
        SIMULATION_CREATE_NODE(Node2); // Validator
        SIMULATION_CREATE_NODE(Node3); // Validator
        SIMULATION_CREATE_NODE(Node4); // Watcher

        SCPQuorumSet qSet;
        qSet.threshold = 3;
        qSet.validators.push_back(vNode1NodeID);
        qSet.validators.push_back(vNode2NodeID);
        qSet.validators.push_back(vNode3NodeID);

        Config const& cfg1 = getTestConfig(1);
        Config const& cfg2 = getTestConfig(2);
        Config const& cfg3 = getTestConfig(3);
        Config cfg4 = getTestConfig(4);

        // Step 2: open writable files and pass them to watcher config
        cfg4.NODE_IS_VALIDATOR = false;
        cfg4.FORCE_SCP = false;
#ifdef _WIN32
        cfg4.METADATA_OUTPUT_STREAM = metaPath;
#else
        int fd = ::open(metaPath.c_str(), O_CREAT | O_WRONLY, 0644);
        REQUIRE(fd != -1);
        cfg4.METADATA_OUTPUT_STREAM = fmt::format(FMT_STRING("fd:{}"), fd);
#endif

        // Step 3: Run simulation a few steps to stream metadata.
        auto app1 = simulation->addNode(vNode1SecretKey, qSet, &cfg1);
        auto app2 = simulation->addNode(vNode2SecretKey, qSet, &cfg2);
        auto app3 = simulation->addNode(vNode3SecretKey, qSet, &cfg3);
        auto app4 = simulation->addNode(vNode4SecretKey, qSet, &cfg4);

        simulation->addPendingConnection(vNode1NodeID, vNode2NodeID);
        simulation->addPendingConnection(vNode1NodeID, vNode3NodeID);
        simulation->addPendingConnection(vNode1NodeID, vNode4NodeID);

        simulation->startAllNodes();
        bool watchersAreCorrupted = false;
        simulation->crankUntil(
            [&]() {
                // As long as the watchers are in sync, wait for them to get the
                // news of all the ledgers closed by the validators.  But once
                // the watchers are corrupt, they won't be able to close more
                // ledgers, so at that point we start waiting only for the
                // validators to do so.
                if (watchersAreCorrupted)
                {
                    return app1->getLedgerManager().getLastClosedLedgerNum() ==
                           ledgerToWaitFor;
                }

                auto const lastClosedLedger =
                    app4->getLedgerManager().getLastClosedLedgerNum();

                if (lastClosedLedger == expectedLastWatcherLedger - 1)
                {
                    expectedLastSafeHash = app4->getLedgerManager()
                                               .getLastClosedLedgerHeader()
                                               .hash;

                    if (induceOneLedgerFork)
                    {
                        txtest::closeLedgerOn(
                            *app4, ledgerToCorrupt,
                            app4->getLedgerManager()
                                    .getLastClosedLedgerHeader()
                                    .header.scpValue.closeTime +
                                1);

                        expectedLastUnsafeHash =
                            app4->getLedgerManager()
                                .getLastClosedLedgerHeader()
                                .hash;

                        watchersAreCorrupted = true;
                        return false;
                    }
                }

                return lastClosedLedger == ledgerToWaitFor;
            },
            std::chrono::seconds{200}, false);

        REQUIRE(app4->getLedgerManager().getLastClosedLedgerNum() ==
                expectedLastWatcherLedger);

        if (!induceOneLedgerFork)
        {
            expectedLastUnsafeHash =
                app4->getLedgerManager().getLastClosedLedgerHeader().hash;
        }
    }

    // Step 4: reopen the files as XDR streams and read back the LCMs
    // and check they have the expected content.
    auto readLcms = [](std::string const& lcmPath) {
        std::vector<LedgerCloseMeta> lcms;
        XDRInputFileStream stream;
        stream.open(lcmPath);
        LedgerCloseMeta lcm;
        while (stream && stream.readOne(lcm))
        {
            lcms.push_back(lcm);
        }
        return lcms;
    };

    auto lcms = readLcms(metaPath);
    // The "- 1" is because we don't stream meta for the genesis ledger.
    REQUIRE(lcms.size() == expectedLastWatcherLedger - 1);
    if (lcms.back().v() == 0)
    {
        REQUIRE(lcms.back().v0().ledgerHeader.hash == expectedLastUnsafeHash);
    }
    else if (lcms.back().v() == 1)
    {
        REQUIRE(lcms.back().v1().ledgerHeader.hash == expectedLastUnsafeHash);
    }
    else
    {
        REQUIRE(false);
    }
}

TEST_CASE("METADATA_DEBUG_LEDGERS works", "[metadebug]")
{
    VirtualClock clock;
    Config cfg = getTestConfig();
    cfg.MANUAL_CLOSE = false;
    cfg.METADATA_DEBUG_LEDGERS = 768;
    auto app = createTestApplication(clock, cfg);
    app->start();
    auto bucketDir = app->getBucketManager().getBucketDir();
    auto n = metautils::getNumberOfDebugFilesToKeep(cfg.METADATA_DEBUG_LEDGERS);
    bool gotToExpectedSize = false;
    auto& lm = app->getLedgerManager();
    bool debugFilesGenerated = false;
    auto const& debugFilePath = metautils::getMetaDebugDirPath(bucketDir);

    auto closeLedgers = [&](uint32_t numLedgers) {
        while (lm.getLastClosedLedgerNum() < numLedgers)
        {
            clock.crank(false);
            // Don't check for debug files until the directory has been
            // generated
            if (!debugFilesGenerated)
            {
                debugFilesGenerated = fs::exists(debugFilePath.string());
            }

            if (app->getWorkScheduler().allChildrenDone() &&
                debugFilesGenerated)
            {
                auto files = metautils::listMetaDebugFiles(bucketDir);
                REQUIRE(files.size() <= n);
                if (files.size() + 1 >= n)
                {
                    gotToExpectedSize = true;
                }
            }
        }
        while (!app->getWorkScheduler().allChildrenDone())
        {
            clock.crank(false);
        }
        REQUIRE(gotToExpectedSize);
    };

    SECTION("meta zipped and garbage collected")
    {
        closeLedgers(2 * cfg.METADATA_DEBUG_LEDGERS);
    }
    SECTION("meta replayed")
    {
        // Generate just enough meta to not triggers garbage collection
        closeLedgers(cfg.METADATA_DEBUG_LEDGERS);
        app->gracefulStop();

        // Verify presence of the latest debug tx set
        auto txSetPath = metautils::getLatestTxSetFilePath(bucketDir);
        REQUIRE(fs::exists(txSetPath.string()));

        StoredDebugTransactionSet sts;
        {
            XDRInputFileStream in;
            in.open(txSetPath.string());
            in.readOne(sts);
        }
        REQUIRE(sts.ledgerSeq == lm.getLastClosedLedgerNum());

        // Now test replay from meta
        VirtualClock clock2;
        Config cfg2 = getTestConfig(1);
        auto replayApp = createTestApplication(clock2, cfg2);
        replayApp->start();

        auto replayWork =
            replayApp->getWorkScheduler().executeWork<ReplayDebugMetaWork>(
                lm.getLastClosedLedgerNum(), bucketDir);
        REQUIRE(replayWork->getState() == BasicWork::State::WORK_SUCCESS);
        REQUIRE(replayApp->getLedgerManager().getLastClosedLedgerNum() ==
                lm.getLastClosedLedgerNum());
    }
}

TEST_CASE_VERSIONS("meta stream contains reasonable meta", "[ledgerclosemeta]")
{
    auto test = [&](Config cfg, bool isSoroban) {
        using namespace stellar::txtest;

        // We need to fix a deterministic NODE_SEED for this test to be stable.
        cfg.NODE_SEED = SecretKey::pseudoRandomForTestingFromSeed(12345);
        auto prefix = isSoroban ? std::string("metatest-soroban-")
                                : std::string("metatest-");
        TmpDirManager tdm(prefix + binToHex(randomBytes(8)));
        TmpDir td = tdm.tmpDir("meta-ok");
        std::string metaPath = td.getName() + "/stream.xdr";

        cfg.METADATA_OUTPUT_STREAM = metaPath;
        cfg.USE_CONFIG_FOR_GENESIS = true;

        // LedgerNum that we will examine the meta at
        uint32_t targetSeq;

        if (isSoroban)
        {
            // This test will submit several interesting soroban TXs. First, we
            // will deploy a contract and let it become ARCHIVED. We'll then
            // submit the following TXs
            // 1. Restore archived entry
            // 2. Extend contract
            // 3. Invoke contract that creates new storage
            // 4. Failed invoke contract (bad footprint)
            // 5. Create contract instance

            // Deploy and extend contract so it won't be archived during test
            SorobanTest test(cfg);
            ContractStorageTestClient client(test);
            auto& contract = client.getContract();

            test.invokeExtendOp(contract.getKeys(), 10'000);

            // Write key that we will restore later
            REQUIRE(client.put("archived", ContractDataDurability::PERSISTENT,
                               42) == INVOKE_HOST_FUNCTION_SUCCESS);
            auto archivedKeySymbol = makeSymbolSCVal("archived");
            auto archivedLk =
                contractDataKey(contract.getAddress(), archivedKeySymbol,
                                ContractDataDurability::PERSISTENT);

            uint32_t liveUntilLedger =
                test.getLCLSeq() +
                test.getNetworkCfg().stateArchivalSettings().minPersistentTTL -
                1;

            // Generate a few accounts so we can send multiple TXs in a single
            // ledger.
            auto bal = test.getApp().getLedgerManager().getLastMinBalance(2);
            auto acc1 = test.getRoot().create("acc1", bal);
            auto acc2 = test.getRoot().create("acc2", bal);
            auto acc3 = test.getRoot().create("acc3", bal);
            auto acc4 = test.getRoot().create("acc4", bal);
            auto acc5 = test.getRoot().create("acc5", bal);

            // Close ledgers until out contract expires. These ledgers won't
            // emit meta
            for (uint32_t i =
                     test.getApp().getLedgerManager().getLastClosedLedgerNum();
                 i <= liveUntilLedger + 1; ++i)
            {
                closeLedgerOn(test.getApp(), i, 2, 1, 2016);
            }
            REQUIRE(!test.isEntryLive(archivedLk, test.getLCLSeq()));

            // Restore archived entry
            SorobanResources restoreResources;
            restoreResources.footprint.readWrite = {archivedLk};
            restoreResources.instructions = 0;
            restoreResources.readBytes = 5'000;
            restoreResources.writeBytes = 1'000;
            auto tx1 = test.createRestoreTx(restoreResources, 1'000,
                                            DEFAULT_TEST_RESOURCE_FEE, &acc1);

            // Extend TTL of Wasm and instance
            SorobanResources extendResources;
            extendResources.footprint.readOnly = contract.getKeys();
            extendResources.instructions = 0;
            extendResources.readBytes = 10'000;
            extendResources.writeBytes = 0;
            auto tx2 =
                test.createExtendOpTx(extendResources, /*extendTo*/ 10'000,
                                      1'000, DEFAULT_TEST_RESOURCE_FEE, &acc2);

            // Write new entry with key PERSISTENT("key")
            auto liveKeySymbol = makeSymbolSCVal("key");
            auto fnName = "put_persistent";
            auto args = {liveKeySymbol, makeU64SCVal(42)};
            auto liveLk = contractDataKey(client.getContract().getAddress(),
                                          liveKeySymbol,
                                          ContractDataDurability::PERSISTENT);
            auto putSpec = SorobanInvocationSpec()
                               .setInstructions(4'000'000)
                               .setReadBytes(10'000)
                               .setWriteBytes(1'000)
                               .setInclusionFee(1'000)
                               .setRefundableResourceFee(40'000)
                               .setReadWriteFootprint({liveLk});

            auto tx3 = contract.prepareInvocation(fnName, args, putSpec)
                           .withExactNonRefundableResourceFee()
                           .createTx(&acc3);

            // Same put TX from before, but this one should fail due to invalid
            // footprint
            auto tx4 = contract
                           .prepareInvocation(fnName, args,
                                              putSpec.setReadWriteFootprint({}))
                           .withExactNonRefundableResourceFee()
                           .createTx(&acc4);
            SorobanResources createResources;
            createResources.instructions = 200'000;
            createResources.readBytes = 5000;
            createResources.writeBytes = 5000;

            auto tx5 = makeSorobanCreateContractTx(
                test.getApp(), acc5,
                makeContractIDPreimage(acc5, sha256("salt")),
                makeWasmExecutable(contract.getKeys()[0].contractCode().hash),
                createResources, 1000);

            closeLedger(test.getApp(), {tx1, tx2, tx3, tx4, tx5});
            targetSeq = 28;
        }
        else
        {
            // Do some stuff
            VirtualClock clock;
            auto app = createTestApplication(clock, cfg);
            auto& lm = app->getLedgerManager();
            auto txFee = lm.getLastTxFee();
            auto bal = app->getLedgerManager().getLastMinBalance(2);

            // We're going to examine the meta generated by ledger #6.
            targetSeq = 6;

            if (appProtocolVersionStartsFrom(*app, SOROBAN_PROTOCOL_VERSION))
            {
                modifySorobanNetworkConfig(
                    *app, [&](SorobanNetworkConfig& cfg) {
                        cfg.mStateArchivalSettings
                            .bucketListWindowSamplePeriod = 1;
                    });

                // Modify Soroban network config closes a ledger
                ++targetSeq;
            }

            auto root = TestAccount::createRoot(*app);

            // Ledgers #2, #3 and #4 create accounts, which happen directly
            // and don't emit meta.
            auto acc1 = root.create("acc1", bal);
            auto acc2 = root.create("acc2", bal);
            auto issuer =
                root.create("issuer", lm.getLastMinBalance(0) + 100 * txFee);
            auto cur1 = issuer.asset("CUR1");

            // Ledger #5 sets up a trustline which has to happen before we
            // can use it.
            acc1.changeTrust(cur1, 100);

            // Ledger #6 uses closeLedger so emits interesting meta.
            std::vector<TransactionFrameBasePtr> txs = {
                // First tx pays 1000 XLM from root to acc1
                root.tx({payment(acc1.getPublicKey(), 1000)}),
                // Second tx pays acc1 50 cur1 units twice from issuer.
                issuer.tx({payment(acc1, cur1, 50), payment(acc1, cur1, 50)})};
            if (protocolVersionStartsFrom(
                    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                    ProtocolVersion::V_13))
            {
                // If we're in the world where fee-bumps exist (protocol 13
                // or later), we re-wrap the final tx in a fee-bump from
                // acc2.
                auto tx = txs.back();
                txs.back() = feeBump(
                    *app, acc2,
                    std::static_pointer_cast<TransactionTestFrame const>(tx),
                    5000);
            }
            closeLedger(*app, txs);
        }

        XDRInputFileStream in;
        in.open(metaPath);
        LedgerCloseMeta lcm;
        uint32_t maxSeq = 0;
        while (in.readOne(lcm))
        {
            uint32_t ledgerSeq{0};

            if (protocolVersionIsBefore(
                    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                    SOROBAN_PROTOCOL_VERSION))
            {
                // LCM v0
                REQUIRE(lcm.v() == 0);
                REQUIRE(lcm.v0().ledgerHeader.header.ledgerVersion ==
                        cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION);
                ledgerSeq = lcm.v0().ledgerHeader.header.ledgerSeq;
            }
            else
            {
                // LCM v1
                REQUIRE(lcm.v() == 1);
                REQUIRE(lcm.v1().ledgerHeader.header.ledgerVersion ==
                        cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION);
                ledgerSeq = lcm.v1().ledgerHeader.header.ledgerSeq;
            }

            if (ledgerSeq == targetSeq)
            {
                std::string refJsonPath;
                if (isSoroban)
                {
                    refJsonPath = fmt::format(
                        FMT_STRING("testdata/"
                                   "ledger-close-meta-v{}-protocol-{}-"
                                   "soroban.json"),
                        lcm.v(), cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION);
                }
                else
                {
                    refJsonPath = fmt::format(
                        FMT_STRING("testdata/"
                                   "ledger-close-meta-v{}-protocol-{}.json"),
                        lcm.v(), cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION);
                }
                normalizeMeta(lcm);
                std::string have = xdrToCerealString(lcm, "LedgerCloseMeta");
                if (getenv("GENERATE_TEST_LEDGER_CLOSE_META"))
                {
                    std::ofstream outJson(refJsonPath);
                    outJson.write(have.data(), have.size());
                }
                else
                {
                    // If the format of close meta has changed, you will see
                    // a failure here. If the change is expected run this
                    // test with GENERATE_TEST_LEDGER_CLOSE_META=1 to
                    // generate new close meta files.
                    std::ifstream inJson(refJsonPath);
                    REQUIRE(inJson);
                    std::string expect(std::istreambuf_iterator<char>{inJson},
                                       {});
                    REQUIRE(expect == have);
                }
            }
            maxSeq = ledgerSeq;
        }
        REQUIRE(maxSeq == targetSeq);
    };

    SECTION("stellar classic")
    {
        test(getTestConfig(), false);
    }

    SECTION("soroban")
    {
        Config cfg = getTestConfig();
        if (protocolVersionStartsFrom(
                cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION,
                SOROBAN_PROTOCOL_VERSION))
        {
            test(cfg, true);
        }
    }
}
