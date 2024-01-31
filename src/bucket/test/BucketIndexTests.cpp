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
    insertContractKey(std::vector<int32_t>& readByteQuotaOffsets,
                      std::vector<Hash>& txs,
                      UnorderedMap<LedgerKey, UnorderedSet<Hash>>& lkToTx,
                      UnorderedMap<Hash, uint32_t>& txReadBytes,
                      UnorderedMap<LedgerKey, std::uint32_t>& keyToEntrySize)
    {
        auto e = LedgerTestUtils::generateValidLedgerEntryWithTypes(
            {TTL, CONTRACT_CODE, CONTRACT_DATA});
        auto k = LedgerEntryKey(e);
        mKeysToSearch.emplace(LedgerEntryKey(e));
        auto const& [_, inserted] = mTestEntries.emplace(k, e);
        // No key collisions
        REQUIRE(inserted);
        insertEntries({e});

        auto key_size = xdr::xdr_size(k);
        auto entry_size = xdr::xdr_size(e);

        for (size_t i = 0; i < readByteQuotaOffsets.size(); i++)
        {
            // Update lkToTx and txReadBytes.
            lkToTx[k].insert(txs[i]);
            if (readByteQuotaOffsets[i] >= 0 ||
                std::abs(readByteQuotaOffsets[i]) < key_size)
            {
                keyToEntrySize[k] = std::max(key_size, entry_size) +
                                    Bucket::kBucketEntrySizeEncodingBytes;
                txReadBytes[txs[i]] +=
                    keyToEntrySize[k] + readByteQuotaOffsets[i];
            }
        }
        return k;
    }
    LedgerKey
    insertContractKey(int32_t readByteQuotaOffset, Hash tx,
                      UnorderedMap<LedgerKey, UnorderedSet<Hash>>& lkToTx,
                      UnorderedMap<Hash, uint32_t>& txReadBytes,
                      UnorderedMap<LedgerKey, std::uint32_t>& keyToEntrySize)
    {
        std::vector<int32_t> offsets(1, readByteQuotaOffset);
        std::vector<Hash> txs{};
        txs.emplace_back(tx);
        return insertContractKey(offsets, txs, lkToTx, txReadBytes,
                                 keyToEntrySize);
    }

    LedgerKey
    insertContractDataKeyMinSize(
        size_t minSize, Hash tx,
        UnorderedMap<LedgerKey, UnorderedSet<Hash>>& lkToTx,
        UnorderedMap<Hash, uint32_t>& txReadBytes,
        UnorderedMap<LedgerKey, std::uint32_t>& keyToEntrySize)
    {
        auto e =
            LedgerTestUtils::generateValidLedgerEntryWithTypes({CONTRACT_DATA});
        SCVal cd_value(SCV_BYTES);
        xdr::opaque_vec<xdr::XDR_MAX_LEN> data_bytes;
        size_t data_bytes_size = 0;
        do
        {
            data_bytes = autocheck::generator<decltype(data_bytes)>()(minSize);
            data_bytes_size = xdr::xdr_size(data_bytes);
        } while (data_bytes_size < minSize);
        cd_value.bytes().assign(data_bytes.begin(), data_bytes.end());
        e.data.contractData().val = cd_value;

        auto k = LedgerEntryKey(e);
        mKeysToSearch.emplace(LedgerEntryKey(e));
        auto const& [_, inserted] = mTestEntries.emplace(k, e);
        // No key collisions
        REQUIRE(inserted);
        insertEntries({e});
        keyToEntrySize[k] = xdr::xdr_size(e);
        REQUIRE(keyToEntrySize[k] > minSize);
        txReadBytes[tx] +=
            keyToEntrySize[k] + Bucket::kBucketEntrySizeEncodingBytes;
        lkToTx[k].insert(tx);
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
        size_t page_size = 16384;
        for (auto const& bucketHash : getBM().getBucketListReferencedBuckets())
        {
            if (isZero(bucketHash))
            {
                continue;
            }
            auto b = getBM().getBucketByHash(bucketHash);
            REQUIRE(!b->isEmpty());
            REQUIRE(b->isIndexed());
            page_size = std::max(
                page_size,
                static_cast<size_t>(b->getIndexForTesting().getPageSize()));
        }

        UnorderedMap<LedgerKey, UnorderedSet<Hash>> lkToTx;
        UnorderedMap<Hash, uint32_t> txReadBytes;
        UnorderedSet<LedgerKey> notLoaded;
        LedgerKeySet expectedSuccessKeys;
        LedgerKeySet expectedFailedKeys;
        UnorderedMap<LedgerKey, std::uint32_t> keyToEntrySize;
        std::vector<Hash> txs(4, Hash());
        for (uint8_t i = 0; i < txs.size(); i++)
        {
            txs[i][0] = i;
        }

        expectedSuccessKeys.insert(insertContractDataKeyMinSize(
            page_size, txs[0], lkToTx, txReadBytes, keyToEntrySize));
        // loadKeys will load an entry if it has a transaction with a non-zero
        // quota. Even with a quota of 1, we can load an entry with a size
        // larger than the page size.
        expectedSuccessKeys.insert(insertContractDataKeyMinSize(
            page_size, txs[1], lkToTx, txReadBytes, keyToEntrySize));
        txReadBytes[txs[1]] = 1;
        // tx[2] has zero quota, so no keys should succeed.
        expectedFailedKeys.insert(insertContractDataKeyMinSize(
            page_size, txs[2], lkToTx, txReadBytes, keyToEntrySize));
        txReadBytes[txs[2]] = 0;

        auto key_a = insertContractDataKeyMinSize(page_size, txs[3], lkToTx,
                                                  txReadBytes, keyToEntrySize);
        auto key_b = insertContractDataKeyMinSize(page_size, txs[3], lkToTx,
                                                  txReadBytes, keyToEntrySize);
        txReadBytes[txs[3]] -=
            std::max(keyToEntrySize[key_a], keyToEntrySize[key_b]);

        auto loadResult = getBM().loadKeysWithLimits(mKeysToSearch, lkToTx,
                                                     txReadBytes, notLoaded);
        LedgerKeySet loadResultSet{};
        std::transform(
            loadResult.begin(), loadResult.end(),
            std::inserter(loadResultSet, loadResultSet.end()), [](auto& le) {
                return LedgerEntryKey(const_cast<const LedgerEntry&>(le));
            });
        // Either a or b should be loaded, but not both.
        if (notLoaded.find(key_a) == notLoaded.end())
        {
            expectedSuccessKeys.insert(key_a);
            expectedFailedKeys.insert(key_b);
        }
        else
        {
            expectedFailedKeys.insert(key_a);
            expectedSuccessKeys.insert(key_b);
        }
        REQUIRE(loadResult.size() == expectedSuccessKeys.size());
        REQUIRE(notLoaded.size() == expectedFailedKeys.size());

        for (auto key : expectedSuccessKeys)
        {
            REQUIRE(notLoaded.find(key) == notLoaded.end());
            REQUIRE(loadResultSet.find(key) != loadResultSet.end());
        }
        for (auto key : expectedFailedKeys)
        {
            REQUIRE(notLoaded.find(key) != notLoaded.end());
            REQUIRE(loadResultSet.find(key) == loadResultSet.end());
        }
        for (auto tx : txs)
        {
            REQUIRE(txReadBytes[tx] == 0);
        }
    }

    void
    testLoadContractKeysWithInsufficientReadBytes()
    {
        UnorderedMap<LedgerKey, UnorderedSet<Hash>> lkToTx;
        UnorderedMap<Hash, uint32_t> txReadBytes;
        LedgerKeySet expectedSuccessKeys;
        LedgerKeySet expectedFailedKeys;

        UnorderedMap<LedgerKey, std::uint32_t> keyToEntrySize;
        std::vector<Hash> txs(9, Hash());
        for (uint8_t i = 0; i < txs.size(); i++)
        {
            txs[i][0] = i;
        }

        // TX 0 has one key, with zero read quota.
        expectedFailedKeys.insert(insertContractKey(
            -100, txs[0], lkToTx, txReadBytes, keyToEntrySize));
        txReadBytes[txs[0]] = 0;

        // TX 1 has one key, with read quota of 1.
        expectedSuccessKeys.insert(
            insertContractKey(0, txs[1], lkToTx, txReadBytes, keyToEntrySize));
        txReadBytes[txs[1]] = 1;

        // TX 2 has one key, with read quota one less than the entry size
        expectedSuccessKeys.insert(
            insertContractKey(-1, txs[2], lkToTx, txReadBytes, keyToEntrySize));

        // TX 3 has one key, quota equal to the entry size.
        expectedSuccessKeys.insert(
            insertContractKey(0, txs[3], lkToTx, txReadBytes, keyToEntrySize));
        // TX 4 has one key, quota one more than the entry size.
        expectedSuccessKeys.insert(
            insertContractKey(1, txs[4], lkToTx, txReadBytes, keyToEntrySize));
        // TX 5 and 6 share a key. TX 5 has enough quota, TX 6 does not.
        std::vector<int32_t> offsets_shared({0, -100});
        std::vector<Hash> txs_shared({txs[5], txs[6]});
        auto key_shared = insertContractKey(offsets_shared, txs_shared, lkToTx,
                                            txReadBytes, keyToEntrySize);
        expectedSuccessKeys.insert(key_shared);
        // TX 7 has two keys, it has enough quota for one but not the other.
        // Depending on the iteration order, one or both should succeed.
        auto key_a =
            insertContractKey(-1, txs[7], lkToTx, txReadBytes, keyToEntrySize);
        auto key_b =
            insertContractKey(0, txs[7], lkToTx, txReadBytes, keyToEntrySize);
        txReadBytes[txs[7]] -=
            std::max(keyToEntrySize[key_a], keyToEntrySize[key_b]);

        // TX 8 has three keys, enough for all.
        expectedSuccessKeys.insert((
            insertContractKey(0, txs[8], lkToTx, txReadBytes, keyToEntrySize)));
        expectedSuccessKeys.insert((
            insertContractKey(0, txs[8], lkToTx, txReadBytes, keyToEntrySize)));
        expectedSuccessKeys.insert((
            insertContractKey(0, txs[8], lkToTx, txReadBytes, keyToEntrySize)));

        UnorderedSet<LedgerKey> notLoaded;
        auto loadResult = getBM().loadKeysWithLimits(mKeysToSearch, lkToTx,
                                                     txReadBytes, notLoaded);
        LedgerKeySet loadResultSet{};
        std::transform(
            loadResult.begin(), loadResult.end(),
            std::inserter(loadResultSet, loadResultSet.end()), [](auto& le) {
                return LedgerEntryKey(const_cast<const LedgerEntry&>(le));
            });

        // Either one or both of a and b should be loaded.
        REQUIRE(loadResultSet.count(key_a) + loadResultSet.count(key_b) > 0);

        for (auto key : expectedSuccessKeys)
        {
            REQUIRE(notLoaded.find(key) == notLoaded.end());
            REQUIRE(loadResultSet.find(key) != loadResultSet.end());
        }
        for (auto key : expectedFailedKeys)
        {
            REQUIRE(notLoaded.find(key) != notLoaded.end());
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
        auto test = BucketIndexTest(cfg, /*levels=*/1);
        test.buildGeneralTest();
        test.testLoadContractKeysWithInsufficientReadBytes();
        test.run();
    };

    testAllIndexTypes(f);
}
TEST_CASE("ContractData entry spanning pages", "[bucket][bucketindex]")
{
    auto f = [&](Config& cfg) {
        auto test = BucketIndexTest(cfg, /*levels=*/1);
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