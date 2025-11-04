// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"

#include "bucket/BucketManager.h"
#include "bucket/BucketSnapshotManager.h"
#include "bucket/BucketUtils.h"
#include "bucket/LiveBucket.h"
#include "bucket/test/BucketTestUtils.h"
#include "database/Database.h"
#include "herder/TxSetFrame.h"
#include "invariant/Invariant.h"
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTypeUtils.h"
#include "ledger/NetworkConfig.h"
#include "ledger/test/LedgerTestUtils.h"
#include "main/Application.h"
#include "test/Catch2.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/ProtocolVersion.h"
#include "xdr/Stellar-contract.h"
#include "xdr/Stellar-ledger-entries.h"

#include <algorithm>
#include <fmt/format.h>

using namespace stellar;

namespace InvariantTests
{

class TestInvariant : public Invariant
{
  public:
    TestInvariant(int id, bool shouldFail)
        : Invariant(true), mInvariantID(id), mShouldFail(shouldFail)
    {
    }

    // if id < 0, generate prefix that will match any invariant
    static std::string
    toString(int id, bool fail)
    {
        if (id < 0)
        {
            return fmt::format("TestInvariant{}", fail ? "Fail" : "Succeed");
        }
        else
        {
            return fmt::format("TestInvariant{}{}", fail ? "Fail" : "Succeed",
                               id);
        }
    }

    virtual std::string
    getName() const override
    {
        return toString(mInvariantID, mShouldFail);
    }

    virtual std::string
    checkOnBucketApply(
        std::shared_ptr<LiveBucket const> bucket, uint32_t oldestLedger,
        uint32_t newestLedger,
        std::unordered_set<LedgerKey> const& shadowedKeys) override
    {
        return mShouldFail ? "fail" : "";
    }

    virtual std::string
    checkAfterAssumeState(uint32_t newestLedger) override
    {
        return mShouldFail ? "fail" : "";
    }

    virtual std::string
    checkOnOperationApply(Operation const& operation,
                          OperationResult const& result,
                          LedgerTxnDelta const& ltxDelta,
                          std::vector<ContractEvent> const& events,
                          AppConnector& app) override
    {
        return mShouldFail ? "fail" : "";
    }

  private:
    int mInvariantID;
    bool mShouldFail;
};
}

using namespace InvariantTests;

TEST_CASE("no duplicate register", "[invariant]")
{
    VirtualClock clock;
    Config cfg = getTestConfig();
    cfg.INVARIANT_CHECKS = {};
    Application::pointer app = createTestApplication(clock, cfg);

    app->getInvariantManager().registerInvariant<TestInvariant>(0, true);
    REQUIRE_THROWS_AS(
        app->getInvariantManager().registerInvariant<TestInvariant>(0, true),
        std::runtime_error);
}

TEST_CASE("no duplicate enable", "[invariant]")
{
    VirtualClock clock;
    Config cfg = getTestConfig();
    cfg.INVARIANT_CHECKS = {};
    Application::pointer app = createTestApplication(clock, cfg);

    app->getInvariantManager().registerInvariant<TestInvariant>(0, true);
    app->getInvariantManager().enableInvariant(
        TestInvariant::toString(0, true));
    REQUIRE_THROWS_AS(app->getInvariantManager().enableInvariant(
                          TestInvariant::toString(0, true)),
                      std::runtime_error);
}

TEST_CASE("only enable registered invariants", "[invariant]")
{
    VirtualClock clock;
    Config cfg = getTestConfig();
    cfg.INVARIANT_CHECKS = {};
    Application::pointer app = createTestApplication(clock, cfg);

    app->getInvariantManager().registerInvariant<TestInvariant>(0, true);
    app->getInvariantManager().enableInvariant(
        TestInvariant::toString(0, true));
    REQUIRE_THROWS_AS(app->getInvariantManager().enableInvariant("WrongName"),
                      std::runtime_error);
}

TEST_CASE("enable registered invariants regex", "[invariant]")
{
    VirtualClock clock;
    Config cfg = getTestConfig();
    cfg.INVARIANT_CHECKS = {};
    Application::pointer app = createTestApplication(clock, cfg);

    const int nbInvariants = 3;
    for (int i = 0; i < nbInvariants; i++)
    {
        app->getInvariantManager().registerInvariant<TestInvariant>(i, true);
    }
    app->getInvariantManager().registerInvariant<TestInvariant>(nbInvariants,
                                                                false);

    app->getInvariantManager().enableInvariant(
        TestInvariant::toString(-1, true) + ".*");
    auto e = app->getInvariantManager().getEnabledInvariants();
    std::sort(e.begin(), e.end());

    REQUIRE(e.size() == nbInvariants);
    for (int i = 0; i < nbInvariants; i++)
    {
        REQUIRE(e[i] == TestInvariant::toString(i, true));
    }
}

TEST_CASE("onBucketApply fail succeed", "[invariant]")
{
    {
        VirtualClock clock;
        Config cfg = getTestConfig();
        cfg.INVARIANT_CHECKS = {};
        Application::pointer app = createTestApplication(clock, cfg);

        app->getInvariantManager().registerInvariant<TestInvariant>(0, true);
        app->getInvariantManager().enableInvariant(
            TestInvariant::toString(0, true));

        auto bucket = std::make_shared<LiveBucket>();
        uint32_t ledger = 1;
        uint32_t level = 0;
        bool isCurr = true;
        REQUIRE_THROWS_AS(app->getInvariantManager().checkOnBucketApply(
                              bucket, ledger, level, isCurr, {}),
                          InvariantDoesNotHold);
    }

    {
        VirtualClock clock;
        Config cfg = getTestConfig();
        cfg.INVARIANT_CHECKS = {};
        Application::pointer app = createTestApplication(clock, cfg);

        app->getInvariantManager().registerInvariant<TestInvariant>(0, false);
        app->getInvariantManager().enableInvariant(
            TestInvariant::toString(0, false));

        auto bucket = std::make_shared<LiveBucket>();
        uint32_t ledger = 1;
        uint32_t level = 0;
        bool isCurr = true;
        REQUIRE_NOTHROW(app->getInvariantManager().checkOnBucketApply(
            bucket, ledger, level, isCurr, {}));
    }
}

TEST_CASE("onOperationApply fail succeed", "[invariant]")
{
    VirtualClock clock;
    Config cfg = getTestConfig();
    Application::pointer app = createTestApplication(clock, cfg);

    OperationResult res;
    SECTION("Fail")
    {
        app->getInvariantManager().registerInvariant<TestInvariant>(0, true);
        app->getInvariantManager().enableInvariant(
            TestInvariant::toString(0, true));

        LedgerTxn ltx(app->getLedgerTxnRoot());
        REQUIRE_THROWS_AS(
            app->getInvariantManager().checkOnOperationApply(
                {}, res, ltx.getDelta(), {}, app->getAppConnector()),
            InvariantDoesNotHold);
    }
    SECTION("Succeed")
    {
        app->getInvariantManager().registerInvariant<TestInvariant>(0, false);
        app->getInvariantManager().enableInvariant(
            TestInvariant::toString(0, false));

        LedgerTxn ltx(app->getLedgerTxnRoot());
        REQUIRE_NOTHROW(app->getInvariantManager().checkOnOperationApply(
            {}, res, ltx.getDelta(), {}, app->getAppConnector()));
    }
}

TEST_CASE_VERSIONS("EventsAreConsistentWithEntryDiffs invariant", "[invariant]")
{
    auto invariantTest = [](bool enableInvariant) {
        VirtualClock clock;
        auto cfg = getTestConfig(0);
        if (enableInvariant)
        {
            cfg.INVARIANT_CHECKS = {"EventsAreConsistentWithEntryDiffs"};
            cfg.EMIT_CLASSIC_EVENTS = true;
            cfg.BACKFILL_STELLAR_ASSET_EVENTS = true;
        }
        else
        {
            cfg.INVARIANT_CHECKS = {};
        }

        auto app = createTestApplication(clock, cfg);

        // set up world
        auto root = app->getRoot();
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto ltxe = loadAccount(ltx, root->getPublicKey());
        REQUIRE(ltxe.current().data.type() == ACCOUNT);
        ltxe.current().data.account().balance -= 1;

        OperationResult res;
        if (enableInvariant)
        {
            REQUIRE_THROWS_AS(
                app->getInvariantManager().checkOnOperationApply(
                    {}, res, ltx.getDelta(), {}, app->getAppConnector()),
                InvariantDoesNotHold);
        }
        else
        {
            REQUIRE_NOTHROW(app->getInvariantManager().checkOnOperationApply(
                {}, res, ltx.getDelta(), {}, app->getAppConnector()));
        }
    };

    invariantTest(true);
    invariantTest(false);
}

TEST_CASE_VERSIONS("State archival eviction invariant", "[invariant][archival]")
{
    // Make sure we disable the archival invariant since we will be invoking it
    // manually
    auto cfg = getTestConfig();
    cfg.INVARIANT_CHECKS = {};

    // Scan all entries in a single pass
    cfg.OVERRIDE_EVICTION_PARAMS_FOR_TESTING = true;
    cfg.TESTING_STARTING_EVICTION_SCAN_LEVEL = 1;
    cfg.TESTING_MAX_ENTRIES_TO_ARCHIVE = 100;
    cfg.TESTING_EVICTION_SCAN_SIZE = 1'000'000;

    VirtualClock clock;
    auto app = createTestApplication<BucketTestUtils::BucketTestApplication>(
        clock, cfg);

    for_versions_from(23, *app, [&] {
        BucketTestUtils::LedgerManagerForBucketTests& lm =
            app->getLedgerManager();
        uint32_t const evictionLedger = 16;

        auto createContractDataWithTTL = [&](ContractDataDurability durability,
                                             uint32_t ttlLedger,
                                             uint32_t valU32 = 0) {
            LedgerEntry entry =
                LedgerTestUtils::generateValidLedgerEntryOfType(CONTRACT_DATA);
            entry.data.contractData().durability = durability;
            entry.lastModifiedLedgerSeq = 1;
            entry.data.contractData().val.type(SCV_U32);
            entry.data.contractData().val.u32() = valU32;

            LedgerEntry ttl;
            ttl.data.type(TTL);
            ttl.data.ttl().keyHash = getTTLKey(entry).ttl().keyHash;
            ttl.data.ttl().liveUntilLedgerSeq = ttlLedger;
            ttl.lastModifiedLedgerSeq = entry.lastModifiedLedgerSeq;

            return std::make_pair(entry, ttl);
        };

        // Create test entries that will be evicted
        auto [tempEntry, tempTTL] =
            createContractDataWithTTL(TEMPORARY, evictionLedger, 1);
        auto [persistentEntry, persistentTTL] =
            createContractDataWithTTL(PERSISTENT, evictionLedger, 2);

        // Create test entries that will remain live
        auto [liveTempEntry, liveTempTTL] =
            createContractDataWithTTL(TEMPORARY, evictionLedger + 100);
        auto [livePersistentEntry, livePersistentTTL] =
            createContractDataWithTTL(PERSISTENT, evictionLedger + 100);

        lm.setNextLedgerEntryBatchForBucketTesting(
            {tempEntry, tempTTL, persistentEntry, persistentTTL, liveTempEntry,
             liveTempTTL, livePersistentEntry, livePersistentTTL},
            {}, {});
        closeLedger(*app);

        for (uint32_t ledgerSeq = lm.getLastClosedLedgerNum() + 1;
             ledgerSeq <= evictionLedger; ++ledgerSeq)
        {
            closeLedger(*app);
        }

        // Make sure the entries have not been evicted
        auto liveBL = app->getBucketManager()
                          .getBucketSnapshotManager()
                          .copySearchableLiveBucketListSnapshot();
        auto hotArchive = app->getBucketManager()
                              .getBucketSnapshotManager()
                              .copySearchableHotArchiveBucketListSnapshot();
        REQUIRE(liveBL->load(LedgerEntryKey(tempEntry)));
        REQUIRE(liveBL->load(LedgerEntryKey(persistentEntry)));
        REQUIRE(liveBL->load(getTTLKey(tempEntry)));
        REQUIRE(liveBL->load(getTTLKey(persistentEntry)));
        REQUIRE(!hotArchive->load(LedgerEntryKey(tempEntry)));
        REQUIRE(!hotArchive->load(LedgerEntryKey(persistentEntry)));

        SECTION("Entries properly evicted")
        {
            closeLedger(*app);

            liveBL = app->getBucketManager()
                         .getBucketSnapshotManager()
                         .copySearchableLiveBucketListSnapshot();
            hotArchive = app->getBucketManager()
                             .getBucketSnapshotManager()
                             .copySearchableHotArchiveBucketListSnapshot();
            REQUIRE(!liveBL->load(LedgerEntryKey(tempEntry)));
            REQUIRE(!liveBL->load(LedgerEntryKey(persistentEntry)));
            REQUIRE(!liveBL->load(getTTLKey(tempEntry)));
            REQUIRE(!liveBL->load(getTTLKey(persistentEntry)));
            REQUIRE(!hotArchive->load(LedgerEntryKey(tempEntry)));
            REQUIRE(hotArchive->load(LedgerEntryKey(persistentEntry)));
        }

        SECTION("invariant check")
        {
            app->getInvariantManager().enableInvariant(
                "ArchivedStateConsistency");

            auto ledgerVersion =
                lm.getLastClosedLedgerHeader().header.ledgerVersion;

            // Manually trigger eviction so we can test the invariant directly
            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto evictedState =
                app->getBucketManager().resolveBackgroundEvictionScan(
                    ltx, lm.getLastClosedLedgerNum() + 1, {}, ledgerVersion,
                    lm.getLastClosedSorobanNetworkConfig());

            auto snapshot = app->getBucketManager()
                                .getBucketSnapshotManager()
                                .copySearchableLiveBucketListSnapshot();
            auto hotArchiveSnap =
                app->getBucketManager()
                    .getBucketSnapshotManager()
                    .copySearchableHotArchiveBucketListSnapshot();
            // Persistent entry
            REQUIRE(evictedState.archivedEntries.size() == 1);
            // Temp entry, temp TTL, persistent TTL
            REQUIRE(evictedState.deletedKeys.size() == 3);

            UnorderedMap<LedgerKey, LedgerEntry> emptyMap;

            auto populateCommitState = [&](LedgerCommitState& commitState) {
                commitState.persistentEvictedFromLive =
                    evictedState.archivedEntries;
                commitState.tempAndTTLEvictedFromLive =
                    evictedState.deletedKeys;
                commitState.restoredFromArchive = emptyMap;
                commitState.restoredFromLiveState = emptyMap;
            };

            SECTION("temp entry does not exist")
            {
                // Add random temp entry key to evicted state deleted keys
                LedgerEntry fakeTempEntry =
                    LedgerTestUtils::generateValidLedgerEntryOfType(
                        CONTRACT_DATA);
                fakeTempEntry.data.contractData().durability = TEMPORARY;
                auto fakeTempKey = LedgerEntryKey(fakeTempEntry);
                auto fakeTTLKey = getTTLKey(fakeTempEntry);

                evictedState.deletedKeys.push_back(fakeTempKey);
                evictedState.deletedKeys.push_back(fakeTTLKey);

                LedgerCommitState commitState;
                populateCommitState(commitState);
                REQUIRE_THROWS_AS(
                    app->getInvariantManager().checkOnLedgerCommit(
                        snapshot, hotArchiveSnap, commitState,
                        lm.getInMemorySorobanStateForTesting()),
                    InvariantDoesNotHold);
            }

            SECTION("temp entry TTL not evicted")
            {
                // Remove the temp entry TTL from the evicted state deleted keys
                auto tempTTLKey = getTTLKey(tempEntry);
                auto it = std::find(evictedState.deletedKeys.begin(),
                                    evictedState.deletedKeys.end(), tempTTLKey);
                if (it != evictedState.deletedKeys.end())
                {
                    evictedState.deletedKeys.erase(it);
                }

                LedgerCommitState commitState;
                populateCommitState(commitState);
                REQUIRE_THROWS_AS(
                    app->getInvariantManager().checkOnLedgerCommit(
                        snapshot, hotArchiveSnap, commitState,
                        lm.getInMemorySorobanStateForTesting()),
                    InvariantDoesNotHold);
            }

            SECTION("temp entry TTL not expired")
            {
                // Add live data key and TTL to evicted state (they shouldn't be
                // evicted since TTL is not expired)
                evictedState.deletedKeys.push_back(
                    LedgerEntryKey(liveTempEntry));
                evictedState.deletedKeys.push_back(getTTLKey(liveTempEntry));

                LedgerCommitState commitState;
                populateCommitState(commitState);
                REQUIRE_THROWS_AS(
                    app->getInvariantManager().checkOnLedgerCommit(
                        snapshot, hotArchiveSnap, commitState,
                        lm.getInMemorySorobanStateForTesting()),
                    InvariantDoesNotHold);
            }

            SECTION("persistent entry TTL not evicted")
            {
                // Remove the persistent entry TTL from the evicted state
                // deleted keys
                auto persistentTTLKey = getTTLKey(persistentEntry);
                auto it =
                    std::find(evictedState.deletedKeys.begin(),
                              evictedState.deletedKeys.end(), persistentTTLKey);
                if (it != evictedState.deletedKeys.end())
                {
                    evictedState.deletedKeys.erase(it);
                }

                LedgerCommitState commitState;
                populateCommitState(commitState);
                REQUIRE_THROWS_AS(
                    app->getInvariantManager().checkOnLedgerCommit(
                        snapshot, hotArchiveSnap, commitState,
                        lm.getInMemorySorobanStateForTesting()),
                    InvariantDoesNotHold);
            }

            SECTION("persistent entry TTL not expired")
            {
                // Add live data key and TTL to evicted state (they shouldn't be
                // evicted since TTL is not expired)
                evictedState.archivedEntries.push_back(livePersistentEntry);
                evictedState.deletedKeys.push_back(
                    getTTLKey(livePersistentEntry));

                LedgerCommitState commitState;
                populateCommitState(commitState);
                REQUIRE_THROWS_AS(
                    app->getInvariantManager().checkOnLedgerCommit(
                        snapshot, hotArchiveSnap, commitState,
                        lm.getInMemorySorobanStateForTesting()),
                    InvariantDoesNotHold);
            }

            SECTION("persistent entry does not exist")
            {
                // Add random persistent entry key to evicted state archived
                // entries
                LedgerEntry fakePersistentEntry =
                    LedgerTestUtils::generateValidLedgerEntryOfType(
                        CONTRACT_DATA);
                fakePersistentEntry.data.contractData().durability = PERSISTENT;
                auto fakeTTLKey = getTTLKey(fakePersistentEntry);

                evictedState.archivedEntries.push_back(fakePersistentEntry);
                evictedState.deletedKeys.push_back(fakeTTLKey);

                LedgerCommitState commitState;
                populateCommitState(commitState);
                REQUIRE_THROWS_AS(
                    app->getInvariantManager().checkOnLedgerCommit(
                        snapshot, hotArchiveSnap, commitState,
                        lm.getInMemorySorobanStateForTesting()),
                    InvariantDoesNotHold);
            }

            SECTION("outdated entry evicted")
            {
                // Manually modify the value of the persistent entry in the
                // evicted state
                evictedState.archivedEntries[0].data.contractData().val.u32() =
                    42;

                // Protocol 23 exhibited this bug, so the assert only applies to
                // p24+
                LedgerCommitState commitState;
                populateCommitState(commitState);
                if (protocolVersionStartsFrom(ledgerVersion,
                                              ProtocolVersion::V_24))
                {
                    REQUIRE_THROWS_AS(
                        app->getInvariantManager().checkOnLedgerCommit(
                            snapshot, hotArchiveSnap, commitState,
                            lm.getInMemorySorobanStateForTesting()),
                        InvariantDoesNotHold);
                }
                else
                {
                    REQUIRE_NOTHROW(
                        app->getInvariantManager().checkOnLedgerCommit(
                            snapshot, hotArchiveSnap, commitState,
                            lm.getInMemorySorobanStateForTesting()));
                }
            }

            SECTION("orphaned TTL")
            {
                // Add orphaned TTL to evicted state
                evictedState.deletedKeys.push_back(getTTLKey(liveTempEntry));
                LedgerCommitState commitState;
                populateCommitState(commitState);
                REQUIRE_THROWS_AS(
                    app->getInvariantManager().checkOnLedgerCommit(
                        snapshot, hotArchiveSnap, commitState,
                        lm.getInMemorySorobanStateForTesting()),
                    InvariantDoesNotHold);
            }

            SECTION("Valid eviction")
            {
                // Valid eviction should always pass
                LedgerCommitState commitState;
                populateCommitState(commitState);
                REQUIRE_NOTHROW(app->getInvariantManager().checkOnLedgerCommit(
                    snapshot, hotArchiveSnap, commitState,
                    lm.getInMemorySorobanStateForTesting()));
            }
        }
    });
}

TEST_CASE("InMemorySorobanState matches BucketList", "[invariant][soroban]")
{
    auto cfg = getTestConfig();
    cfg.INVARIANT_CHECKS = {"InMemorySorobanStateMatchesBucketList"};

    VirtualClock clock;
    auto app = createTestApplication<BucketTestUtils::BucketTestApplication>(
        clock, cfg);

    BucketTestUtils::LedgerManagerForBucketTests& lm = app->getLedgerManager();

    // Helper to create contract data with TTL
    auto createEntryWithTTL = [&](uint32_t valU32, bool isCode) {
        LedgerEntry entry;

        if (isCode)
        {
            entry =
                LedgerTestUtils::generateValidLedgerEntryOfType(CONTRACT_CODE);
        }
        else
        {
            entry =
                LedgerTestUtils::generateValidLedgerEntryOfType(CONTRACT_DATA);
            entry.data.contractData().durability = PERSISTENT;
            entry.data.contractData().val.type(SCV_U32);
            entry.data.contractData().val.u32() = valU32;
        }

        entry.lastModifiedLedgerSeq = 2;

        LedgerEntry ttl;
        ttl.data.type(TTL);
        ttl.data.ttl().keyHash = getTTLKey(entry).ttl().keyHash;
        ttl.data.ttl().liveUntilLedgerSeq = 100;
        ttl.lastModifiedLedgerSeq = 2;

        return std::make_pair(entry, ttl);
    };

    // Populate initial test entries
    auto [dataToExtend, dataToExtendTTL] = createEntryWithTTL(10, false);
    auto [oldDataToUpdate, oldDataToUpdateTTL] = createEntryWithTTL(20, false);
    auto [dataToDelete, dataToDeleteTTL] = createEntryWithTTL(30, false);
    auto [dataToAdd, dataToAddTTL] = createEntryWithTTL(40, false);
    auto [codeToExtend, codeToExtendTTL] = createEntryWithTTL(50, true);
    auto [oldCodeToUpdate, oldCodeToUpdateTTL] = createEntryWithTTL(60, true);
    auto [codeToDelete, codeToDeleteTTL] = createEntryWithTTL(70, true);
    auto [codeToAdd, codeToAddTTL] = createEntryWithTTL(80, true);

    std::vector<LedgerEntry> initEntries = {
        dataToExtend,       dataToExtendTTL, oldDataToUpdate,
        oldDataToUpdateTTL, dataToDelete,    dataToDeleteTTL,
        codeToExtend,       codeToExtendTTL, oldCodeToUpdate,
        oldCodeToUpdateTTL, codeToDelete,    codeToDeleteTTL};
    lm.setNextLedgerEntryBatchForBucketTesting(initEntries, {}, {});
    closeLedger(*app);

    // Exercise all possible cache updates.
    // for dataToExtent/codeToExtend: just modify TTL
    // for dataToUpdate/codeToUpdate: modify data
    // for dataToDelete/codeToDelete: delete entry
    // codeToAdd/dataToAdd: will be a new entry created in the test ledger
    auto newDataToExtendTTL = dataToExtendTTL;
    newDataToExtendTTL.data.ttl().liveUntilLedgerSeq = 200;
    newDataToExtendTTL.lastModifiedLedgerSeq = 3;

    auto newDataToUpdate = oldDataToUpdate;
    newDataToUpdate.data.contractData().val.u32() = 25;
    newDataToUpdate.lastModifiedLedgerSeq = 3;
    auto newDataToUpdateTTL = oldDataToUpdateTTL;
    newDataToUpdateTTL.lastModifiedLedgerSeq = 3;

    auto newCodeToExtendTTL = codeToExtendTTL;
    newCodeToExtendTTL.data.ttl().liveUntilLedgerSeq = 200;
    newCodeToExtendTTL.lastModifiedLedgerSeq = 3;

    auto newCodeToUpdate = oldCodeToUpdate;
    newCodeToUpdate.lastModifiedLedgerSeq = 3;
    auto newCodeToUpdateTTL = oldCodeToUpdateTTL;
    newCodeToUpdateTTL.lastModifiedLedgerSeq = 3;

    std::vector<LedgerEntry> liveEntries = {
        newDataToExtendTTL, newDataToUpdate, newDataToUpdateTTL,
        newCodeToExtendTTL, newCodeToUpdate, newCodeToUpdateTTL};
    std::vector<LedgerKey> deadKeys = {
        LedgerEntryKey(dataToDelete), LedgerEntryKey(dataToDeleteTTL),
        LedgerEntryKey(codeToDelete), LedgerEntryKey(codeToDeleteTTL)};
    std::vector<LedgerEntry> initEntriesForAdd = {dataToAdd, dataToAddTTL,
                                                  codeToAdd, codeToAddTTL};

    lm.setNextLedgerEntryBatchForBucketTesting(initEntriesForAdd, liveEntries,
                                               deadKeys);
    closeLedger(*app);

    auto& sorobanState = const_cast<InMemorySorobanState&>(
        lm.getInMemorySorobanStateForTesting());
    auto snapshot = app->getBucketManager()
                        .getBucketSnapshotManager()
                        .copySearchableLiveBucketListSnapshot();
    auto hotArchiveSnap = app->getBucketManager()
                              .getBucketSnapshotManager()
                              .copySearchableHotArchiveBucketListSnapshot();

    LedgerCommitState commitState;
    commitState.initEntries = initEntriesForAdd;
    commitState.liveEntries = liveEntries;
    commitState.deadEntries = deadKeys;

    auto replaceContractData = [&](LedgerEntry const& keyEntry,
                                   LedgerEntry const& newEntry,
                                   TTLData const& ttl) {
        auto it = sorobanState.mContractDataEntries.find(
            InternalContractDataMapEntry(LedgerEntryKey(keyEntry)));
        REQUIRE(it != sorobanState.mContractDataEntries.end());
        sorobanState.mContractDataEntries.erase(it);
        sorobanState.mContractDataEntries.insert(InternalContractDataMapEntry(
            std::make_shared<LedgerEntry const>(newEntry), ttl));
    };

    auto findContractCode = [&](LedgerEntry const& entry) {
        auto ttlKey = getTTLKey(LedgerEntryKey(entry));
        auto it = sorobanState.mContractCodeEntries.find(ttlKey.ttl().keyHash);
        REQUIRE(it != sorobanState.mContractCodeEntries.end());
        return it;
    };

    auto expectFail = [&]() {
        REQUIRE_THROWS_AS(app->getInvariantManager().checkOnLedgerCommit(
                              snapshot, hotArchiveSnap, commitState,
                              lm.getInMemorySorobanStateForTesting()),
                          InvariantDoesNotHold);
    };

    SECTION("invariant passes when cache matches BucketList")
    {
        REQUIRE_NOTHROW(app->getInvariantManager().checkOnLedgerCommit(
            snapshot, hotArchiveSnap, commitState,
            lm.getInMemorySorobanStateForTesting()));
    }

    SECTION("CONTRACT_DATA not updated in cache")
    {
        // Replace an updated data entry with it's original, outdated version in
        // the cache
        replaceContractData(
            oldDataToUpdate, oldDataToUpdate,
            TTLData{oldDataToUpdateTTL.data.ttl().liveUntilLedgerSeq,
                    oldDataToUpdateTTL.lastModifiedLedgerSeq});

        expectFail();
    }

    SECTION("deleted CONTRACT_DATA still in cache")
    {
        // Manually re-add deleted entry to the cache
        sorobanState.mContractDataEntries.insert(InternalContractDataMapEntry(
            std::make_shared<LedgerEntry const>(dataToDelete),
            TTLData{dataToDeleteTTL.data.ttl().liveUntilLedgerSeq,
                    dataToDeleteTTL.lastModifiedLedgerSeq}));

        expectFail();
    }

    SECTION("CONTRACT_DATA TTL updated in cache without BucketList update")
    {
        // Update a TTL value in the cache that was not actually updated
        replaceContractData(oldDataToUpdate, newDataToUpdate,
                            TTLData{999, 3}); // Wrong liveUntilLedgerSeq

        expectFail();
    }

    SECTION("CONTRACT_DATA TTL not updated in cache")
    {
        // Replace an updated TTL data entry with it's original, outdated
        // version in the cache
        replaceContractData(
            oldDataToUpdate, newDataToUpdate,
            TTLData{oldDataToUpdateTTL.data.ttl().liveUntilLedgerSeq,
                    oldDataToUpdateTTL.lastModifiedLedgerSeq});

        expectFail();
    }

    SECTION("CONTRACT_CODE not updated in cache")
    {
        // Replace an updated code entry with it's original, outdated version in
        // the cache
        auto it = findContractCode(newCodeToUpdate);
        it->second.ledgerEntry =
            std::make_shared<LedgerEntry const>(oldCodeToUpdate);

        expectFail();
    }

    SECTION("deleted CONTRACT_CODE still in cache")
    {
        // Manually re-add deleted code entry to the cache
        auto ttlKey = getTTLKey(LedgerEntryKey(codeToDelete));

        // Re-insert the deleted code entry
        sorobanState.mContractCodeEntries.emplace(
            ttlKey.ttl().keyHash,
            ContractCodeMapEntryT(
                std::make_shared<LedgerEntry const>(codeToDelete),
                TTLData{codeToDeleteTTL.data.ttl().liveUntilLedgerSeq,
                        codeToDeleteTTL.lastModifiedLedgerSeq},
                static_cast<uint32_t>(
                    codeToDelete.data.contractCode().code.size())));

        expectFail();
    }

    SECTION("CONTRACT_CODE TTL updated in cache without BucketList update")
    {
        // Update a code TTL value in the cache that was not actually updated
        auto it = findContractCode(oldCodeToUpdate);
        it->second.ttlData.liveUntilLedgerSeq = 999;

        expectFail();
    }

    SECTION("CONTRACT_CODE TTL not updated in cache")
    {
        // Replace an updated TTL code entry with it's original, outdated
        // version in the cache
        auto it = findContractCode(oldCodeToUpdate);
        it->second.ttlData =
            TTLData{oldCodeToUpdateTTL.data.ttl().liveUntilLedgerSeq,
                    oldCodeToUpdateTTL.lastModifiedLedgerSeq};

        expectFail();
    }

    SECTION("added CONTRACT_DATA has different value in cache")
    {
        // Corrupt by modifying the recently added data entry in cache
        auto corruptedDataToAdd = dataToAdd;
        corruptedDataToAdd.data.contractData().val.u32() = 999;

        replaceContractData(dataToAdd, corruptedDataToAdd,
                            TTLData{dataToAddTTL.data.ttl().liveUntilLedgerSeq,
                                    dataToAddTTL.lastModifiedLedgerSeq});

        expectFail();
    }

    SECTION("added CONTRACT_DATA not in cache")
    {
        // Remove the recently added data entry from cache
        auto it = sorobanState.mContractDataEntries.find(
            InternalContractDataMapEntry(LedgerEntryKey(dataToAdd)));
        REQUIRE(it != sorobanState.mContractDataEntries.end());
        sorobanState.mContractDataEntries.erase(it);

        expectFail();
    }

    SECTION("added CONTRACT_CODE has different value in cache")
    {
        // Corrupt by modifying the recently added code entry in cache
        auto it = findContractCode(codeToAdd);
        auto corruptedCodeToAdd = codeToAdd;
        corruptedCodeToAdd.lastModifiedLedgerSeq = 999;
        it->second.ledgerEntry =
            std::make_shared<LedgerEntry const>(corruptedCodeToAdd);

        expectFail();
    }

    SECTION("added CONTRACT_CODE not in cache")
    {
        // Remove the added recently code entry from cache
        auto ttlKey = getTTLKey(LedgerEntryKey(codeToAdd));
        auto it = sorobanState.mContractCodeEntries.find(ttlKey.ttl().keyHash);
        REQUIRE(it != sorobanState.mContractCodeEntries.end());
        sorobanState.mContractCodeEntries.erase(it);

        // Should fail since added CONTRACT_CODE is missing from cache
        expectFail();
    }
}
