// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// This file contains tests for the BucketIndex and higher-level operations
// concerning key-value lookup based on the BucketList.

#include "bucket/BucketManager.h"
#include "bucket/BucketSnapshotManager.h"
#include "bucket/LiveBucket.h"
#include "bucket/LiveBucketList.h"
#include "bucket/test/BucketTestUtils.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "test/test.h"

#include "util/UnorderedMap.h"
#include "util/UnorderedSet.h"
#include "util/XDRCereal.h"
#include "util/types.h"

using namespace stellar;
using namespace BucketTestUtils;

namespace BucketManagerTests
{

class BucketIndexTest
{
  protected:
    std::unique_ptr<VirtualClock> mClock;
    std::shared_ptr<BucketTestApplication> mApp;

    // Mapping of Key->value that BucketList should return
    UnorderedMap<LedgerKey, LedgerEntry> mTestEntries;

    // Set of keys to query BucketList for
    LedgerKeySet mKeysToSearch;
    stellar::uniform_int_distribution<uint8_t> mDist;
    uint32_t mLevelsToBuild;

    static void
    validateResults(UnorderedMap<LedgerKey, LedgerEntry> const& validEntries,
                    std::vector<LedgerEntry> const& blEntries)
    {
        REQUIRE(validEntries.size() == blEntries.size());
        for (auto const& entry : blEntries)
        {
            auto iter = validEntries.find(LedgerEntryKey(entry));
            REQUIRE(iter != validEntries.end());
            REQUIRE(iter->second == entry);
        }
    }

    void
    insertEntries(std::vector<LedgerEntry> const& entries)
    {
        mApp->getLedgerManager().setNextLedgerEntryBatchForBucketTesting(
            {}, entries, {});
        closeLedger(*mApp);
    }

    void
    buildBucketList(std::function<void(std::vector<LedgerEntry>&)> f)
    {
        uint32_t ledger = 0;
        do
        {
            ++ledger;
            std::vector<LedgerEntry> entries =
                LedgerTestUtils::generateValidLedgerEntriesWithExclusions(
                    {CONFIG_SETTING}, 10);
            f(entries);
            closeLedger(*mApp);
        } while (!LiveBucketList::levelShouldSpill(ledger, mLevelsToBuild - 1));
    }

  public:
    BucketIndexTest(Config const& cfg, uint32_t levels = 6)
        : mClock(std::make_unique<VirtualClock>())
        , mApp(createTestApplication<BucketTestApplication>(*mClock, cfg))
        , mLevelsToBuild(levels)
    {
    }

    BucketManager&
    getBM() const
    {
        return mApp->getBucketManager();
    }

    virtual void
    buildGeneralTest()
    {
        auto f = [&](std::vector<LedgerEntry> const& entries) {
            // Sample ~4% of entries
            if (mDist(gRandomEngine) < 10)
            {
                for (auto const& e : entries)
                {
                    auto k = LedgerEntryKey(e);
                    mTestEntries.emplace(k, e);
                    mKeysToSearch.emplace(k);
                }
            }
            mApp->getLedgerManager().setNextLedgerEntryBatchForBucketTesting(
                {}, entries, {});
        };

        buildBucketList(f);
    }

    void
    runHistoricalSnapshotTest()
    {
        uint32_t ledger = 0;
        auto canonicalEntry =
            LedgerTestUtils::generateValidLedgerEntryWithExclusions(
                {LedgerEntryType::CONFIG_SETTING});
        canonicalEntry.lastModifiedLedgerSeq = 0;

        do
        {
            ++ledger;
            auto entryCopy = canonicalEntry;
            entryCopy.lastModifiedLedgerSeq = ledger;
            mApp->getLedgerManager().setNextLedgerEntryBatchForBucketTesting(
                {}, {entryCopy}, {});
            closeLedger(*mApp);
        } while (ledger < mApp->getConfig().QUERY_SNAPSHOT_LEDGERS + 2);
        ++ledger;

        auto searchableBL = getBM()
                                .getBucketSnapshotManager()
                                .copySearchableLiveBucketListSnapshot();
        auto lk = LedgerEntryKey(canonicalEntry);

        auto currentLoadedEntry = searchableBL->load(lk);
        REQUIRE(currentLoadedEntry);

        // Note: The definition of "historical snapshot" ledger is that the
        // BucketList snapshot for ledger N is the BucketList as it exists at
        // the beginning of ledger N. This means that the lastModifiedLedgerSeq
        // is at most N - 1.
        REQUIRE(currentLoadedEntry->lastModifiedLedgerSeq == ledger - 1);

        for (uint32_t currLedger = ledger; currLedger > 0; --currLedger)
        {
            auto loadRes = searchableBL->loadKeysFromLedger({lk}, currLedger);

            // If we query an older snapshot, should return <null, notFound>
            if (currLedger < ledger - mApp->getConfig().QUERY_SNAPSHOT_LEDGERS)
            {
                REQUIRE(!loadRes);
            }
            else
            {
                REQUIRE(loadRes);
                REQUIRE(loadRes->size() == 1);
                REQUIRE(loadRes->at(0).lastModifiedLedgerSeq == currLedger - 1);
            }
        }
    }

    virtual void
    buildMultiVersionTest()
    {
        std::vector<LedgerKey> toDestroy;
        std::vector<LedgerEntry> toUpdate;
        auto f = [&](std::vector<LedgerEntry> const& entries) {
            // Actually update/destroy entries for ~4% of ledgers
            if (mDist(gRandomEngine) < 10)
            {
                for (auto& e : toUpdate)
                {
                    e.data.account().balance += 1;
                    auto iter = mTestEntries.find(LedgerEntryKey(e));
                    iter->second = e;
                }

                for (auto const& k : toDestroy)
                {
                    mTestEntries.erase(k);
                }

                mApp->getLedgerManager()
                    .setNextLedgerEntryBatchForBucketTesting({}, toUpdate,
                                                             toDestroy);
                toDestroy.clear();
                toUpdate.clear();
            }
            else
            {
                // Sample ~15% of entries to be destroyed/updated
                if (mDist(gRandomEngine) < 40)
                {
                    for (auto const& e : entries)
                    {
                        mTestEntries.emplace(LedgerEntryKey(e), e);
                        mKeysToSearch.emplace(LedgerEntryKey(e));
                        if (e.data.type() == ACCOUNT)
                        {
                            toUpdate.emplace_back(e);
                        }
                        else
                        {
                            toDestroy.emplace_back(LedgerEntryKey(e));
                        }
                    }
                }

                mApp->getLedgerManager()
                    .setNextLedgerEntryBatchForBucketTesting({}, entries, {});
            }
        };

        buildBucketList(f);
    }

    void
    insertSimilarContractDataKeys()
    {
        auto templateEntry =
            LedgerTestUtils::generateValidLedgerEntryWithTypes({CONTRACT_DATA});

        auto generateEntry = [&](ContractDataDurability t) {
            auto le = templateEntry;
            le.data.contractData().durability = t;
            return le;
        };

        std::vector<LedgerEntry> entries = {
            generateEntry(ContractDataDurability::TEMPORARY),
            generateEntry(ContractDataDurability::PERSISTENT),
        };
        for (auto const& e : entries)
        {
            auto k = LedgerEntryKey(e);
            auto const& [_, inserted] = mTestEntries.emplace(k, e);

            // No key collisions
            REQUIRE(inserted);
            mKeysToSearch.emplace(k);
        }

        insertEntries(entries);
    }

    virtual void
    run()
    {
        auto searchableBL = getBM()
                                .getBucketSnapshotManager()
                                .copySearchableLiveBucketListSnapshot();

        // Test bulk load lookup
        auto loadResult =
            searchableBL->loadKeysWithLimits(mKeysToSearch, nullptr);
        validateResults(mTestEntries, loadResult);

        // Test individual entry lookup
        loadResult.clear();
        for (auto const& key : mKeysToSearch)
        {
            auto entryPtr = searchableBL->load(key);
            if (entryPtr)
            {
                loadResult.emplace_back(*entryPtr);
            }
        }

        validateResults(mTestEntries, loadResult);
    }

    // Do many lookups with subsets of sampled entries
    virtual void
    runPerf(size_t n)
    {
        auto searchableBL = getBM()
                                .getBucketSnapshotManager()
                                .copySearchableLiveBucketListSnapshot();
        for (size_t i = 0; i < n; ++i)
        {
            LedgerKeySet searchSubset;
            UnorderedMap<LedgerKey, LedgerEntry> testEntriesSubset;

            // Not actual size, as there may be duplicated elements, but good
            // enough
            auto subsetSize = 500;
            for (auto j = 0; j < subsetSize; ++j)
            {
                auto iter = mKeysToSearch.begin();
                std::advance(
                    iter, rand_uniform(size_t(400), mKeysToSearch.size() - 1));
                searchSubset.emplace(*iter);
                auto mapIter = mTestEntries.find(*iter);
                testEntriesSubset.emplace(*mapIter);
            }

            if (rand_flip())
            {
                // Add keys not in bucket list as well
                auto addKeys =
                    LedgerTestUtils::generateValidLedgerEntryKeysWithExclusions(
                        {CONFIG_SETTING}, 10);

                searchSubset.insert(addKeys.begin(), addKeys.end());
            }

            auto blLoad =
                searchableBL->loadKeysWithLimits(searchSubset, nullptr);
            validateResults(testEntriesSubset, blLoad);
        }
    }

    void
    testInvalidKeys()
    {
        auto searchableBL = getBM()
                                .getBucketSnapshotManager()
                                .copySearchableLiveBucketListSnapshot();

        // Load should return empty vector for keys not in bucket list
        auto keysNotInBL =
            LedgerTestUtils::generateValidLedgerEntryKeysWithExclusions(
                {CONFIG_SETTING}, 10);
        LedgerKeySet invalidKeys(keysNotInBL.begin(), keysNotInBL.end());

        // Test bulk load
        REQUIRE(searchableBL->loadKeysWithLimits(invalidKeys, nullptr).size() ==
                0);

        // Test individual load
        for (auto const& key : invalidKeys)
        {
            auto entryPtr = searchableBL->load(key);
            REQUIRE(!entryPtr);
        }
    }

    void
    restartWithConfig(Config const& cfg)
    {
        mApp->gracefulStop();
        while (mClock->crank(false))
            ;
        mApp.reset();
        mClock = std::make_unique<VirtualClock>();
        mApp =
            createTestApplication<BucketTestApplication>(*mClock, cfg, false);
    }
};

class BucketIndexPoolShareTest : public BucketIndexTest
{
    AccountEntry mAccountToSearch;
    AccountEntry mAccount2;

    // Liquidity pools with all combinations of the 3 assets will be created,
    // but only mAssetToSearch will be searched
    Asset mAssetToSearch;
    Asset mAsset2;
    Asset mAsset3;

    static LedgerEntry
    generateTrustline(AccountEntry a, LiquidityPoolEntry p)
    {
        LedgerEntry t;
        t.data.type(TRUSTLINE);
        t.data.trustLine().accountID = a.accountID;
        t.data.trustLine().asset.type(ASSET_TYPE_POOL_SHARE);
        t.data.trustLine().asset.liquidityPoolID() = p.liquidityPoolID;
        return t;
    }

    void
    buildTest(bool shouldMultiVersion)
    {
        auto f = [&](std::vector<LedgerEntry>& entries) {
            std::vector<LedgerEntry> poolEntries;
            std::vector<LedgerKey> toWriteNewVersion;
            if (mDist(gRandomEngine) < 30)
            {
                // Make sure we generate a unique poolID for each entry
                LiquidityPoolEntry pool;
                for (;;)
                {
                    pool = LedgerTestUtils::generateValidLiquidityPoolEntry();
                    for (auto e : poolEntries)
                    {
                        if (e.data.liquidityPool().liquidityPoolID ==
                            pool.liquidityPoolID)
                        {
                            continue;
                        }
                    }

                    break;
                }

                auto& params = pool.body.constantProduct().params;

                auto trustlineToSearch =
                    generateTrustline(mAccountToSearch, pool);
                auto trustline2 = generateTrustline(mAccount2, pool);

                // Include target asset
                if (rand_flip())
                {
                    if (rand_flip())
                    {
                        params.assetA = mAssetToSearch;
                        params.assetB = rand_flip() ? mAsset2 : mAsset3;
                    }
                    else
                    {
                        params.assetA = rand_flip() ? mAsset2 : mAsset3;
                        params.assetB = mAssetToSearch;
                    }

                    mTestEntries.emplace(LedgerEntryKey(trustlineToSearch),
                                         trustlineToSearch);
                }
                // Don't include target asset
                else
                {
                    params.assetA = mAsset2;
                    params.assetB = mAsset3;
                }

                LedgerEntry poolEntry;
                poolEntry.data.type(LIQUIDITY_POOL);
                poolEntry.data.liquidityPool() = pool;
                poolEntries.emplace_back(poolEntry);
                entries.emplace_back(trustlineToSearch);
                entries.emplace_back(trustline2);
            }
            // Write new version via delete
            else if (shouldMultiVersion && mDist(gRandomEngine) < 10 &&
                     !mTestEntries.empty())
            {
                // Arbitrarily pcik first entry of map
                auto iter = mTestEntries.begin();
                toWriteNewVersion.emplace_back(iter->first);
                mTestEntries.erase(iter);
            }
            // Write new version via modify
            else if (shouldMultiVersion && mDist(gRandomEngine) < 10 &&
                     !mTestEntries.empty())
            {
                // Arbitrarily pick first entry of map
                auto iter = mTestEntries.begin();
                iter->second.data.trustLine().balance += 10;
                entries.emplace_back(iter->second);
            }

            // We only index liquidity pool INITENTRY, so they must be inserted
            // as INITENTRY
            mApp->getLedgerManager().setNextLedgerEntryBatchForBucketTesting(
                poolEntries, entries, toWriteNewVersion);
        };

        BucketIndexTest::buildBucketList(f);
    }

  public:
    BucketIndexPoolShareTest(Config& cfg, uint32_t levels = 6)
        : BucketIndexTest(cfg, levels)
    {
        mAccountToSearch = LedgerTestUtils::generateValidAccountEntry();
        mAccount2 = LedgerTestUtils::generateValidAccountEntry();

        mAssetToSearch.type(ASSET_TYPE_CREDIT_ALPHANUM4);
        mAsset2.type(ASSET_TYPE_CREDIT_ALPHANUM4);
        mAsset3.type(ASSET_TYPE_CREDIT_ALPHANUM4);
        strToAssetCode(mAssetToSearch.alphaNum4().assetCode, "ast1");
        strToAssetCode(mAsset2.alphaNum4().assetCode, "ast2");
        strToAssetCode(mAsset3.alphaNum4().assetCode, "ast2");
    }

    virtual void
    buildGeneralTest() override
    {
        buildTest(false);
    }

    virtual void
    buildMultiVersionTest() override
    {
        buildTest(true);
    }

    virtual void
    run() override
    {
        auto searchableBL = getBM()
                                .getBucketSnapshotManager()
                                .copySearchableLiveBucketListSnapshot();
        auto loadResult =
            searchableBL->loadPoolShareTrustLinesByAccountAndAsset(
                mAccountToSearch.accountID, mAssetToSearch);
        validateResults(mTestEntries, loadResult);
    }
};

static void
testAllIndexTypes(std::function<void(Config&)> f)
{
    SECTION("individual index only")
    {
        Config cfg(getTestConfig());
        cfg.BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT = 0;
        f(cfg);
    }

    SECTION("individual and range index")
    {
        Config cfg(getTestConfig());

        // First 3 levels individual, last 3 range index
        cfg.BUCKETLIST_DB_INDEX_CUTOFF = 1;
        f(cfg);
    }

    SECTION("range index only")
    {
        Config cfg(getTestConfig());
        cfg.BUCKETLIST_DB_INDEX_CUTOFF = 0;
        f(cfg);
    }
}

TEST_CASE("key-value lookup", "[bucket][bucketindex]")
{
    auto f = [&](Config& cfg) {
        auto test = BucketIndexTest(cfg);
        test.buildGeneralTest();
        test.run();
        test.testInvalidKeys();
    };

    testAllIndexTypes(f);
}

TEST_CASE("do not load outdated values", "[bucket][bucketindex]")
{
    auto f = [&](Config& cfg) {
        auto test = BucketIndexTest(cfg);
        test.buildMultiVersionTest();
        test.run();
    };

    testAllIndexTypes(f);
}

TEST_CASE("load from historical snapshots", "[bucket][bucketindex]")
{
    auto f = [&](Config& cfg) {
        cfg.QUERY_SNAPSHOT_LEDGERS = 5;
        auto test = BucketIndexTest(cfg);
        test.runHistoricalSnapshotTest();
    };

    testAllIndexTypes(f);
}

TEST_CASE("loadPoolShareTrustLinesByAccountAndAsset", "[bucket][bucketindex]")
{
    auto f = [&](Config& cfg) {
        auto test = BucketIndexPoolShareTest(cfg);
        test.buildGeneralTest();
        test.run();
    };

    testAllIndexTypes(f);
}

TEST_CASE(
    "loadPoolShareTrustLinesByAccountAndAsset does not load outdated versions",
    "[bucket][bucketindex]")
{
    auto f = [&](Config& cfg) {
        auto test = BucketIndexPoolShareTest(cfg);
        test.buildMultiVersionTest();
        test.run();
    };

    testAllIndexTypes(f);
}

TEST_CASE("ContractData key with same ScVal", "[bucket][bucketindex]")
{
    auto f = [&](Config& cfg) {
        auto test = BucketIndexTest(cfg, /*levels=*/1);
        test.buildGeneralTest();
        test.insertSimilarContractDataKeys();
        test.run();
    };

    testAllIndexTypes(f);
}

TEST_CASE("serialize bucket indexes", "[bucket][bucketindex]")
{
    Config cfg(getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT));

    // All levels use range config
    cfg.BUCKETLIST_DB_INDEX_CUTOFF = 0;
    cfg.BUCKETLIST_DB_PERSIST_INDEX = true;
    cfg.INVARIANT_CHECKS = {};

    // Node is not a validator, so indexes will persist
    cfg.NODE_IS_VALIDATOR = false;
    cfg.FORCE_SCP = false;

    auto test = BucketIndexTest(cfg, /*levels=*/3);
    test.buildGeneralTest();

    std::set<Hash> liveBuckets;
    auto& liveBL = test.getBM().getLiveBucketList();
    for (auto i = 0; i < LiveBucketList::kNumLevels; ++i)
    {
        auto level = liveBL.getLevel(i);
        for (auto const& b : {level.getCurr(), level.getSnap()})
        {
            liveBuckets.emplace(b->getHash());
        }
    }

    for (auto const& bucketHash : liveBuckets)
    {
        if (isZero(bucketHash))
        {
            continue;
        }

        // Check if index files are saved
        auto indexFilename = test.getBM().bucketIndexFilename(bucketHash);
        REQUIRE(fs::exists(indexFilename));

        auto b = test.getBM().getBucketByHash<LiveBucket>(bucketHash);
        REQUIRE(b->isIndexed());

        auto onDiskIndex =
            loadIndex<LiveBucket>(test.getBM(), indexFilename, b->getSize());
        REQUIRE(onDiskIndex);

        auto& inMemoryIndex = b->getIndexForTesting();
        REQUIRE((inMemoryIndex == *onDiskIndex));
    }

    // Restart app with different config to test that indexes created with
    // different config settings are not loaded from disk. These params will
    // invalidate every index in BL
    cfg.BUCKETLIST_DB_INDEX_CUTOFF = 0;
    cfg.BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT = 10;
    test.restartWithConfig(cfg);

    for (auto const& bucketHash : liveBuckets)
    {
        if (isZero(bucketHash))
        {
            continue;
        }

        // Check if in-memory index has correct params
        auto b = test.getBM().getBucketByHash<LiveBucket>(bucketHash);
        REQUIRE(!b->isEmpty());
        REQUIRE(b->isIndexed());

        auto& inMemoryIndex = b->getIndexForTesting();
        REQUIRE(inMemoryIndex.getPageSize() == (1UL << 10));
        auto inMemoryCoutners = inMemoryIndex.getBucketEntryCounters();
        // Ensure the inMemoryIndex has some non-zero counters.
        REQUIRE(!inMemoryCoutners.entryTypeCounts.empty());
        REQUIRE(!inMemoryCoutners.entryTypeSizes.empty());
        bool allZero = true;
        for (auto const& [k, v] : inMemoryCoutners.entryTypeCounts)
        {
            allZero = allZero && (v == 0);
            allZero = allZero && (inMemoryCoutners.entryTypeSizes.at(k) == 0);
        }
        REQUIRE(!allZero);

        // Check if on-disk index rewritten with correct config params
        auto indexFilename = test.getBM().bucketIndexFilename(bucketHash);
        auto onDiskIndex =
            loadIndex<LiveBucket>(test.getBM(), indexFilename, b->getSize());
        REQUIRE((inMemoryIndex == *onDiskIndex));
    }
}

// The majority of BucketListDB functionality is shared by all bucketlist types.
// This test is a simple sanity check and tests the interface differences
// between the live bucketlist and the hot archive bucketlist.
TEST_CASE("hot archive bucket lookups", "[bucket][bucketindex][archive]")
{
    auto f = [&](Config& cfg) {
        auto clock = VirtualClock();
        auto app = createTestApplication<BucketTestApplication>(clock, cfg);

        UnorderedMap<LedgerKey, LedgerEntry> expectedArchiveEntries;
        UnorderedSet<LedgerKey> expectedDeletedEntries;
        UnorderedSet<LedgerKey> expectedRestoredEntries;
        UnorderedSet<LedgerKey> keysToSearch;

        auto ledger = 1;

        // Use snapshot across ledger to test update behavior
        auto searchableBL = app->getBucketManager()
                                .getBucketSnapshotManager()
                                .copySearchableHotArchiveBucketListSnapshot();

        auto checkLoad =
            [&](LedgerKey const& k,
                std::shared_ptr<HotArchiveBucketEntry const> entryPtr) {
                // Restored entries should be null
                if (expectedRestoredEntries.find(k) !=
                    expectedRestoredEntries.end())
                {
                    REQUIRE(!entryPtr);
                }

                // Deleted entries should be HotArchiveBucketEntry of type
                // DELETED
                else if (expectedDeletedEntries.find(k) !=
                         expectedDeletedEntries.end())
                {
                    REQUIRE(entryPtr);
                    REQUIRE(entryPtr->type() ==
                            HotArchiveBucketEntryType::HOT_ARCHIVE_DELETED);
                    REQUIRE(entryPtr->key() == k);
                }

                // Archived entries should contain full LedgerEntry
                else
                {
                    auto expectedIter = expectedArchiveEntries.find(k);
                    REQUIRE(expectedIter != expectedArchiveEntries.end());
                    REQUIRE(entryPtr);
                    REQUIRE(entryPtr->type() ==
                            HotArchiveBucketEntryType::HOT_ARCHIVE_ARCHIVED);
                    REQUIRE(entryPtr->archivedEntry() == expectedIter->second);
                }
            };

        auto checkResult = [&] {
            LedgerKeySet bulkLoadKeys;
            for (auto const& k : keysToSearch)
            {
                auto entryPtr = searchableBL->load(k);
                checkLoad(k, entryPtr);
                bulkLoadKeys.emplace(k);
            }

            auto bulkLoadResult = searchableBL->loadKeys(bulkLoadKeys);
            for (auto entry : bulkLoadResult)
            {
                if (entry.type() == HOT_ARCHIVE_DELETED)
                {
                    auto k = entry.key();
                    auto iter = expectedDeletedEntries.find(k);
                    REQUIRE(iter != expectedDeletedEntries.end());
                    expectedDeletedEntries.erase(iter);
                }
                else
                {
                    REQUIRE(entry.type() == HOT_ARCHIVE_ARCHIVED);
                    auto le = entry.archivedEntry();
                    auto k = LedgerEntryKey(le);
                    auto iter = expectedArchiveEntries.find(k);
                    REQUIRE(iter != expectedArchiveEntries.end());
                    REQUIRE(iter->second == le);
                    expectedArchiveEntries.erase(iter);
                }
            }

            REQUIRE(expectedDeletedEntries.empty());
            REQUIRE(expectedArchiveEntries.empty());
        };

        auto archivedEntries =
            LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                {CONTRACT_DATA, CONTRACT_CODE}, 10);
        for (auto const& e : archivedEntries)
        {
            auto k = LedgerEntryKey(e);
            expectedArchiveEntries.emplace(k, e);
            keysToSearch.emplace(k);
        }

        // Note: keys to search automatically populated by these functions
        auto deletedEntries =
            LedgerTestUtils::generateValidUniqueLedgerKeysWithTypes(
                {CONTRACT_DATA, CONTRACT_CODE}, 10, keysToSearch);
        for (auto const& k : deletedEntries)
        {
            expectedDeletedEntries.emplace(k);
        }

        auto restoredEntries =
            LedgerTestUtils::generateValidUniqueLedgerKeysWithTypes(
                {CONTRACT_DATA, CONTRACT_CODE}, 10, keysToSearch);
        for (auto const& k : restoredEntries)
        {
            expectedRestoredEntries.emplace(k);
        }

        auto header =
            app->getLedgerManager().getLastClosedLedgerHeader().header;
        header.ledgerSeq += 1;
        header.ledgerVersion = static_cast<uint32_t>(
            HotArchiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION);
        addHotArchiveBatchAndUpdateSnapshot(*app, header, archivedEntries,
                                            restoredEntries, deletedEntries);
        app->getBucketManager()
            .getBucketSnapshotManager()
            .maybeCopySearchableHotArchiveBucketListSnapshot(searchableBL);
        checkResult();

        // Add a few batches so that entries are no longer in the top bucket
        for (auto i = 0; i < 100; ++i)
        {
            header.ledgerSeq += 1;
            addHotArchiveBatchAndUpdateSnapshot(*app, header, {}, {}, {});
            app->getBucketManager()
                .getBucketSnapshotManager()
                .maybeCopySearchableHotArchiveBucketListSnapshot(searchableBL);
        }

        // Shadow entries via liveEntry
        auto liveShadow1 = LedgerEntryKey(archivedEntries[0]);
        auto liveShadow2 = deletedEntries[1];

        header.ledgerSeq += 1;
        addHotArchiveBatchAndUpdateSnapshot(*app, header, {},
                                            {liveShadow1, liveShadow2}, {});
        app->getBucketManager()
            .getBucketSnapshotManager()
            .maybeCopySearchableHotArchiveBucketListSnapshot(searchableBL);

        // Point load
        for (auto const& k : {liveShadow1, liveShadow2})
        {
            auto entryPtr = searchableBL->load(k);
            REQUIRE(!entryPtr);
        }

        // Bulk load
        auto bulkLoadResult =
            searchableBL->loadKeys({liveShadow1, liveShadow2});
        REQUIRE(bulkLoadResult.size() == 0);

        // Shadow via deletedEntry
        auto deletedShadow = LedgerEntryKey(archivedEntries[1]);

        header.ledgerSeq += 1;
        addHotArchiveBatchAndUpdateSnapshot(*app, header, {}, {},
                                            {deletedShadow});
        app->getBucketManager()
            .getBucketSnapshotManager()
            .maybeCopySearchableHotArchiveBucketListSnapshot(searchableBL);

        // Point load
        auto entryPtr = searchableBL->load(deletedShadow);
        REQUIRE(entryPtr);
        REQUIRE(entryPtr->type() ==
                HotArchiveBucketEntryType::HOT_ARCHIVE_DELETED);
        REQUIRE(entryPtr->key() == deletedShadow);

        // Bulk load
        auto bulkLoadResult2 = searchableBL->loadKeys({deletedShadow});
        REQUIRE(bulkLoadResult2.size() == 1);
        REQUIRE(bulkLoadResult2[0].type() == HOT_ARCHIVE_DELETED);
        REQUIRE(bulkLoadResult2[0].key() == deletedShadow);

        // Shadow via archivedEntry
        auto archivedShadow = archivedEntries[3];
        archivedShadow.lastModifiedLedgerSeq = ledger;

        header.ledgerSeq += 1;
        addHotArchiveBatchAndUpdateSnapshot(*app, header, {archivedShadow}, {},
                                            {});
        app->getBucketManager()
            .getBucketSnapshotManager()
            .maybeCopySearchableHotArchiveBucketListSnapshot(searchableBL);

        // Point load
        entryPtr = searchableBL->load(LedgerEntryKey(archivedShadow));
        REQUIRE(entryPtr);
        REQUIRE(entryPtr->type() ==
                HotArchiveBucketEntryType::HOT_ARCHIVE_ARCHIVED);
        REQUIRE(entryPtr->archivedEntry() == archivedShadow);

        // Bulk load
        auto bulkLoadResult3 =
            searchableBL->loadKeys({LedgerEntryKey(archivedShadow)});
        REQUIRE(bulkLoadResult3.size() == 1);
        REQUIRE(bulkLoadResult3[0].type() == HOT_ARCHIVE_ARCHIVED);
        REQUIRE(bulkLoadResult3[0].archivedEntry() == archivedShadow);
    };

    testAllIndexTypes(f);
}
}
