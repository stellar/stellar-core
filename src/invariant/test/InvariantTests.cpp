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

                REQUIRE_THROWS_AS(
                    app->getInvariantManager().checkOnLedgerCommit(
                        snapshot, hotArchiveSnap, evictedState.archivedEntries,
                        evictedState.deletedKeys, emptyMap, emptyMap, {}, {},
                        {}, lm.getInMemorySorobanStateForTesting()),
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

                REQUIRE_THROWS_AS(
                    app->getInvariantManager().checkOnLedgerCommit(
                        snapshot, hotArchiveSnap, evictedState.archivedEntries,
                        evictedState.deletedKeys, emptyMap, emptyMap, {}, {},
                        {}, lm.getInMemorySorobanStateForTesting()),
                    InvariantDoesNotHold);
            }

            SECTION("temp entry TTL not expired")
            {
                // Add live data key and TTL to evicted state (they shouldn't be
                // evicted since TTL is not expired)
                evictedState.deletedKeys.push_back(
                    LedgerEntryKey(liveTempEntry));
                evictedState.deletedKeys.push_back(getTTLKey(liveTempEntry));

                REQUIRE_THROWS_AS(
                    app->getInvariantManager().checkOnLedgerCommit(
                        snapshot, hotArchiveSnap, evictedState.archivedEntries,
                        evictedState.deletedKeys, emptyMap, emptyMap, {}, {},
                        {}, lm.getInMemorySorobanStateForTesting()),
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

                REQUIRE_THROWS_AS(
                    app->getInvariantManager().checkOnLedgerCommit(
                        snapshot, hotArchiveSnap, evictedState.archivedEntries,
                        evictedState.deletedKeys, emptyMap, emptyMap, {}, {},
                        {}, lm.getInMemorySorobanStateForTesting()),
                    InvariantDoesNotHold);
            }

            SECTION("persistent entry TTL not expired")
            {
                // Add live data key and TTL to evicted state (they shouldn't be
                // evicted since TTL is not expired)
                evictedState.archivedEntries.push_back(livePersistentEntry);
                evictedState.deletedKeys.push_back(
                    getTTLKey(livePersistentEntry));

                REQUIRE_THROWS_AS(
                    app->getInvariantManager().checkOnLedgerCommit(
                        snapshot, hotArchiveSnap, evictedState.archivedEntries,
                        evictedState.deletedKeys, emptyMap, emptyMap, {}, {},
                        {}, lm.getInMemorySorobanStateForTesting()),
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

                REQUIRE_THROWS_AS(
                    app->getInvariantManager().checkOnLedgerCommit(
                        snapshot, hotArchiveSnap, evictedState.archivedEntries,
                        evictedState.deletedKeys, emptyMap, emptyMap, {}, {},
                        {}, lm.getInMemorySorobanStateForTesting()),
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
                if (protocolVersionStartsFrom(ledgerVersion,
                                              ProtocolVersion::V_24))
                {
                    REQUIRE_THROWS_AS(
                        app->getInvariantManager().checkOnLedgerCommit(
                            snapshot, hotArchiveSnap,
                            evictedState.archivedEntries,
                            evictedState.deletedKeys, emptyMap, emptyMap, {},
                            {}, {}, lm.getInMemorySorobanStateForTesting()),
                        InvariantDoesNotHold);
                }
                else
                {
                    REQUIRE_NOTHROW(
                        app->getInvariantManager().checkOnLedgerCommit(
                            snapshot, hotArchiveSnap,
                            evictedState.archivedEntries,
                            evictedState.deletedKeys, emptyMap, emptyMap, {},
                            {}, {}, lm.getInMemorySorobanStateForTesting()));
                }
            }

            SECTION("orphaned TTL")
            {
                // Add orphaned TTL to evicted state
                evictedState.deletedKeys.push_back(getTTLKey(liveTempEntry));
                REQUIRE_THROWS_AS(
                    app->getInvariantManager().checkOnLedgerCommit(
                        snapshot, hotArchiveSnap, evictedState.archivedEntries,
                        evictedState.deletedKeys, emptyMap, emptyMap, {}, {},
                        {}, lm.getInMemorySorobanStateForTesting()),
                    InvariantDoesNotHold);
            }

            SECTION("Valid eviction")
            {
                // Valid eviction should always pass
                UnorderedMap<LedgerKey, LedgerEntry> emptyMap;
                REQUIRE_NOTHROW(app->getInvariantManager().checkOnLedgerCommit(
                    snapshot, hotArchiveSnap, evictedState.archivedEntries,
                    evictedState.deletedKeys, emptyMap, emptyMap, {}, {}, {},
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
    auto [dataToUpdate, dataToUpdateTTL] = createEntryWithTTL(20, false);
    auto [dataToDelete, dataToDeleteTTL] = createEntryWithTTL(30, false);
    auto [codeToExtend, codeToExtendTTL] = createEntryWithTTL(50, true);
    auto [codeToUpdate, codeToUpdateTTL] = createEntryWithTTL(60, true);
    auto [codeToDelete, codeToDeleteTTL] = createEntryWithTTL(70, true);

    std::vector<LedgerEntry> initEntries = {
        dataToExtend, dataToExtendTTL, dataToUpdate, dataToUpdateTTL,
        dataToDelete, dataToDeleteTTL, codeToExtend, codeToExtendTTL,
        codeToUpdate, codeToUpdateTTL, codeToDelete, codeToDeleteTTL};
    lm.setNextLedgerEntryBatchForBucketTesting(initEntries, {}, {});
    closeLedger(*app);

    // Exercise all possible cache updates.
    // for dataToExtent/codeToExtend: just modify TTL
    // for dataToUpdate/codeToUpdate: modify data
    // for dataToDelete/codeToDelete: delete entry
    auto dataToExtendTTLUpdated = dataToExtendTTL;
    dataToExtendTTLUpdated.data.ttl().liveUntilLedgerSeq = 200;
    dataToExtendTTLUpdated.lastModifiedLedgerSeq = 3;

    auto dataToUpdateUpdated = dataToUpdate;
    dataToUpdateUpdated.data.contractData().val.u32() = 25;
    dataToUpdateUpdated.lastModifiedLedgerSeq = 3;
    auto dataToUpdateTTLUpdated = dataToUpdateTTL;
    dataToUpdateTTLUpdated.lastModifiedLedgerSeq = 3;

    auto codeToExtendTTLUpdated = codeToExtendTTL;
    codeToExtendTTLUpdated.data.ttl().liveUntilLedgerSeq = 200;
    codeToExtendTTLUpdated.lastModifiedLedgerSeq = 3;

    auto codeToUpdateUpdated = codeToUpdate;
    codeToUpdateUpdated.lastModifiedLedgerSeq = 3;
    auto codeToUpdateTTLUpdated = codeToUpdateTTL;
    codeToUpdateTTLUpdated.lastModifiedLedgerSeq = 3;

    std::vector<LedgerEntry> liveEntries = {
        dataToExtendTTLUpdated, dataToUpdateUpdated, dataToUpdateTTLUpdated,
        codeToExtendTTLUpdated, codeToUpdateUpdated, codeToUpdateTTLUpdated};
    std::vector<LedgerKey> deadKeys = {
        LedgerEntryKey(dataToDelete), LedgerEntryKey(dataToDeleteTTL),
        LedgerEntryKey(codeToDelete), LedgerEntryKey(codeToDeleteTTL)};

    lm.setNextLedgerEntryBatchForBucketTesting({}, liveEntries, deadKeys);
    closeLedger(*app);

    // Common variables used by all test sections
    auto& sorobanState = const_cast<InMemorySorobanState&>(
        lm.getInMemorySorobanStateForTesting());
    auto snapshot = app->getBucketManager()
                        .getBucketSnapshotManager()
                        .copySearchableLiveBucketListSnapshot();
    auto hotArchiveSnap = app->getBucketManager()
                              .getBucketSnapshotManager()
                              .copySearchableHotArchiveBucketListSnapshot();

    SECTION("invariant passes when cache matches BucketList")
    {
        // Should pass since cache and BucketList are in sync
        REQUIRE_NOTHROW(app->getInvariantManager().checkOnLedgerCommit(
            snapshot, hotArchiveSnap, {}, {}, {}, {}, {}, liveEntries, deadKeys,
            lm.getInMemorySorobanStateForTesting()));
    }

    SECTION("CONTRACT_DATA not updated in cache")
    {
        // Corrupt dataToUpdate by replacing with original value
        auto it = sorobanState.mContractDataEntries.find(
            InternalContractDataMapEntry(LedgerEntryKey(dataToUpdate)));
        REQUIRE(it != sorobanState.mContractDataEntries.end());

        sorobanState.mContractDataEntries.erase(it);
        // Use original dataToUpdate instead of dataToUpdateUpdated
        sorobanState.mContractDataEntries.insert(InternalContractDataMapEntry(
            std::make_shared<LedgerEntry const>(dataToUpdate),
            TTLData{dataToUpdateTTL.data.ttl().liveUntilLedgerSeq,
                    dataToUpdateTTL.lastModifiedLedgerSeq}));

        // Should fail since cache has stale CONTRACT_DATA entry
        REQUIRE_THROWS_AS(app->getInvariantManager().checkOnLedgerCommit(
                              snapshot, hotArchiveSnap, {}, {}, {}, {}, {},
                              liveEntries, deadKeys,
                              lm.getInMemorySorobanStateForTesting()),
                          InvariantDoesNotHold);
    }

    SECTION("deleted CONTRACT_DATA still in cache")
    {
        // Manually re-add dataToDelete to cache
        sorobanState.mContractDataEntries.insert(InternalContractDataMapEntry(
            std::make_shared<LedgerEntry const>(dataToDelete),
            TTLData{dataToDeleteTTL.data.ttl().liveUntilLedgerSeq,
                    dataToDeleteTTL.lastModifiedLedgerSeq}));

        // Should fail since deleted CONTRACT_DATA is still in cache
        REQUIRE_THROWS_AS(app->getInvariantManager().checkOnLedgerCommit(
                              snapshot, hotArchiveSnap, {}, {}, {}, {}, {},
                              liveEntries, deadKeys,
                              lm.getInMemorySorobanStateForTesting()),
                          InvariantDoesNotHold);
    }

    SECTION("CONTRACT_DATA TTL updated in cache without BucketList update")
    {
        // Corrupt dataToUpdate TTL by using wrong value
        auto it = sorobanState.mContractDataEntries.find(
            InternalContractDataMapEntry(LedgerEntryKey(dataToUpdate)));
        REQUIRE(it != sorobanState.mContractDataEntries.end());

        sorobanState.mContractDataEntries.erase(it);
        sorobanState.mContractDataEntries.insert(InternalContractDataMapEntry(
            std::make_shared<LedgerEntry const>(dataToUpdateUpdated),
            TTLData{999, 3})); // Wrong liveUntilLedgerSeq

        // Should fail since cache has corrupted TTL
        REQUIRE_THROWS_AS(app->getInvariantManager().checkOnLedgerCommit(
                              snapshot, hotArchiveSnap, {}, {}, {}, {}, {},
                              liveEntries, deadKeys,
                              lm.getInMemorySorobanStateForTesting()),
                          InvariantDoesNotHold);
    }

    SECTION("CONTRACT_DATA TTL not updated in cache")
    {
        // Corrupt dataToUpdate TTL by using original value
        auto it = sorobanState.mContractDataEntries.find(
            InternalContractDataMapEntry(LedgerEntryKey(dataToUpdate)));
        REQUIRE(it != sorobanState.mContractDataEntries.end());

        // Use original TTL instead of updated TTL
        sorobanState.mContractDataEntries.erase(it);
        sorobanState.mContractDataEntries.insert(InternalContractDataMapEntry(
            std::make_shared<LedgerEntry const>(dataToUpdateUpdated),
            TTLData{dataToUpdateTTL.data.ttl().liveUntilLedgerSeq,
                    dataToUpdateTTL.lastModifiedLedgerSeq}));

        // Should fail since cache has stale TTL
        REQUIRE_THROWS_AS(app->getInvariantManager().checkOnLedgerCommit(
                              snapshot, hotArchiveSnap, {}, {}, {}, {}, {},
                              liveEntries, deadKeys,
                              lm.getInMemorySorobanStateForTesting()),
                          InvariantDoesNotHold);
    }

    SECTION("CONTRACT_CODE not updated in cache")
    {
        // Corrupt codeToUpdate by replacing with original value
        auto ttlKey = getTTLKey(LedgerEntryKey(codeToUpdate));
        auto it = sorobanState.mContractCodeEntries.find(ttlKey.ttl().keyHash);
        REQUIRE(it != sorobanState.mContractCodeEntries.end());

        // Use original codeToUpdate instead of codeToUpdateUpdated
        it->second.ledgerEntry =
            std::make_shared<LedgerEntry const>(codeToUpdate);

        // Should fail since cache has stale CONTRACT_CODE entry
        REQUIRE_THROWS_AS(app->getInvariantManager().checkOnLedgerCommit(
                              snapshot, hotArchiveSnap, {}, {}, {}, {}, {},
                              liveEntries, deadKeys,
                              lm.getInMemorySorobanStateForTesting()),
                          InvariantDoesNotHold);
    }

    SECTION("deleted CONTRACT_CODE still in cache")
    {
        // Manually re-add codeToDelete to cache (simulating it not being
        // deleted)
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

        // Should fail since deleted CONTRACT_CODE is still in cache
        REQUIRE_THROWS_AS(app->getInvariantManager().checkOnLedgerCommit(
                              snapshot, hotArchiveSnap, {}, {}, {}, {}, {},
                              liveEntries, deadKeys,
                              lm.getInMemorySorobanStateForTesting()),
                          InvariantDoesNotHold);
    }

    SECTION("CONTRACT_CODE TTL updated in cache without BucketList update")
    {
        // Corrupt codeToUpdate TTL by using wrong value
        auto ttlKey = getTTLKey(LedgerEntryKey(codeToUpdate));
        auto it = sorobanState.mContractCodeEntries.find(ttlKey.ttl().keyHash);
        REQUIRE(it != sorobanState.mContractCodeEntries.end());

        // Corrupt by changing TTL to wrong value
        it->second.ttlData.liveUntilLedgerSeq = 999;

        // Should fail since cache has corrupted TTL
        REQUIRE_THROWS_AS(app->getInvariantManager().checkOnLedgerCommit(
                              snapshot, hotArchiveSnap, {}, {}, {}, {}, {},
                              liveEntries, deadKeys,
                              lm.getInMemorySorobanStateForTesting()),
                          InvariantDoesNotHold);
    }

    SECTION("CONTRACT_CODE TTL not updated in cache")
    {
        // Corrupt codeToUpdate TTL by using original value
        auto ttlKey = getTTLKey(LedgerEntryKey(codeToUpdate));
        auto it = sorobanState.mContractCodeEntries.find(ttlKey.ttl().keyHash);
        REQUIRE(it != sorobanState.mContractCodeEntries.end());

        // Use original TTL instead of updated TTL
        it->second.ttlData =
            TTLData{codeToUpdateTTL.data.ttl().liveUntilLedgerSeq,
                    codeToUpdateTTL.lastModifiedLedgerSeq};

        // Should fail since cache has stale TTL
        REQUIRE_THROWS_AS(app->getInvariantManager().checkOnLedgerCommit(
                              snapshot, hotArchiveSnap, {}, {}, {}, {}, {},
                              liveEntries, deadKeys,
                              lm.getInMemorySorobanStateForTesting()),
                          InvariantDoesNotHold);
    }
}
