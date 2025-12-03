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
#include "ledger/InMemorySorobanState.h"
#include "ledger/LedgerStateSnapshot.h"
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

namespace
{
std::pair<LedgerEntry, LedgerEntry>
createContractDataWithTTL(ContractDataDurability durability, uint32_t ttlLedger,
                          uint32_t valU32 = 0)
{
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
}

// Helper to create a CONTRACT_CODE entry with its associated TTL entry
std::pair<LedgerEntry, LedgerEntry>
createContractCodeWithTTL(uint32_t ttlLedger)
{
    LedgerEntry entry =
        LedgerTestUtils::generateValidLedgerEntryOfType(CONTRACT_CODE);
    entry.lastModifiedLedgerSeq = 1;

    LedgerEntry ttl;
    ttl.data.type(TTL);
    ttl.data.ttl().keyHash = getTTLKey(entry).ttl().keyHash;
    ttl.data.ttl().liveUntilLedgerSeq = ttlLedger;
    ttl.lastModifiedLedgerSeq = entry.lastModifiedLedgerSeq;

    return std::make_pair(entry, ttl);
}
}

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

    int const nbInvariants = 3;
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

            auto snapshot = app->getBucketManager()
                                .getBucketSnapshotManager()
                                .copySearchableLiveBucketListSnapshot();
            // Manually trigger eviction so we can test the invariant directly
            LedgerTxn ltx(app->getLedgerTxnRoot());
            ltx.loadHeader().current().ledgerSeq++;
            auto evictedState =
                app->getBucketManager().resolveBackgroundEvictionScan(snapshot,
                                                                      ltx, {});

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
                        evictedState.deletedKeys, emptyMap, emptyMap),
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
                        evictedState.deletedKeys, emptyMap, emptyMap),
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
                        evictedState.deletedKeys, emptyMap, emptyMap),
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
                        evictedState.deletedKeys, emptyMap, emptyMap),
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
                        evictedState.deletedKeys, emptyMap, emptyMap),
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
                        evictedState.deletedKeys, emptyMap, emptyMap),
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
                            evictedState.deletedKeys, emptyMap, emptyMap),
                        InvariantDoesNotHold);
                }
                else
                {
                    REQUIRE_NOTHROW(
                        app->getInvariantManager().checkOnLedgerCommit(
                            snapshot, hotArchiveSnap,
                            evictedState.archivedEntries,
                            evictedState.deletedKeys, emptyMap, emptyMap));
                }
            }

            SECTION("orphaned TTL")
            {
                // Add orphaned TTL to evicted state
                evictedState.deletedKeys.push_back(getTTLKey(liveTempEntry));
                REQUIRE_THROWS_AS(
                    app->getInvariantManager().checkOnLedgerCommit(
                        snapshot, hotArchiveSnap, evictedState.archivedEntries,
                        evictedState.deletedKeys, emptyMap, emptyMap),
                    InvariantDoesNotHold);
            }

            SECTION("Valid eviction")
            {
                // Valid eviction should always pass
                UnorderedMap<LedgerKey, LedgerEntry> emptyMap;
                REQUIRE_NOTHROW(app->getInvariantManager().checkOnLedgerCommit(
                    snapshot, hotArchiveSnap, evictedState.archivedEntries,
                    evictedState.deletedKeys, emptyMap, emptyMap));
            }
        }
    });
}

TEST_CASE("BucketList state consistency invariant", "[invariant]")
{
    // Disable invariants so we can run them manually
    auto cfg = getTestConfig();
    cfg.INVARIANT_CHECKS = {};

    VirtualClock clock;
    auto app = createTestApplication<BucketTestUtils::BucketTestApplication>(
        clock, cfg);
    BucketTestUtils::LedgerManagerForBucketTests& lm = app->getLedgerManager();

    auto [dataEntry1, dataTTL1] = createContractDataWithTTL(PERSISTENT, 1000);
    auto [dataEntry2, dataTTL2] = createContractDataWithTTL(TEMPORARY, 1000);
    auto [codeEntry1, codeTTL1] = createContractCodeWithTTL(1000);
    auto [codeEntry2, codeTTL2] = createContractCodeWithTTL(1000);

    lm.setNextLedgerEntryBatchForBucketTesting(
        {dataEntry1, dataTTL1, dataEntry2, dataTTL2, codeEntry1, codeTTL1,
         codeEntry2, codeTTL2},
        {}, {});
    closeLedger(*app);

    auto getLedgerState = [&]() {
        auto liveBL = app->getBucketManager()
                          .getBucketSnapshotManager()
                          .copySearchableLiveBucketListSnapshot();
        auto hotArchive = app->getBucketManager()
                              .getBucketSnapshotManager()
                              .copySearchableHotArchiveBucketListSnapshot();
        return std::make_shared<CompleteConstLedgerState>(
            liveBL, hotArchive, lm.getLastClosedLedgerHeader(),
            lm.getLastClosedLedgerHAS());
    };

    app->getInvariantManager().enableInvariant("BucketListStateConsistency");

    SECTION("Valid state passes invariant")
    {
        REQUIRE_NOTHROW(app->getInvariantManager().runStateSnapshotInvariant(
            getLedgerState(), lm.getInMemorySorobanStateForTesting()));
    }

    auto testLiveEntryNotInCache = [&](bool isContractCode) {
        InMemorySorobanState modifiedState =
            lm.getInMemorySorobanStateForTesting();

        if (isContractCode)
        {
            modifiedState.mContractCodeEntries.erase(
                modifiedState.mContractCodeEntries.begin());
        }
        else
        {
            modifiedState.mContractDataEntries.erase(
                modifiedState.mContractDataEntries.begin());
        }

        REQUIRE_THROWS_AS(app->getInvariantManager().runStateSnapshotInvariant(
                              getLedgerState(), modifiedState),
                          InvariantDoesNotHold);
    };

    SECTION("Live CONTRACT_DATA entry not in cache")
    {
        testLiveEntryNotInCache(false);
    }

    SECTION("Live CONTRACT_CODE entry not in cache")
    {
        testLiveEntryNotInCache(true);
    }

    auto testEntryWrongValue = [&](bool isContractCode) {
        InMemorySorobanState modifiedState =
            lm.getInMemorySorobanStateForTesting();

        // Get entry, modify it, and replace in the appropriate map
        if (isContractCode)
        {
            auto it = modifiedState.mContractCodeEntries.begin();
            auto keyHash = it->first;
            auto const& codeEntry = it->second;
            LedgerEntry modifiedEntry = *codeEntry.ledgerEntry;
            modifiedEntry.lastModifiedLedgerSeq += 100;
            auto ttlData = codeEntry.ttlData;
            auto sizeBytes = codeEntry.sizeBytes;
            modifiedState.mContractCodeEntries.erase(it);
            modifiedState.mContractCodeEntries.emplace(
                keyHash, ContractCodeMapEntryT(
                             std::make_shared<LedgerEntry const>(modifiedEntry),
                             ttlData, sizeBytes));
        }
        else
        {
            auto it = modifiedState.mContractDataEntries.begin();
            auto const& entryData = it->get();
            LedgerEntry modifiedEntry = *entryData.ledgerEntry;
            modifiedEntry.lastModifiedLedgerSeq += 100;
            auto ttlData = entryData.ttlData;
            modifiedState.mContractDataEntries.erase(it);
            modifiedState.mContractDataEntries.emplace(
                InternalContractDataMapEntry(modifiedEntry, ttlData));
        }

        REQUIRE_THROWS_AS(app->getInvariantManager().runStateSnapshotInvariant(
                              getLedgerState(), modifiedState),
                          InvariantDoesNotHold);
    };

    SECTION("CONTRACT_DATA entry in cache has wrong value")
    {
        testEntryWrongValue(false);
    }

    SECTION("CONTRACT_CODE entry in cache has wrong value")
    {
        testEntryWrongValue(true);
    }

    auto testExtraEntryInCache = [&](bool isContractCode) {
        InMemorySorobanState modifiedState =
            lm.getInMemorySorobanStateForTesting();

        // Create a new entry that doesn't exist in the BL and add it to the
        // cache
        if (isContractCode)
        {
            auto [extraEntry, extraTTL] = createContractCodeWithTTL(1000);
            auto ttlKey = getTTLKey(extraEntry);
            TTLData ttlData(extraTTL.data.ttl().liveUntilLedgerSeq, 1);
            modifiedState.mContractCodeEntries.emplace(
                ttlKey.ttl().keyHash,
                ContractCodeMapEntryT(
                    std::make_shared<LedgerEntry const>(extraEntry), ttlData,
                    100));
        }
        else
        {
            auto [extraEntry, extraTTL] =
                createContractDataWithTTL(PERSISTENT, 1000);
            TTLData ttlData(extraTTL.data.ttl().liveUntilLedgerSeq, 1);
            modifiedState.mContractDataEntries.emplace(
                InternalContractDataMapEntry(extraEntry, ttlData));
        }

        REQUIRE_THROWS_AS(app->getInvariantManager().runStateSnapshotInvariant(
                              getLedgerState(), modifiedState),
                          InvariantDoesNotHold);
    };

    SECTION("Extra CONTRACT_DATA entry in cache not in BL")
    {
        testExtraEntryInCache(false);
    }

    SECTION("Extra CONTRACT_CODE entry in cache not in BL")
    {
        testExtraEntryInCache(true);
    }

    SECTION("TTL mismatch between cache and BL")
    {
        InMemorySorobanState modifiedState =
            lm.getInMemorySorobanStateForTesting();

        // Corrupt TTL of an entry in the cache
        auto it = modifiedState.mContractDataEntries.begin();
        auto const& entryData = it->get();
        LedgerEntry entryCopy = *entryData.ledgerEntry;

        TTLData wrongTTL(42, 1);
        modifiedState.mContractDataEntries.erase(it);
        modifiedState.mContractDataEntries.emplace(
            InternalContractDataMapEntry(entryCopy, wrongTTL));

        REQUIRE_THROWS_AS(app->getInvariantManager().runStateSnapshotInvariant(
                              getLedgerState(), modifiedState),
                          InvariantDoesNotHold);
    }

    SECTION("Orphan TTL in BL without Soroban entry")
    {
        // Add an orphan TTL directly to the BucketList without going
        // through the normal ledger path to avoid hitting cache asserts
        auto [phantomEntry, phantomTTL] =
            createContractDataWithTTL(PERSISTENT, 1000);

        BucketTestUtils::addLiveBatchAndUpdateSnapshot(
            *app, lm.getLastClosedLedgerHeader().header, {phantomTTL}, {}, {});

        REQUIRE_THROWS_AS(
            app->getInvariantManager().runStateSnapshotInvariant(
                getLedgerState(), lm.getInMemorySorobanStateForTesting()),
            InvariantDoesNotHold);
    }

    SECTION("Live entry also in hot archive")
    {
        // Add one of the live entries to the hot archive - this violates
        // the invariant that no entry can be live in both BucketLists
        BucketTestUtils::addHotArchiveBatchAndUpdateSnapshot(
            *app, lm.getLastClosedLedgerHeader().header, {dataEntry1}, {});

        REQUIRE_THROWS_AS(
            app->getInvariantManager().runStateSnapshotInvariant(
                getLedgerState(), lm.getInMemorySorobanStateForTesting()),
            InvariantDoesNotHold);
    }
}
