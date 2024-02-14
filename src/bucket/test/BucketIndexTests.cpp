// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// This file contains tests for the BucketIndex and higher-level operations
// concerning key-value lookup based on the BucketList.

#include "bucket/BucketIndexImpl.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "bucket/test/BucketTestUtils.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "test/test.h"

#include "lib/bloom_filter.hpp"

#include "util/XDRCereal.h"

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
        } while (!BucketList::levelShouldSpill(ledger, mLevelsToBuild - 1));
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
        // Test bulk load lookup
        auto loadResult = getBM().loadKeys(mKeysToSearch);
        validateResults(mTestEntries, loadResult);

        // Test individual entry lookup
        loadResult.clear();
        for (auto const& key : mKeysToSearch)
        {
            auto entryPtr = getBM().getLedgerEntry(key);
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

            auto blLoad = getBM().loadKeys(searchSubset);
            validateResults(testEntriesSubset, blLoad);
        }
    }

    void
    testInvalidKeys()
    {
        // Load should return empty vector for keys not in bucket list
        auto keysNotInBL =
            LedgerTestUtils::generateValidLedgerEntryKeysWithExclusions(
                {CONFIG_SETTING}, 10);
        LedgerKeySet invalidKeys(keysNotInBL.begin(), keysNotInBL.end());

        // Test bulk load
        REQUIRE(getBM().loadKeys(invalidKeys).size() == 0);

        // Test individual load
        for (auto const& key : invalidKeys)
        {
            auto entryPtr = getBM().getLedgerEntry(key);
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
            std::vector<LedgerKey> toWriteNewVersion;
            if (mDist(gRandomEngine) < 30)
            {
                auto pool = LedgerTestUtils::generateValidLiquidityPoolEntry();
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
                entries.emplace_back(poolEntry);
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

            mApp->getLedgerManager().setNextLedgerEntryBatchForBucketTesting(
                {}, entries, toWriteNewVersion);
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
        auto loadResult = getBM().loadPoolShareTrustLinesByAccountAndAsset(
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
        cfg.EXPERIMENTAL_BUCKETLIST_DB = true;
        cfg.EXPERIMENTAL_BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT = 0;
        f(cfg);
    }

    SECTION("individual and range index")
    {
        Config cfg(getTestConfig());
        cfg.EXPERIMENTAL_BUCKETLIST_DB = true;

        // First 3 levels individual, last 3 range index
        cfg.EXPERIMENTAL_BUCKETLIST_DB_INDEX_CUTOFF = 1;
        f(cfg);
    }

    SECTION("range index only")
    {
        Config cfg(getTestConfig());
        cfg.EXPERIMENTAL_BUCKETLIST_DB = true;
        cfg.EXPERIMENTAL_BUCKETLIST_DB_INDEX_CUTOFF = 0;
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

TEST_CASE("serialize bucket indexes", "[bucket][bucketindex][!hide]")
{
    Config cfg(getTestConfig(0, Config::TESTDB_ON_DISK_SQLITE));

    // First 3 levels individual, last 3 range index
    cfg.EXPERIMENTAL_BUCKETLIST_DB_INDEX_CUTOFF = 1;
    cfg.EXPERIMENTAL_BUCKETLIST_DB = true;
    cfg.EXPERIMENTAL_BUCKETLIST_DB_PERSIST_INDEX = true;

    // Node is not a validator, so indexes will persist
    cfg.NODE_IS_VALIDATOR = false;
    cfg.FORCE_SCP = false;

    auto test = BucketIndexTest(cfg);
    test.buildGeneralTest();

    auto buckets = test.getBM().getBucketListReferencedBuckets();
    for (auto const& bucketHash : buckets)
    {
        if (isZero(bucketHash))
        {
            continue;
        }

        // Check if index files are saved
        auto indexFilename = test.getBM().bucketIndexFilename(bucketHash);
        REQUIRE(fs::exists(indexFilename));

        auto b = test.getBM().getBucketByHash(bucketHash);
        REQUIRE(b->isIndexed());

        auto onDiskIndex =
            BucketIndex::load(test.getBM(), indexFilename, b->getSize());
        REQUIRE(onDiskIndex);

        auto& inMemoryIndex = b->getIndexForTesting();
        REQUIRE((inMemoryIndex == *onDiskIndex));
    }

    // Restart app with different config to test that indexes created with
    // different config settings are not loaded from disk. These params will
    // invalidate every index in BL
    cfg.EXPERIMENTAL_BUCKETLIST_DB_INDEX_CUTOFF = 0;
    cfg.EXPERIMENTAL_BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT = 10;
    test.restartWithConfig(cfg);

    for (auto const& bucketHash : buckets)
    {
        if (isZero(bucketHash))
        {
            continue;
        }

        // Check if in-memory index has correct params
        auto b = test.getBM().getBucketByHash(bucketHash);
        REQUIRE(!b->isEmpty());
        REQUIRE(b->isIndexed());

        auto& inMemoryIndex = b->getIndexForTesting();
        REQUIRE(inMemoryIndex.getPageSize() == (1UL << 10));

        // Check if on-disk index rewritten with correct config params
        auto indexFilename = test.getBM().bucketIndexFilename(bucketHash);
        auto onDiskIndex =
            BucketIndex::load(test.getBM(), indexFilename, b->getSize());
        REQUIRE((inMemoryIndex == *onDiskIndex));
    }
}
}