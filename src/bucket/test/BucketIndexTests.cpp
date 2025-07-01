// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// This file contains tests for the BucketIndex and higher-level operations
// concerning key-value lookup based on the BucketList.

#include "bucket/BucketIndexUtils.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketSnapshotManager.h"
#include "bucket/BucketUtils.h"
#include "bucket/LiveBucket.h"
#include "bucket/LiveBucketList.h"
#include "bucket/test/BucketTestUtils.h"
#include "ledger/LedgerTypeUtils.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "test/test.h"

#include "util/GlobalChecks.h"
#include "util/UnorderedMap.h"
#include "util/UnorderedSet.h"
#include "util/XDRCereal.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"

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
    UnorderedSet<LedgerKey> mGeneratedKeys;

    UnorderedMap<LedgerKey, LedgerEntry> mContractCodeEntries;
    UnorderedMap<LedgerKey, LedgerEntry> mContractDataEntries;

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
    buildBucketList(std::function<void(std::vector<LedgerEntry>&)> f,
                    bool isCacheTest = false, bool sorobanOnly = false)
    {
        releaseAssertOrThrow(!(isCacheTest && sorobanOnly));

        uint32_t ledger = 0;
        do
        {
            ++ledger;
            std::vector<LedgerEntry> entries;
            if (!isCacheTest && !sorobanOnly)
            {
                entries = LedgerTestUtils::
                    generateValidUniqueLedgerEntriesWithExclusions(
                        {CONFIG_SETTING, TTL}, 10, mGeneratedKeys);
            }
            else if (isCacheTest)
            {
                entries =
                    LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                        {ACCOUNT}, 10, mGeneratedKeys);
            }
            else if (sorobanOnly)
            {

                entries =
                    LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                        {CONTRACT_DATA, CONTRACT_CODE}, 10, mGeneratedKeys);

                // Insert TTL for each entry
                for (auto& e : entries)
                {
                    if (e.data.type() == CONTRACT_CODE)
                    {
                        mContractCodeEntries.emplace(LedgerEntryKey(e), e);
                    }
                    else if (e.data.type() == CONTRACT_DATA)
                    {
                        mContractDataEntries.emplace(LedgerEntryKey(e), e);
                    }
                }
            }

            auto entriesSize = entries.size();
            for (size_t i = 0; i < entriesSize; ++i)
            {
                auto const& e = entries.at(i);
                releaseAssertOrThrow(e.data.type() != TTL);

                // Insert TTL for Soroban entries to maintain invariant
                if (isSorobanEntry(e.data))
                {
                    LedgerEntry ttl;
                    ttl.data.type(TTL);
                    ttl.data.ttl().keyHash = getTTLKey(e).ttl().keyHash;

                    // Entry should never expire
                    ttl.data.ttl().liveUntilLedgerSeq = ledger + 10'000;

                    // Add TTL key to mGeneratedKeys to maintain uniqueness
                    auto ttlKey = LedgerEntryKey(ttl);
                    mGeneratedKeys.insert(ttlKey);

                    entries.push_back(ttl);
                }
            }

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

    Application&
    getApp() const
    {
        return *mApp;
    }

    UnorderedMap<LedgerKey, LedgerEntry> const&
    getContractCodeEntries() const
    {
        return mContractCodeEntries;
    }

    UnorderedMap<LedgerKey, LedgerEntry> const&
    getContractDataEntries() const
    {
        return mContractDataEntries;
    }

    virtual void
    buildGeneralTest(bool isCacheTest = false)
    {
        auto f = [&](std::vector<LedgerEntry> const& entries) {
            // Sample ~4% of entries if not a cache test
            // For cache tests we want to load all entries to test eviction
            // behavior
            if (isCacheTest || mDist(gRandomEngine) < 10)
            {
                for (auto const& e : entries)
                {
                    auto k = LedgerEntryKey(e);
                    mTestEntries.emplace(k, e);
                    mKeysToSearch.emplace(k);
                }
            }
            mApp->getLedgerManager().setNextLedgerEntryBatchForBucketTesting(
                entries, {}, {});
        };

        buildBucketList(f, isCacheTest);
    }

    void
    runHistoricalSnapshotTest()
    {
        uint32_t ledger = 0;

        // Exclude soroban types so we don't have to insert TTLs
        auto canonicalEntry =
            LedgerTestUtils::generateValidLedgerEntryWithExclusions(
                {CONFIG_SETTING, TTL, CONTRACT_CODE, CONTRACT_DATA});
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
    buildMultiVersionTest(bool sorobanOnly = false)
    {
        std::vector<LedgerKey> toDestroy;
        std::vector<LedgerEntry> toUpdate;
        auto f = [&](std::vector<LedgerEntry> const& entries) {
            // Actually update/destroy entries for ~4% of ledgers
            if (mDist(gRandomEngine) < 10)
            {
                for (auto& e : toUpdate)
                {
                    e.lastModifiedLedgerSeq++;
                    auto iter = mTestEntries.find(LedgerEntryKey(e));
                    iter->second = e;

                    if (sorobanOnly)
                    {
                        if (e.data.type() == CONTRACT_CODE)
                        {
                            mContractCodeEntries.emplace(LedgerEntryKey(e), e);
                        }
                        else if (e.data.type() == CONTRACT_DATA)
                        {
                            mContractDataEntries.emplace(LedgerEntryKey(e), e);
                        }
                    }
                }

                for (auto const& k : toDestroy)
                {
                    mTestEntries.erase(k);
                    if (sorobanOnly)
                    {
                        if (k.type() == CONTRACT_CODE)
                        {
                            mContractCodeEntries.erase(k);
                        }
                        else if (k.type() == CONTRACT_DATA)
                        {
                            mContractDataEntries.erase(k);
                        }
                    }
                }

                mApp->getLedgerManager()
                    .setNextLedgerEntryBatchForBucketTesting(entries, toUpdate,
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
                        if (rand_flip())
                        {
                            if (e.data.type() == TTL)
                            {
                                // Make sure we don't try to update the same
                                // entry we want to destroy
                                auto ttlKey = LedgerEntryKey(e);
                                auto iter = std::find(toDestroy.begin(),
                                                      toDestroy.end(), ttlKey);
                                if (iter != toDestroy.end())
                                {
                                    continue;
                                }
                            }

                            toUpdate.emplace_back(e);
                        }
                        // Never destroy just a TTL key to preserve invariant
                        else if (e.data.type() != TTL)
                        {
                            toDestroy.emplace_back(LedgerEntryKey(e));

                            // If we destroy a soroban entry, we also destroy
                            // the corresponding TTL entry
                            if (isSorobanEntry(e.data))
                            {
                                auto ttlKey = getTTLKey(e);
                                toDestroy.emplace_back(ttlKey);

                                // Make sure we don't try to destroy the same
                                // entry we want to update
                                for (auto iter = toUpdate.begin();
                                     iter != toUpdate.end(); ++iter)
                                {
                                    if (LedgerEntryKey(*iter) == ttlKey)
                                    {
                                        toUpdate.erase(iter);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }

                mApp->getLedgerManager()
                    .setNextLedgerEntryBatchForBucketTesting(entries, {}, {});
            }
        };

        buildBucketList(f, /*isCacheTest=*/false, sorobanOnly);
    }

    void
    insertSimilarContractDataKeys()
    {
        auto templateEntry =
            LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                {CONTRACT_DATA}, 1, mGeneratedKeys)
                .front();

        auto generateEntry = [&](ContractDataDurability t) {
            auto le = templateEntry;
            le.data.contractData().durability = t;
            return le;
        };

        std::vector<LedgerEntry> entries = {
            generateEntry(ContractDataDurability::TEMPORARY),
            generateEntry(ContractDataDurability::PERSISTENT),
        };

        auto entriesSize = entries.size();
        for (size_t i = 0; i < entriesSize; ++i)
        {
            auto const& e = entries.at(i);
            LedgerEntry ttl;
            ttl.data.type(TTL);
            ttl.data.ttl().keyHash = getTTLKey(e).ttl().keyHash;
            ttl.data.ttl().liveUntilLedgerSeq =
                e.lastModifiedLedgerSeq + 10'000;

            // Add TTL key to mGeneratedKeys to maintain uniqueness
            auto ttlKey = LedgerEntryKey(ttl);
            mGeneratedKeys.insert(ttlKey);

            entries.push_back(ttl);
        }

        for (auto const& e : entries)
        {
            auto k = LedgerEntryKey(e);
            auto const& [_, inserted] = mTestEntries.emplace(k, e);

            // No key collisions
            REQUIRE(inserted);
            mKeysToSearch.emplace(k);
        }

        mApp->getLedgerManager().setNextLedgerEntryBatchForBucketTesting(
            entries, {}, {});
        closeLedger(*mApp);
    }

    virtual void
    run(std::optional<double> expectedHitRate = std::nullopt)
    {
        auto searchableBL = getBM()
                                .getBucketSnapshotManager()
                                .copySearchableLiveBucketListSnapshot();

        auto& hitMeter = getBM().getCacheHitMeter();
        auto& missMeter = getBM().getCacheMissMeter();
        auto startingHitCount = hitMeter.count();
        auto startingMissCount = missMeter.count();

        auto sumOfInMemoryEntries = 0;
        auto& liveBL = getBM().getLiveBucketList();
        for (uint32_t i = 0; i < LiveBucketList::kNumLevels; ++i)
        {
            auto level = liveBL.getLevel(i);
            auto curr = level.getCurr();
            auto snap = level.getSnap();
            if (curr->hasInMemoryEntries() && !curr->isEmpty())
            {
                sumOfInMemoryEntries +=
                    curr->getBucketEntryCounters().numEntries();
            }

            if (snap->hasInMemoryEntries() && !snap->isEmpty())
            {
                sumOfInMemoryEntries +=
                    snap->getBucketEntryCounters().numEntries();
            }
        }

        // Checks hit rate then sets startingHitCount and startingMissCount
        // to current values
        auto checkHitRate = [&](auto expectedHitRate, auto& startingHitCount,
                                auto& startingMissCount, auto numLoads) {
            if (!expectedHitRate)
            {
                return;
            }

            if (*expectedHitRate == 1.0)
            {
                // We should have no misses
                REQUIRE(missMeter.count() == startingMissCount);

                // All point loads should be hits
                // in-memory entries do not hit the cache, so we subtract them
                // from the total number of loads
                REQUIRE(hitMeter.count() ==
                        startingHitCount + numLoads - sumOfInMemoryEntries);
            }
            else
            {
                auto newMisses = missMeter.count() - startingMissCount;
                auto newHits = hitMeter.count() - startingHitCount;
                REQUIRE(newMisses > 0);
                REQUIRE(newHits > 0);
                REQUIRE(newMisses + newHits == numLoads - sumOfInMemoryEntries);

                auto hitRate =
                    static_cast<double>(newHits) / (newMisses + newHits);

                // Allow 15% deviation from expected hit rate
                REQUIRE(hitRate < *expectedHitRate * 1.15);
                REQUIRE(hitRate > *expectedHitRate * 0.85);
            }

            startingHitCount = hitMeter.count();
            startingMissCount = missMeter.count();
        };

        // Test bulk load lookup
        auto loadResult = searchableBL->loadKeys(mKeysToSearch, "test");
        validateResults(mTestEntries, loadResult);

        if (expectedHitRate)
        {
            // We should have no cache hits since we're starting from an empty
            // cache. In-memory entries are not counted in cache metrics.
            REQUIRE(hitMeter.count() == startingHitCount);
            REQUIRE(missMeter.count() == startingMissCount +
                                             mKeysToSearch.size() -
                                             sumOfInMemoryEntries);

            startingHitCount = hitMeter.count();
            startingMissCount = missMeter.count();
        }

        loadResult.clear();

        // Test individual entry lookup
        // Subtle: We use a "randomized LIFO" cache eviction policy, where we
        // select two random elements and evict the older one. Our first load
        // loads mKeysToSearch in order. If we were to do that again, we would
        // hit the worst case cache performance, since we'd always be loading
        // the oldest entry in the cache. We really just want to test that the
        // cache size is respected, so we load keys in reverse order here. That
        // way, our three loads are:
        // 1. mKeysToSearch in-order (cache warming pass above)
        // 2. mKeysToSearch in reverse order
        // 3. mKeysToSearch in-order
        // This avoids the worst case cache performance so metrics are more what
        // we would expect for tests
        for (auto iter = mKeysToSearch.rbegin(); iter != mKeysToSearch.rend();
             ++iter)
        {
            auto entryPtr = searchableBL->load(*iter);
            if (entryPtr)
            {
                loadResult.emplace_back(*entryPtr);
            }
        }

        validateResults(mTestEntries, loadResult);

        if (expectedHitRate)
        {
            checkHitRate(expectedHitRate, startingHitCount, startingMissCount,
                         mKeysToSearch.size());

            // Run bulk lookup again
            auto loadResult2 = searchableBL->loadKeys(mKeysToSearch, "test");
            validateResults(mTestEntries, loadResult2);

            checkHitRate(expectedHitRate, startingHitCount, startingMissCount,
                         mKeysToSearch.size());
        }
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

            // Not actual size, as there may be duplicated elements, but
            // good enough
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
                // Add keys not in bucket list as well. Don't add soroban keys
                // to avoid state cache
                auto addKeys =
                    LedgerTestUtils::generateValidLedgerEntryKeysWithExclusions(
                        {CONFIG_SETTING, TTL, CONTRACT_CODE, CONTRACT_DATA},
                        10);

                searchSubset.insert(addKeys.begin(), addKeys.end());
            }

            auto blLoad = searchableBL->loadKeys(searchSubset, "test");
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
            LedgerTestUtils::generateValidUniqueLedgerKeysWithTypes(
                {ACCOUNT, TRUSTLINE, DATA, CLAIMABLE_BALANCE, LIQUIDITY_POOL},
                10, mGeneratedKeys);
        LedgerKeySet invalidKeys(keysNotInBL.begin(), keysNotInBL.end());

        // Test bulk load
        REQUIRE(searchableBL->loadKeys(invalidKeys, "test").size() == 0);

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
            std::vector<LedgerKey> toDelete;
            std::vector<LedgerEntry> toUpdate;
            if (mDist(gRandomEngine) < 30)
            {
                auto pool =
                    LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                        {LIQUIDITY_POOL}, 1, mGeneratedKeys)
                        .front();

                auto& params =
                    pool.data.liquidityPool().body.constantProduct().params;

                auto trustlineToSearch = generateTrustline(
                    mAccountToSearch, pool.data.liquidityPool());
                auto trustline2 =
                    generateTrustline(mAccount2, pool.data.liquidityPool());

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

                entries.emplace_back(pool);
                entries.emplace_back(trustlineToSearch);
                entries.emplace_back(trustline2);
            }
            // Write new version via delete
            else if (shouldMultiVersion && mDist(gRandomEngine) < 10 &&
                     !mTestEntries.empty())
            {
                // Arbitrarily pick first entry of map
                auto iter = mTestEntries.begin();
                toDelete.emplace_back(iter->first);
                mTestEntries.erase(iter);
            }
            // Write new version via modify
            else if (shouldMultiVersion && mDist(gRandomEngine) < 10 &&
                     !mTestEntries.empty())
            {
                // Arbitrarily pick first entry of map
                auto iter = mTestEntries.begin();
                iter->second.data.trustLine().balance += 10;
                toUpdate.emplace_back(iter->second);
            }

            std::vector<LedgerEntry> initEntries;
            initEntries.insert(initEntries.end(), poolEntries.begin(),
                               poolEntries.end());
            initEntries.insert(initEntries.end(), entries.begin(),
                               entries.end());

            mApp->getLedgerManager().setNextLedgerEntryBatchForBucketTesting(
                initEntries, toUpdate, toDelete);
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
    buildGeneralTest(bool isCacheTest = false) override
    {
        buildTest(false);
    }

    virtual void
    buildMultiVersionTest(bool ignored = false) override
    {
        buildTest(true);
    }

    virtual void
    run(std::optional<double> expectedHitRate = std::nullopt) override
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

TEST_CASE("bl cache", "[bucket][bucketindex]")
{
    SECTION("disable cache")
    {
        Config cfg(getTestConfig());
        cfg.BUCKETLIST_DB_INDEX_CUTOFF = 0;
        cfg.BUCKETLIST_DB_MEMORY_FOR_CACHING = 0;

        auto test = BucketIndexTest(cfg);
        test.buildGeneralTest(/*isCacheTest=*/true);
        test.run();

        auto& liveBL = test.getBM().getLiveBucketList();
        for (auto i = 0; i < LiveBucketList::kNumLevels; ++i)
        {
            auto level = liveBL.getLevel(i);
            REQUIRE(level.getCurr()->getMaxCacheSize() == 0);
            REQUIRE(level.getSnap()->getMaxCacheSize() == 0);
        }

        // We shouldn't meter anything when cache is disabled
        auto& hitMeter = test.getBM().getCacheHitMeter();
        auto& missMeter = test.getBM().getCacheMissMeter();
        REQUIRE(hitMeter.count() == 0);
        REQUIRE(missMeter.count() == 0);
    }

    auto runCacheTest = [](size_t cacheSizeMb, auto checkCacheSize,
                           double expectedHitRate) {
        // Use disk index for all levels so each bucket has a cache
        Config cfg(getTestConfig());
        cfg.BUCKETLIST_DB_INDEX_CUTOFF = 0;
        cfg.BUCKETLIST_DB_MEMORY_FOR_CACHING = cacheSizeMb;

        auto test = BucketIndexTest(cfg);
        test.buildGeneralTest(/*isCacheTest=*/true);
        test.run(expectedHitRate);
        auto& liveBL = test.getBM().getLiveBucketList();

        for (auto i = 0; i < LiveBucketList::kNumLevels; ++i)
        {
            auto level = liveBL.getLevel(i);

            checkCacheSize(level.getCurr());
            checkCacheSize(level.getSnap());
        }

        return liveBL.sumBucketEntryCounters().entryTypeSizes.at(
            LedgerEntryTypeAndDurability::ACCOUNT);
    };

    auto checkCompleteCacheSize = [](auto b) {
        if (!b->isEmpty() && !b->hasInMemoryEntries())
        {
            auto cacheSize = b->getMaxCacheSize();
            auto accountsInBucket =
                b->getBucketEntryCounters().entryTypeCounts.at(
                    LedgerEntryTypeAndDurability::ACCOUNT);
            REQUIRE(cacheSize == accountsInBucket);
        }
    };

    // First run the test with a very large cache limit so we cache everything
    auto approximateCacheSizeBytes =
        runCacheTest(5'000, checkCompleteCacheSize, 1.0);

    // Run the test again, but with a partial cache
    auto cachedAccountEntries = 0;
    auto totalAccountCount = 0;
    auto checkPartialCacheSize = [&cachedAccountEntries,
                                  &totalAccountCount](auto b) {
        if (!b->isEmpty() && !b->hasInMemoryEntries())
        {
            cachedAccountEntries += b->getMaxCacheSize();
            totalAccountCount += b->getBucketEntryCounters().entryTypeCounts.at(
                LedgerEntryTypeAndDurability::ACCOUNT);
        }
    };

    // Cache approximately half of all entries
    auto fullCacheSizeMB = approximateCacheSizeBytes / 1024 / 1024;
    auto smallCacheSizeMB = fullCacheSizeMB / 2;

    // Make sure we don't round down to 0
    REQUIRE(smallCacheSizeMB > 0);

    // Because we configure in MB, actual ratio won't be 0.5 because of rounding
    // errors, so calculate it here
    double expectedCachedRatio =
        smallCacheSizeMB * 1024.0 * 1024.0 / approximateCacheSizeBytes;

    runCacheTest(smallCacheSizeMB, checkPartialCacheSize, expectedCachedRatio);

    REQUIRE(cachedAccountEntries >
            totalAccountCount * (expectedCachedRatio - 0.15));
    REQUIRE(cachedAccountEntries <
            totalAccountCount * (expectedCachedRatio + 0.15));
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

TEST_CASE("bucket entry counters", "[bucket][bucketindex]")
{
    // Initialize global counter for all of bucketlist
    std::map<LedgerEntryTypeAndDurability, size_t> totalEntryTypeCounts;
    std::map<LedgerEntryTypeAndDurability, size_t> totalEntryTypeSizes;
    for (uint32_t type =
             static_cast<uint32_t>(LedgerEntryTypeAndDurability::ACCOUNT);
         type < static_cast<uint32_t>(LedgerEntryTypeAndDurability::NUM_TYPES);
         ++type)
    {
        totalEntryTypeCounts[static_cast<LedgerEntryTypeAndDurability>(type)] =
            0;
        totalEntryTypeSizes[static_cast<LedgerEntryTypeAndDurability>(type)] =
            0;
    }

    auto checkBucket = [&](auto bucket) {
        if (bucket->isEmpty())
        {
            return;
        }

        // Local counter for each bucket
        std::map<LedgerEntryTypeAndDurability, size_t> entryTypeCounts;
        std::map<LedgerEntryTypeAndDurability, size_t> entryTypeSizes;
        for (uint32_t type =
                 static_cast<uint32_t>(LedgerEntryTypeAndDurability::ACCOUNT);
             type <
             static_cast<uint32_t>(LedgerEntryTypeAndDurability::NUM_TYPES);
             ++type)
        {
            entryTypeCounts[static_cast<LedgerEntryTypeAndDurability>(type)] =
                0;
            entryTypeSizes[static_cast<LedgerEntryTypeAndDurability>(type)] = 0;
        }

        for (LiveBucketInputIterator iter(bucket); iter; ++iter)
        {
            auto be = *iter;
            LedgerKey lk = getBucketLedgerKey(be);

            auto count = [&](LedgerEntryTypeAndDurability type) {
                entryTypeCounts[type]++;
                entryTypeSizes[type] += xdr::xdr_size(be);
                totalEntryTypeCounts[type]++;
                totalEntryTypeSizes[type] += xdr::xdr_size(be);
            };

            switch (lk.type())
            {
            case ACCOUNT:
                count(LedgerEntryTypeAndDurability::ACCOUNT);
                break;
            case TRUSTLINE:
                count(LedgerEntryTypeAndDurability::TRUSTLINE);
                break;
            case OFFER:
                count(LedgerEntryTypeAndDurability::OFFER);
                break;
            case DATA:
                count(LedgerEntryTypeAndDurability::DATA);
                break;
            case CLAIMABLE_BALANCE:
                count(LedgerEntryTypeAndDurability::CLAIMABLE_BALANCE);
                break;
            case LIQUIDITY_POOL:
                count(LedgerEntryTypeAndDurability::LIQUIDITY_POOL);
                break;
            case CONTRACT_DATA:
                if (isPersistentEntry(lk))
                {
                    count(
                        LedgerEntryTypeAndDurability::PERSISTENT_CONTRACT_DATA);
                }
                else
                {
                    count(
                        LedgerEntryTypeAndDurability::TEMPORARY_CONTRACT_DATA);
                }
                break;
            case CONTRACT_CODE:
                count(LedgerEntryTypeAndDurability::CONTRACT_CODE);
                break;
            case CONFIG_SETTING:
                count(LedgerEntryTypeAndDurability::CONFIG_SETTING);
                break;
            case TTL:
                count(LedgerEntryTypeAndDurability::TTL);
                break;
            }
        }

        auto const& indexCounters =
            bucket->getIndexForTesting().getBucketEntryCounters();
        REQUIRE(indexCounters.entryTypeCounts == entryTypeCounts);
        REQUIRE(indexCounters.entryTypeSizes == entryTypeSizes);
    };

    auto f = [&](Config& cfg) {
        auto test = BucketIndexTest(cfg);
        test.buildMultiVersionTest();

        for (auto i = 0; i < LiveBucketList::kNumLevels; ++i)
        {
            auto level = test.getBM().getLiveBucketList().getLevel(i);
            checkBucket(level.getCurr());
            checkBucket(level.getSnap());
        }

        auto summedCounters =
            test.getBM().getLiveBucketList().sumBucketEntryCounters();
        REQUIRE(summedCounters.entryTypeCounts == totalEntryTypeCounts);
        REQUIRE(summedCounters.entryTypeSizes == totalEntryTypeSizes);
    };

    testAllIndexTypes(f);
}

// Test that indexes created via an in-memory merge are identical to those
// created via a disk-based merge. Note that while both indexes are "in-memory"
// indexes, one is constructed from a file vs. a vector of BucketEntries
TEST_CASE("in-memory index construction", "[bucket][bucketindex]")
{
    auto test = [&](auto const& entries) {
        VirtualClock clock;
        Config cfg(getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT));

        // in-memory index types only
        cfg.BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT = 0;
        Application::pointer app = createTestApplication(clock, cfg);

        // Create a bucket with in-memory entries and manually index it, once
        // using file IO and once with in-memory state
        auto b = LiveBucket::fresh(
            app->getBucketManager(), getAppLedgerVersion(app), {}, entries, {},
            /*countMergeEvents=*/true, clock.getIOContext(),
            /*doFsync=*/true, /*storeInMemory=*/true,
            /*shouldIndex=*/false);

        auto indexFromFile = createIndex<LiveBucket>(
            app->getBucketManager(), b->getFilename(), b->getHash(),
            clock.getIOContext(), nullptr);

        LiveBucketInputIterator iter(b);
        auto indexFromMemory = std::make_unique<LiveBucketIndex>(
            app->getBucketManager(), b->getInMemoryEntries(),
            iter.getMetadata());

        REQUIRE(indexFromFile);
        REQUIRE(indexFromMemory);
        REQUIRE((*indexFromFile == *indexFromMemory));
    };

    SECTION("no offers")
    {
        std::vector<LedgerEntry> entries =
            LedgerTestUtils::generateValidUniqueLedgerEntriesWithExclusions(
                {CONFIG_SETTING, OFFER}, 100);
        test(entries);
    }

    SECTION("with offers at end of file")
    {
        std::vector<LedgerEntry> entries =
            LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                {ACCOUNT, OFFER}, 100);
        test(entries);
    }

    SECTION("with offers in middle of file")
    {
        std::vector<LedgerEntry> entries =
            LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                {ACCOUNT, OFFER, CONTRACT_DATA}, 100);
        test(entries);
    }
}

TEST_CASE("soroban cache population", "[soroban][bucketindex]")
{
    auto f = [&](Config& cfg) {
        auto test = BucketIndexTest(cfg);
        test.buildMultiVersionTest(/*sorobanOnly=*/true);
        test.run();

        auto& lm = test.getApp().getLedgerManager();
        auto codeEntries = test.getContractCodeEntries();
        auto dataEntries = test.getContractDataEntries();

        auto testCache = [&]() {
            auto& inMemorySorobanState = lm.getInMemorySorobanStateForTesting();

            auto snapshot = test.getBM()
                                .getBucketSnapshotManager()
                                .copySearchableLiveBucketListSnapshot();

            // First, test that the cache is maintained correctly via `addBatch`
            REQUIRE(codeEntries.size() ==
                    inMemorySorobanState.mContractCodeEntries.size());
            for (auto const& [k, v] : codeEntries)
            {
                auto inMemoryEntry =
                    inMemorySorobanState.getContractCodeEntry(k);
                REQUIRE(inMemoryEntry);

                auto liveEntry = snapshot->load(k);
                REQUIRE(liveEntry);
                REQUIRE(*liveEntry == *inMemoryEntry->ledgerEntry);

                auto ttlEntry = snapshot->load(getTTLKey(k));
                REQUIRE(ttlEntry);
                REQUIRE(ttlEntry->data.ttl().liveUntilLedgerSeq ==
                        inMemoryEntry->liveUntilLedgerSeq);
            }

            REQUIRE(dataEntries.size() ==
                    inMemorySorobanState.mContractDataEntries.size());
            for (auto const& [k, v] : dataEntries)
            {
                auto inMemoryEntry =
                    inMemorySorobanState.getContractDataEntry(k);
                REQUIRE(inMemoryEntry);

                auto liveEntry = snapshot->load(k);
                REQUIRE(liveEntry);
                REQUIRE(*liveEntry == *inMemoryEntry->ledgerEntry);

                auto ttlEntry = snapshot->load(getTTLKey(k));
                REQUIRE(ttlEntry);
                REQUIRE(ttlEntry->data.ttl().liveUntilLedgerSeq ==
                        inMemoryEntry->liveUntilLedgerSeq);
            }
        };

        // Test that we maintain the cache properly. We initialized an empty
        // cache on the genesis ledger and updated it with each call to close
        // ledger.
        testCache();

        // Now wipe cache and repopulate from scratch to test initialization on
        // a non-empty bucketlist.
        lm.rebuildInMemorySorobanStateForTesting();
        testCache();
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
            // In memory bucket indexes are not saved to disk
            if (!b->hasInMemoryEntries())
            {
                liveBuckets.emplace(b->getHash());
            }
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
        auto inMemoryCounters = inMemoryIndex.getBucketEntryCounters();
        auto onDiskCounters = onDiskIndex->getBucketEntryCounters();
        REQUIRE(inMemoryCounters == onDiskCounters);
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
                REQUIRE(entry.type() == HOT_ARCHIVE_ARCHIVED);
                auto le = entry.archivedEntry();
                auto k = LedgerEntryKey(le);
                auto iter = expectedArchiveEntries.find(k);
                REQUIRE(iter != expectedArchiveEntries.end());
                REQUIRE(iter->second == le);
                expectedArchiveEntries.erase(iter);
            }

            REQUIRE(expectedArchiveEntries.empty());
        };

        auto archivedEntries =
            LedgerTestUtils::generateUniquePersistentLedgerEntries(
                10, keysToSearch);
        for (auto const& e : archivedEntries)
        {
            auto k = LedgerEntryKey(e);
            expectedArchiveEntries.emplace(k, e);
            keysToSearch.emplace(k);
        }

        // Note: keys to search automatically populated by these functions
        auto restoredEntries =
            LedgerTestUtils::generateUniquePersistentLedgerKeys(10,
                                                                keysToSearch);
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
                                            restoredEntries);
        app->getBucketManager()
            .getBucketSnapshotManager()
            .maybeCopySearchableHotArchiveBucketListSnapshot(searchableBL);
        checkResult();

        // Add a few batches so that entries are no longer in the top bucket
        for (auto i = 0; i < 100; ++i)
        {
            header.ledgerSeq += 1;
            addHotArchiveBatchAndUpdateSnapshot(*app, header, {}, {});
            app->getBucketManager()
                .getBucketSnapshotManager()
                .maybeCopySearchableHotArchiveBucketListSnapshot(searchableBL);
        }

        // Shadow entries via liveEntry
        auto liveShadow1 = LedgerEntryKey(archivedEntries[0]);
        auto liveShadow2 = LedgerEntryKey(archivedEntries[1]);

        header.ledgerSeq += 1;
        addHotArchiveBatchAndUpdateSnapshot(*app, header, {},
                                            {liveShadow1, liveShadow2});
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

        // Shadow via archivedEntries
        auto archivedShadow = archivedEntries[3];
        archivedShadow.lastModifiedLedgerSeq = ledger;

        header.ledgerSeq += 1;
        addHotArchiveBatchAndUpdateSnapshot(*app, header, {archivedShadow}, {});
        app->getBucketManager()
            .getBucketSnapshotManager()
            .maybeCopySearchableHotArchiveBucketListSnapshot(searchableBL);

        // Point load
        auto entryPtr = searchableBL->load(LedgerEntryKey(archivedShadow));
        REQUIRE(entryPtr);
        REQUIRE(entryPtr->type() ==
                HotArchiveBucketEntryType::HOT_ARCHIVE_ARCHIVED);
        REQUIRE(entryPtr->archivedEntry() == archivedShadow);

        // Bulk load
        auto bulkLoadResult2 =
            searchableBL->loadKeys({LedgerEntryKey(archivedShadow)});
        REQUIRE(bulkLoadResult2.size() == 1);
        REQUIRE(bulkLoadResult2[0].type() == HOT_ARCHIVE_ARCHIVED);
        REQUIRE(bulkLoadResult2[0].archivedEntry() == archivedShadow);
    };

    testAllIndexTypes(f);
}

TEST_CASE("getRangeForType bounds verification", "[bucket][bucketindex]")
{
    auto f = [&](Config& cfg) {
        auto clock = VirtualClock();
        auto app = createTestApplication<BucketTestApplication>(clock, cfg);

        auto verifyIndexBounds = [](std::shared_ptr<LiveBucket const> bucket) {
            XDRInputFileStream in;
            in.open(bucket->getFilename().string());
            BucketEntry be;
            std::optional<std::streamoff> pos;

            std::optional<LedgerEntryType> lastSeenType;
            std::set<LedgerEntryType> seenTypes;

            while (in && in.readOne(be))
            {
                if (be.type() != METAENTRY)
                {
                    LedgerKey key = getBucketLedgerKey(be);
                    LedgerEntryType currentType = key.type();
                    seenTypes.insert(currentType);

                    // Check if we've transitioned to a new type
                    if (!lastSeenType || *lastSeenType != currentType)
                    {
                        // If we had a previous type, verify its upper bound
                        if (lastSeenType)
                        {
                            auto prevRange =
                                bucket->getRangeForType(*lastSeenType);
                            REQUIRE(prevRange.has_value());
                            REQUIRE(prevRange->second == pos);
                        }

                        // Verify the lower bound of the new type
                        auto currentRange =
                            bucket->getRangeForType(currentType);
                        REQUIRE(currentRange.has_value());
                        REQUIRE(currentRange->first == pos);

                        lastSeenType = currentType;
                    }
                }
                pos = in.pos();
            }

            // Verify the last type has correct upper bound (EOF)
            REQUIRE(lastSeenType);
            auto lastRange = bucket->getRangeForType(*lastSeenType);
            REQUIRE(lastRange.has_value());
            REQUIRE(lastRange->second ==
                    std::numeric_limits<std::streamoff>::max());

            // Verify that entry types not seen in the bucket return
            // std::nullopt
            for (auto type : xdr::xdr_traits<LedgerEntryType>::enum_values())
            {
                if (seenTypes.find(static_cast<LedgerEntryType>(type)) ==
                    seenTypes.end())
                {
                    auto unseenRange = bucket->getRangeForType(
                        static_cast<LedgerEntryType>(type));
                    REQUIRE(!unseenRange.has_value());
                }
            }
        };

        SECTION("Bucket contains some types")
        {
            auto entries =
                LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                    {ACCOUNT, TRUSTLINE, CLAIMABLE_BALANCE}, 40);

            app->getLedgerManager().setNextLedgerEntryBatchForBucketTesting(
                {}, entries, {});
            closeLedger(*app);

            auto& bm = app->getBucketManager();
            auto bucket = bm.getLiveBucketList().getLevel(0).getCurr();
            verifyIndexBounds(bucket);

            // Non-existent type
            auto contractDataRange = bucket->getRangeForType(CONTRACT_DATA);
            REQUIRE(!contractDataRange.has_value());
        }

        SECTION("Bucket contains only one type, mix of live and dead")
        {
            std::vector<LedgerKey> deadKeys;
            std::vector<LedgerEntry> liveEntries =
                LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                    {TRUSTLINE}, 10);
            for (auto iter = liveEntries.begin(); iter != liveEntries.end();)
            {
                if (rand_flip())
                {
                    deadKeys.push_back(LedgerEntryKey(*iter));
                    iter = liveEntries.erase(iter);
                }
                else
                {
                    ++iter;
                }
            }

            app->getLedgerManager().setNextLedgerEntryBatchForBucketTesting(
                {}, liveEntries, deadKeys);
            closeLedger(*app);

            auto bucket = app->getBucketManager()
                              .getLiveBucketList()
                              .getLevel(0)
                              .getCurr();

            verifyIndexBounds(bucket);
            auto singleRange = bucket->getRangeForType(TRUSTLINE);
            REQUIRE(singleRange.has_value());

            // For a single type, upper bound should be EOF
            REQUIRE(singleRange->second ==
                    std::numeric_limits<std::streamoff>::max());
        }

        SECTION("Scan for entries by type")
        {
            auto const numOffers = 10;
            auto const numClaimableBalances = 15;
            auto const numTrustlines = 5;

            auto offerEntries =
                LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                    {OFFER}, numOffers);
            auto claimableBalanceEntries =
                LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                    {CLAIMABLE_BALANCE}, numClaimableBalances);
            auto trustlineEntries =
                LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                    {TRUSTLINE}, numTrustlines);

            std::vector<LedgerEntry> entries;
            entries.insert(entries.end(), offerEntries.begin(),
                           offerEntries.end());
            entries.insert(entries.end(), claimableBalanceEntries.begin(),
                           claimableBalanceEntries.end());
            entries.insert(entries.end(), trustlineEntries.begin(),
                           trustlineEntries.end());

            app->getLedgerManager().setNextLedgerEntryBatchForBucketTesting(
                {}, entries, {});
            closeLedger(*app);

            auto bucket = app->getBucketManager()
                              .getLiveBucketList()
                              .getLevel(0)
                              .getCurr();
            verifyIndexBounds(bucket);

            auto searchableBL = app->getBucketManager()
                                    .getBucketSnapshotManager()
                                    .copySearchableLiveBucketListSnapshot();

            auto verifyScanForType =
                [&](LedgerEntryType type,
                    std::vector<LedgerEntry> const& entries) {
                    // Scan through the Bucket and make sure we see all entries
                    // of the given type, and the correct value of the entry
                    UnorderedMap<LedgerKey, LedgerEntry> expectedEntries;
                    for (auto const& entry : entries)
                    {
                        expectedEntries.emplace(LedgerEntryKey(entry), entry);
                    }

                    searchableBL->scanForEntriesOfType(
                        type, [&](BucketEntry const& be) {
                            auto lk = getBucketLedgerKey(be);
                            REQUIRE(lk.type() == type);
                            auto iter = expectedEntries.find(lk);
                            REQUIRE(iter != expectedEntries.end());
                            REQUIRE(iter->second == be.liveEntry());
                            expectedEntries.erase(iter);
                            return Loop::INCOMPLETE;
                        });

                    // Verify all expected entries were found
                    REQUIRE(expectedEntries.empty());
                };

            // Verify each type
            verifyScanForType(OFFER, offerEntries);
            verifyScanForType(CLAIMABLE_BALANCE, claimableBalanceEntries);
            verifyScanForType(TRUSTLINE, trustlineEntries);

            // Verify that we don't call the callback for non-existent types
            searchableBL->scanForEntriesOfType(CONTRACT_CODE,
                                               [&](BucketEntry const& be) {
                                                   REQUIRE(false);
                                                   return Loop::INCOMPLETE;
                                               });
        }
    };

    testAllIndexTypes(f);
}
}
