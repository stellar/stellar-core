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

#include "lib/xdrpp/xdrpp/autocheck.h"
#include "util/XDRCereal.h"
#include <autocheck/generator.hpp>
#include <cmath>

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
    UnorderedMap<LedgerKey, std::uint32_t> keyToEntrySize;

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

    LedgerKey
    insertContractKey(std::unordered_set<stellar::LedgerEntryType> types =
                          {TTL, CONTRACT_CODE, CONTRACT_DATA},
                      size_t minSize = 0)
    {
        LedgerEntry e;
        LedgerKey k;
        do
        {
            e = LedgerTestUtils::generateValidLedgerEntryWithTypes(types);
            k = LedgerEntryKey(e);
        } while (mTestEntries.find(k) != mTestEntries.end());
        // No key collisions.

        if (minSize > 0)
        {
            SCVal cdValue(SCV_BYTES);
            auto dataBytes =
                autocheck::generator<xdr::opaque_vec<xdr::XDR_MAX_LEN>>()(
                    minSize);
            cdValue.bytes().assign(dataBytes.begin(), dataBytes.end());
            cdValue.bytes().resize(minSize);
            e.data.contractData().val = cdValue;
            REQUIRE(xdr::xdr_size(e) >= minSize);
        }

        mTestEntries.emplace(k, e);
        mKeysToSearch.emplace(k);
        insertEntries({e});
        return k;
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
    testLoadContractKeySpanningPages()
    {
        size_t pageSize = std::pow(
            2, getTestConfig()
                   .EXPERIMENTAL_BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT);
        LedgerKeySet expectedSuccessKeys;
        LedgerKeySet expectedFailedKeys;
        LedgerKeyMeter lkMeter;
        UnorderedMap<size_t, UnorderedSet<LedgerKey>> txs;

        txs[0].emplace(insertContractKey({CONTRACT_DATA}, pageSize));
        lkMeter.addTxn(0, xdr::xdr_size(mTestEntries[*txs[0].begin()]), txs[0]);
        expectedSuccessKeys.insert(txs[0].begin(), txs[0].end());

        // loadKeys will load an entry if it has a transaction with a non-zero
        // quota. Even with a quota of 1, we can load an entry with a size
        // larger than the page size.
        txs[1].emplace(insertContractKey({CONTRACT_DATA}, pageSize));
        lkMeter.addTxn(1, 1, txs[1]);
        expectedSuccessKeys.insert(txs[1].begin(), txs[1].end());

        // tx[2] has zero quota, so no keys should succeed.
        txs[2].emplace(insertContractKey({CONTRACT_DATA}, pageSize));
        lkMeter.addTxn(2, 0, txs[2]);
        expectedFailedKeys.insert(txs[2].begin(), txs[2].end());

        // Refactor keyA and keyB in the form of the lines above.
        auto keyA = insertContractKey({CONTRACT_DATA}, pageSize);
        auto keyB = insertContractKey({CONTRACT_DATA}, pageSize);
        txs[3].emplace(keyA);
        txs[3].emplace(keyB);
        lkMeter.addTxn(3, xdr::xdr_size(mTestEntries[*txs[3].begin()]), txs[3]);

        auto loadResult = getBM().loadKeysWithLimits(mKeysToSearch, lkMeter);
        LedgerKeySet loadResultSet{};
        std::transform(
            loadResult.begin(), loadResult.end(),
            std::inserter(loadResultSet, loadResultSet.end()), [](auto& le) {
                return LedgerEntryKey(const_cast<const LedgerEntry&>(le));
            });
        // Either a or b should be loaded, but not both.
        REQUIRE(loadResultSet.count(keyA) + loadResultSet.count(keyB) == 1);
        for (auto key : expectedSuccessKeys)
        {
            REQUIRE(loadResultSet.find(key) != loadResultSet.end());
        }
        for (auto key : expectedFailedKeys)
        {
            REQUIRE(loadResultSet.find(key) == loadResultSet.end());
        }
        for (auto tx : txs)
        {
            for (auto key : tx.second)
            {
                // might fail for tx[3]
                REQUIRE(lkMeter.maxReadQuotaForKey(key) == 0);
            }
        }
    }

    void
    testLoadContractKeysWithInsufficientReadBytes()
    {
        UnorderedMap<LedgerKey, std::vector<uint32_t>> meteredLedgerKeyToTx;
        UnorderedMap<size_t, uint32_t> txReadBytes;
        LedgerKeySet expectedSuccessKeys;
        LedgerKeySet expectedFailedKeys;

        // This test inserts keys and populates meteredLedgerKeyToTx and
        // txReadBytes for several different transactions. The transactions will
        // have less than, equal to, or more than enough read quota for their
        // associated keys. The keys which do not have enough quota are added to
        // expectedFailedKeys and those which do are added to
        // expectedSuccessKeys. After calling loadKeysWithLimits, we assert:
        // * for each key in expectedSuccessKeys, key ∈ loadResult & key ∉
        // notLoaded
        // * for each key in expectedFailedKeys, key ∉ loadResult & key ∈
        // notLoaded

        UnorderedMap<LedgerKey, std::uint32_t> keyToEntrySize;

        LedgerKeyMeter lkMeter;
        UnorderedMap<size_t, UnorderedSet<LedgerKey>> txs;
        // TX 0 has one key, with zero read quota.
        txs[0].emplace(insertContractKey());
        lkMeter.addTxn(0, 0, txs[0]);
        expectedFailedKeys.insert(txs[0].begin(), txs[0].end());

        // TX 1 has one key, with read quota of 1.
        txs[1].emplace(insertContractKey());
        lkMeter.addTxn(1, 1, txs[1]);
        expectedSuccessKeys.insert(txs[1].begin(), txs[1].end());

        // TX 2 has one key, with read quota one less than the entry size
        txs[2].emplace(insertContractKey());
        lkMeter.addTxn(2, xdr::xdr_size(mTestEntries[*txs[2].begin()]) - 1,
                       txs[2]);
        expectedSuccessKeys.insert(txs[2].begin(), txs[2].end());

        // TX 3 has one key, quota equal to the entry size.
        txs[3].emplace(insertContractKey());
        lkMeter.addTxn(3, xdr::xdr_size(mTestEntries[*txs[3].begin()]), txs[3]);
        expectedSuccessKeys.insert(txs[3].begin(), txs[3].end());

        // TX 4 has one key, quota one more than the entry size.
        txs[4].emplace(insertContractKey());
        lkMeter.addTxn(4, xdr::xdr_size(mTestEntries[*txs[4].begin()]) + 1,
                       txs[4]);
        expectedSuccessKeys.insert(txs[4].begin(), txs[4].end());

        // TX 5 and 6 share a key. TX 5 has enough quota, TX 6 does not.
        auto keyShared = insertContractKey();
        txs[5].emplace(keyShared);
        txs[6].emplace(keyShared);
        lkMeter.addTxn(5, xdr::xdr_size(mTestEntries[*txs[5].begin()]), txs[5]);
        lkMeter.addTxn(6, 0, txs[6]);
        expectedSuccessKeys.insert(keyShared);

        // TX 7 has two keys, it has enough quota for one but not the other.
        // Depending on the iteration order, one or both should succeed.
        auto keyA = insertContractKey();
        auto keyB = insertContractKey();
        txs[7].emplace(keyA);
        txs[7].emplace(keyB);
        lkMeter.addTxn(7, xdr::xdr_size(mTestEntries[*txs[7].begin()]), txs[7]);

        // TX 8 has three keys, enough for all.
        uint32_t readBytes = 0;
        for (int i = 0; i < 3; i++)
        {
            auto k = insertContractKey();
            txs[8].emplace(k);
            readBytes += xdr::xdr_size(mTestEntries[*txs[8].begin()]);
        }
        lkMeter.addTxn(8, readBytes, txs[8]);
        expectedSuccessKeys.insert(txs[8].begin(), txs[8].end());

        // TODO --
        // TX 9 has a key which has max quota, but does not exist.
        // disinguish between not loaded due to quota vs not existsnat

        auto loadResult = getBM().loadKeysWithLimits(mKeysToSearch, lkMeter);
        LedgerKeySet loadResultSet{};
        std::transform(
            loadResult.begin(), loadResult.end(),
            std::inserter(loadResultSet, loadResultSet.end()), [](auto& le) {
                return LedgerEntryKey(const_cast<const LedgerEntry&>(le));
            });

        // At least one (possibly both) of a and b should be loaded.
        REQUIRE(loadResultSet.count(keyA) + loadResultSet.count(keyB) > 0);
        LedgerKeySet notLoaded{};
        for (auto key : expectedSuccessKeys)
        {
            REQUIRE(loadResultSet.find(key) != loadResultSet.end());
        }
        for (auto key : expectedFailedKeys)
        {
            REQUIRE(loadResultSet.find(key) == loadResultSet.end());
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

TEST_CASE("ContractData key with read bytes accounting",
          "[bucket][bucketindex]")
{
    auto f = [&](Config& cfg) {
        auto test = BucketIndexTest(cfg);
        test.buildGeneralTest();
        test.testLoadContractKeysWithInsufficientReadBytes();
        test.run();
    };

    testAllIndexTypes(f);
}
TEST_CASE("ContractData entry spanning pages", "[bucket][bucketindex]")
{
    auto f = [&](Config& cfg) {
        auto test = BucketIndexTest(cfg);
        test.buildGeneralTest();
        test.testLoadContractKeySpanningPages();
        test.run();
    };
    // This test is intended to the correctness test edge cases of range based
    // indexes, however we want to maintain semantics across index types.
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