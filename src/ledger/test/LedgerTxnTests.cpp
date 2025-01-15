// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketManager.h"
#include "bucket/test/BucketTestUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/LedgerTypeUtils.h"
#include "ledger/NonSociRelatedException.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "lib/util/stdrandom.h"
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"
#include "util/Math.h"
#include "util/UnorderedMap.h"
#include "util/UnorderedSet.h"
#include "util/XDROperators.h"
#include <algorithm>
#include <fmt/format.h>
#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <xdrpp/autocheck.h>

using namespace stellar;

static void
validate(AbstractLedgerTxn& ltx,
         UnorderedMap<LedgerKey, LedgerTxnDelta::EntryDelta> const& expected)
{
    auto const delta = ltx.getDelta();

    auto expectedIter = expected.begin();
    auto iter = delta.entry.begin();
    while (expectedIter != expected.end() && iter != delta.entry.end())
    {
        REQUIRE(expectedIter->first == iter->first);
        REQUIRE((bool)expectedIter->second.current ==
                (bool)iter->second.current);
        if (expectedIter->second.current)
        {
            REQUIRE(*expectedIter->second.current == *iter->second.current);
        }
        REQUIRE((bool)expectedIter->second.previous ==
                (bool)iter->second.previous);
        if (expectedIter->second.previous)
        {
            REQUIRE(*expectedIter->second.previous == *iter->second.previous);
        }
        ++expectedIter;
        ++iter;
    }
    REQUIRE(expectedIter == expected.end());
    REQUIRE(iter == delta.entry.end());
}

static LedgerEntry
generateLedgerEntryWithSameKey(LedgerEntry const& leBase)
{
    LedgerEntry le;
    le.data.type(leBase.data.type());
    le.lastModifiedLedgerSeq = 1;
    do
    {
        switch (le.data.type())
        {
        case ACCOUNT:
            le.data.account() = LedgerTestUtils::generateValidAccountEntry();
            le.data.account().accountID = leBase.data.account().accountID;
            break;
        case DATA:
            le.data.data() = LedgerTestUtils::generateValidDataEntry();
            le.data.data().accountID = leBase.data.data().accountID;
            le.data.data().dataName = leBase.data.data().dataName;
            break;
        case OFFER:
            le.data.offer() = LedgerTestUtils::generateValidOfferEntry();
            le.data.offer().sellerID = leBase.data.offer().sellerID;
            le.data.offer().offerID = leBase.data.offer().offerID;
            break;
        case TRUSTLINE:
            le.data.trustLine() =
                LedgerTestUtils::generateValidTrustLineEntry();
            le.data.trustLine().accountID = leBase.data.trustLine().accountID;
            le.data.trustLine().asset = leBase.data.trustLine().asset;
            break;
        case CLAIMABLE_BALANCE:
            le.data.claimableBalance() =
                LedgerTestUtils::generateValidClaimableBalanceEntry();
            le.data.claimableBalance().balanceID =
                leBase.data.claimableBalance().balanceID;
            break;
        case LIQUIDITY_POOL:
            le.data.liquidityPool() =
                LedgerTestUtils::generateValidLiquidityPoolEntry();
            le.data.liquidityPool().liquidityPoolID =
                leBase.data.liquidityPool().liquidityPoolID;
            break;
        case CONFIG_SETTING:
            le.data.configSetting() =
                LedgerTestUtils::generateValidConfigSettingEntry();
            le.data.configSetting().configSettingID(
                leBase.data.configSetting().configSettingID());
            break;
        case CONTRACT_DATA:
            le.data.contractData() =
                LedgerTestUtils::generateValidContractDataEntry();
            le.data.contractData().contract =
                leBase.data.contractData().contract;
            le.data.contractData().key = leBase.data.contractData().key;
            le.data.contractData().durability =
                leBase.data.contractData().durability;
            break;
        case CONTRACT_CODE:
            le.data.contractCode() =
                LedgerTestUtils::generateValidContractCodeEntry();
            le.data.contractCode().hash = leBase.data.contractCode().hash;
            break;
        case TTL:
            le.data.ttl() = LedgerTestUtils::generateValidTTLEntry();
            le.data.ttl().keyHash = leBase.data.ttl().keyHash;
            break;
        }
    } while (le == leBase);
    return le;
}

TEST_CASE("LedgerTxn addChild", "[ledgertxn]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());

    SECTION("with LedgerTxn parent")
    {
        SECTION("fails if parent has children")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            LedgerTxn ltx2(ltx1);
            REQUIRE_THROWS_AS(LedgerTxn(ltx1), std::runtime_error);
        }

        SECTION("fails if parent is sealed")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            ltx1.getDelta();
            REQUIRE_THROWS_AS(LedgerTxn(ltx1), std::runtime_error);
        }
    }

    SECTION("with LedgerTxnRoot parent")
    {
        SECTION("fails if parent has children")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            REQUIRE_THROWS_AS(LedgerTxn(app->getLedgerTxnRoot()),
                              std::runtime_error);
        }
    }
}

TEST_CASE("LedgerTxn commit into LedgerTxn", "[ledgertxn]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());

    LedgerEntry le1 = LedgerTestUtils::generateValidLedgerEntryWithExclusions(
        {CONFIG_SETTING});
    le1.lastModifiedLedgerSeq = 1;
    LedgerKey key = LedgerEntryKey(le1);

    auto le2 = generateLedgerEntryWithSameKey(le1);

    SECTION("one entry")
    {
        SECTION("created in child")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());

            LedgerTxn ltx2(ltx1);
            REQUIRE(ltx2.create(le1));
            ltx2.commit();

            validate(ltx1, {{key,
                             {std::make_shared<InternalLedgerEntry const>(le1),
                              nullptr}}});
        }

        SECTION("loaded in child")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            REQUIRE(ltx1.create(le1));

            LedgerTxn ltx2(ltx1);
            REQUIRE(ltx2.load(key));
            ltx2.commit();

            validate(ltx1, {{key,
                             {std::make_shared<InternalLedgerEntry const>(le1),
                              nullptr}}});
        }

        SECTION("modified in child")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            REQUIRE(ltx1.create(le1));

            LedgerTxn ltx2(ltx1);
            auto ltxe1 = ltx2.load(key);
            REQUIRE(ltxe1);
            ltxe1.current() = le2;
            ltx2.commit();

            validate(ltx1, {{key,
                             {std::make_shared<InternalLedgerEntry const>(le2),
                              nullptr}}});
        }

        SECTION("erased in child")
        {
            le2 = generateLedgerEntryWithSameKey(le1);
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            REQUIRE(ltx1.create(le1));

            LedgerTxn ltx2(ltx1);
            REQUIRE_NOTHROW(ltx2.erase(key));
            ltx2.commit();

            validate(ltx1, {});
        }
    }

    SECTION("restored keys")
    {
        auto randomEntries =
            LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                {CONTRACT_CODE}, 2);
        std::vector<LedgerKey> randomKeys = {LedgerEntryKey(randomEntries[0]),
                                             LedgerEntryKey(randomEntries[1])};
        LedgerTxn ltx1(app->getLedgerTxnRoot());

        SECTION("hot archive restore key exists in live BL")
        {
            ltx1.create(randomEntries[0]);
            REQUIRE_THROWS(ltx1.restoreFromHotArchive(randomEntries[0], 42));
        }

        SECTION("live BL restore key does not exist")
        {
            REQUIRE_THROWS(ltx1.restoreFromLiveBucketList(randomKeys[0], 42));
        }

        auto checkKey = [](auto const& keySet, auto const& dataKey) {
            REQUIRE(keySet.find(dataKey) != keySet.end());
            REQUIRE(keySet.find(getTTLKey(dataKey)) != keySet.end());
        };

        SECTION("commited to parent")
        {
            SECTION("hot archive")
            {
                ltx1.restoreFromHotArchive(randomEntries[0], 42);

                SECTION("rollback")
                {
                    {
                        LedgerTxn ltx2(ltx1);
                        ltx2.restoreFromHotArchive(randomEntries[1], 42);
                    }

                    REQUIRE(ltx1.getRestoredLiveBucketListKeys().empty());
                    auto keys = ltx1.getRestoredHotArchiveKeys();

                    // Data key + TTL
                    REQUIRE(keys.size() == 2);
                    checkKey(keys, randomKeys[0]);
                }

                SECTION("commit")
                {
                    {
                        LedgerTxn ltx2(ltx1);
                        ltx2.restoreFromHotArchive(randomEntries[1], 42);
                        ltx2.commit();
                    }

                    REQUIRE(ltx1.getRestoredLiveBucketListKeys().empty());
                    auto keys = ltx1.getRestoredHotArchiveKeys();

                    // (data key + TTL) * 2
                    REQUIRE(keys.size() == 4);
                    checkKey(keys, randomKeys[0]);
                    checkKey(keys, randomKeys[1]);
                }
            }

            SECTION("live BL")
            {
                auto getTTLEntry = [](LedgerKey const& key) {
                    LedgerEntry ttl;
                    ttl.data.type(TTL);
                    ttl.data.ttl().liveUntilLedgerSeq = 42;
                    ttl.data.ttl().keyHash = getTTLKey(key).ttl().keyHash;
                    return ttl;
                };

                // Populate live BL with key, then restore it
                ltx1.create(randomEntries[0]);
                ltx1.create(getTTLEntry(randomKeys[0]));
                ltx1.restoreFromLiveBucketList(randomKeys[0], 42);

                SECTION("rollback")
                {
                    {
                        LedgerTxn ltx2(ltx1);
                        ltx2.create(randomEntries[1]);
                        ltx2.create(getTTLEntry(randomKeys[1]));
                        ltx2.restoreFromLiveBucketList(randomKeys[1], 42);
                    }

                    REQUIRE(ltx1.getRestoredHotArchiveKeys().empty());
                    auto keys = ltx1.getRestoredLiveBucketListKeys();

                    // Data key + TTL
                    REQUIRE(keys.size() == 2);
                    checkKey(keys, randomKeys[0]);
                }

                SECTION("commit")
                {
                    {
                        LedgerTxn ltx2(ltx1);
                        ltx2.create(randomEntries[1]);
                        ltx2.create(getTTLEntry(randomKeys[1]));
                        ltx2.restoreFromLiveBucketList(randomKeys[1], 42);
                        ltx2.commit();
                    }

                    REQUIRE(ltx1.getRestoredHotArchiveKeys().empty());
                    auto keys = ltx1.getRestoredLiveBucketListKeys();

                    // (data key + TTL) * 2
                    REQUIRE(keys.size() == 4);
                    checkKey(keys, randomKeys[0]);
                    checkKey(keys, randomKeys[1]);
                }
            }
        }
    }
}

TEST_CASE("LedgerTxn rollback into LedgerTxn", "[ledgertxn]")
{
    auto runTest = [&](Config::TestDbMode mode) {
        VirtualClock clock;
        auto app = createTestApplication(clock, getTestConfig(0, mode));

        LedgerEntry le1 =
            LedgerTestUtils::generateValidLedgerEntryWithExclusions(
                {CONFIG_SETTING});
        le1.lastModifiedLedgerSeq = 1;
        LedgerKey key = LedgerEntryKey(le1);

        auto le2 = generateLedgerEntryWithSameKey(le1);

        SECTION("one entry")
        {
            SECTION("created in child")
            {
                LedgerTxn ltx1(app->getLedgerTxnRoot());

                LedgerTxn ltx2(ltx1);
                REQUIRE(ltx2.create(le1));
                ltx2.rollback();

                validate(ltx1, {});
            }

            SECTION("loaded in child")
            {
                LedgerTxn ltx1(app->getLedgerTxnRoot());
                REQUIRE(ltx1.create(le1));

                LedgerTxn ltx2(ltx1);
                REQUIRE(ltx2.load(key));
                ltx2.rollback();

                validate(ltx1,
                         {{key,
                           {std::make_shared<InternalLedgerEntry const>(le1),
                            nullptr}}});
            }

            SECTION("modified in child")
            {
                LedgerTxn ltx1(app->getLedgerTxnRoot());
                REQUIRE(ltx1.create(le1));

                LedgerTxn ltx2(ltx1);
                auto ltxe1 = ltx2.load(key);
                REQUIRE(ltxe1);
                ltxe1.current() = le2;
                ltx2.rollback();

                validate(ltx1,
                         {{key,
                           {std::make_shared<InternalLedgerEntry const>(le1),
                            nullptr}}});
            }

            SECTION("erased in child")
            {
                le1 = LedgerTestUtils::generateValidLedgerEntryWithExclusions(
                    {CONFIG_SETTING});
                le1.lastModifiedLedgerSeq = 1;
                key = LedgerEntryKey(le1);
                le2 = generateLedgerEntryWithSameKey(le1);
                LedgerTxn ltx1(app->getLedgerTxnRoot());
                REQUIRE(ltx1.create(le1));

                LedgerTxn ltx2(ltx1);
                REQUIRE_NOTHROW(ltx2.erase(key));
                ltx2.rollback();

                validate(ltx1,
                         {{key,
                           {std::make_shared<InternalLedgerEntry const>(le1),
                            nullptr}}});
            }
        }
    };

    SECTION("default")
    {
        runTest(Config::TESTDB_DEFAULT);
    }

#ifdef USE_POSTGRES
    SECTION("postgresql")
    {
        runTest(Config::TESTDB_POSTGRESQL);
    }
#endif
}

TEST_CASE("LedgerTxn round trip", "[ledgertxn]")
{
    std::bernoulli_distribution shouldCommitDist;

    auto generateNew = [](AbstractLedgerTxn& ltx,
                          UnorderedMap<LedgerKey, LedgerEntry>& entries,
                          bool offerOnly) {
        size_t const NEW_ENTRIES = 100;
        UnorderedMap<LedgerKey, LedgerEntry> newBatch;
        while (newBatch.size() < NEW_ENTRIES)
        {
            auto le =
                offerOnly
                    ? LedgerTestUtils::generateValidLedgerEntryOfType(OFFER)
                    : LedgerTestUtils::generateValidLedgerEntryWithExclusions(
                          {CONFIG_SETTING});

            auto key = LedgerEntryKey(le);
            if (entries.find(LedgerEntryKey(le)) == entries.end())
            {
                le.lastModifiedLedgerSeq = 1;
                newBatch[LedgerEntryKey(le)] = le;
            }
        }

        for (auto const& kv : newBatch)
        {
            REQUIRE(ltx.create(kv.second));
            entries[kv.first] = kv.second;
        }
    };

    auto generateModify = [](AbstractLedgerTxn& ltx,
                             UnorderedMap<LedgerKey, LedgerEntry>& entries) {
        size_t const MODIFY_ENTRIES = 25;
        UnorderedMap<LedgerKey, LedgerEntry> modifyBatch;
        stellar::uniform_int_distribution<size_t> dist(0, entries.size() - 1);
        while (modifyBatch.size() < MODIFY_ENTRIES)
        {
            auto iter = entries.begin();
            std::advance(iter, dist(gRandomEngine));
            modifyBatch[iter->first] =
                generateLedgerEntryWithSameKey(iter->second);
        }

        for (auto const& kv : modifyBatch)
        {
            auto ltxe = ltx.load(kv.first);
            REQUIRE(ltxe);
            ltxe.current() = kv.second;
            entries[kv.first] = kv.second;
        }
    };

    auto generateErase = [&](AbstractLedgerTxn& ltx,
                             UnorderedMap<LedgerKey, LedgerEntry>& entries,
                             UnorderedSet<LedgerKey>& dead) {
        size_t const ERASE_ENTRIES = 25;
        UnorderedSet<LedgerKey> eraseBatch;
        stellar::uniform_int_distribution<size_t> dist(0, entries.size() - 1);
        while (eraseBatch.size() < ERASE_ENTRIES)
        {
            auto iter = entries.begin();
            std::advance(iter, dist(gRandomEngine));
            if (iter->first.type() == CONFIG_SETTING)
            {
                continue;
            }
            eraseBatch.insert(iter->first);
        }

        for (auto const& key : eraseBatch)
        {
            REQUIRE_NOTHROW(ltx.erase(key));
            entries.erase(key);
            dead.insert(key);
        }
    };

    auto checkLedger = [](AbstractLedgerTxnParent& ltxParent,
                          UnorderedMap<LedgerKey, LedgerEntry> const& entries,
                          UnorderedSet<LedgerKey> const& dead) {
        LedgerTxn ltx(ltxParent);
        for (auto const& kv : entries)
        {
            auto ltxe = ltx.load(kv.first);
            REQUIRE(ltxe);
            REQUIRE(ltxe.current() == kv.second);
        }

        for (auto const& key : dead)
        {
            if (entries.find(key) == entries.end())
            {
                REQUIRE(!ltx.load(key));
            }
        }
    };

    auto runTest = [&](AbstractLedgerTxnParent& ltxParent, bool offerOnly) {
        UnorderedMap<LedgerKey, LedgerEntry> entries;
        UnorderedSet<LedgerKey> dead;
        size_t const NUM_BATCHES = 10;
        for (size_t k = 0; k < NUM_BATCHES; ++k)
        {
            checkLedger(ltxParent, entries, dead);

            UnorderedMap<LedgerKey, LedgerEntry> updatedEntries = entries;
            UnorderedSet<LedgerKey> updatedDead = dead;
            LedgerTxn ltx1(ltxParent);
            generateNew(ltx1, updatedEntries, offerOnly);
            generateModify(ltx1, updatedEntries);
            generateErase(ltx1, updatedEntries, updatedDead);

            if (entries.empty() || shouldCommitDist(gRandomEngine))
            {
                entries = updatedEntries;
                dead = updatedDead;
                REQUIRE_NOTHROW(ltx1.commit());
            }
        }
    };

    auto runTestWithDbMode = [&](Config::TestDbMode mode) {
        SECTION("round trip to LedgerTxn")
        {
            VirtualClock clock;
            auto app = createTestApplication(clock, getTestConfig(0, mode));

            LedgerTxn ltx1(app->getLedgerTxnRoot());
            runTest(ltx1, false);
        }

        SECTION("round trip to LedgerTxnRoot")
        {
            SECTION("with normal caching")
            {
                VirtualClock clock;
                // BucketListDB incompatible with direct root commits
                auto app = createTestApplication(clock, getTestConfig(0, mode));

                runTest(app->getLedgerTxnRoot(), true);
            }

            SECTION("with no cache")
            {
                VirtualClock clock;

                // BucketListDB incompatible with direct root commits
                auto cfg = getTestConfig(0, mode);
                cfg.ENTRY_CACHE_SIZE = 0;
                auto app = createTestApplication(clock, cfg);

                runTest(app->getLedgerTxnRoot(), true);
            }
        }
    };

    SECTION("bucketlist")
    {
        runTestWithDbMode(Config::TESTDB_BUCKET_DB_PERSISTENT);
    }

    SECTION("in-memory")
    {
        runTestWithDbMode(Config::TESTDB_IN_MEMORY);
    }

#ifdef USE_POSTGRES
    SECTION("postgresql")
    {
        runTestWithDbMode(Config::TESTDB_POSTGRESQL);
    }
#endif
}

TEST_CASE("LedgerTxn rollback and commit deactivate", "[ledgertxn]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());

    auto& root = app->getLedgerTxnRoot();
    auto lh = root.getHeader();

    LedgerEntry le = LedgerTestUtils::generateValidLedgerEntryWithExclusions(
        {CONFIG_SETTING});
    LedgerKey key = LedgerEntryKey(le);

    auto checkDeactivate = [&](std::function<void(LedgerTxn & ltx)> f) {
        SECTION("entry")
        {
            LedgerTxn ltx(root, false);
            auto entry = ltx.create(le);
            REQUIRE(entry);
            f(ltx);
            REQUIRE_THROWS_AS(!entry, std::runtime_error);
        }

        SECTION("const entry")
        {
            LedgerTxn ltx(root, false);
            ltx.create(le);
            auto entry = ltx.loadWithoutRecord(key);
            REQUIRE(entry);
            f(ltx);
            REQUIRE_THROWS_AS(!entry, std::runtime_error);
        }

        SECTION("header")
        {
            LedgerTxn ltx(root, false);
            auto header = ltx.loadHeader();
            REQUIRE(header);
            f(ltx);
            REQUIRE(!header);
        }
    };

    SECTION("commit")
    {
        checkDeactivate([](LedgerTxn& ltx) { ltx.commit(); });
    }

    SECTION("rollback")
    {
        checkDeactivate([](LedgerTxn& ltx) { ltx.rollback(); });
    }
}

TEST_CASE("LedgerTxn create", "[ledgertxn]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());

    LedgerEntry le = LedgerTestUtils::generateValidLedgerEntryWithExclusions(
        {CONFIG_SETTING});
    le.lastModifiedLedgerSeq = 1;
    LedgerKey key = LedgerEntryKey(le);

    SECTION("fails with children")
    {
        LedgerTxn ltx1(app->getLedgerTxnRoot());
        LedgerTxn ltx2(ltx1);
        REQUIRE_THROWS_AS(ltx1.create(le), std::runtime_error);
    }

    SECTION("fails if sealed")
    {
        LedgerTxn ltx1(app->getLedgerTxnRoot());
        ltx1.getDelta();
        REQUIRE_THROWS_AS(ltx1.create(le), std::runtime_error);
    }

    SECTION("when key does not exist")
    {
        LedgerTxn ltx1(app->getLedgerTxnRoot());
        REQUIRE(ltx1.create(le));
        validate(ltx1, {{key,
                         {std::make_shared<InternalLedgerEntry const>(le),
                          nullptr}}});
    }

    SECTION("when key exists in self or parent")
    {
        LedgerTxn ltx1(app->getLedgerTxnRoot());
        REQUIRE(ltx1.create(le));
        REQUIRE_THROWS_AS(ltx1.create(le), std::runtime_error);

        LedgerTxn ltx2(ltx1);
        REQUIRE_THROWS_AS(ltx2.create(le), std::runtime_error);
        validate(ltx2, {});
    }

    SECTION("when key exists in grandparent, erased in parent")
    {
        LedgerTxn ltx1(app->getLedgerTxnRoot());
        REQUIRE(ltx1.create(le));

        LedgerTxn ltx2(ltx1);
        REQUIRE_NOTHROW(ltx2.erase(key));

        LedgerTxn ltx3(ltx2);
        REQUIRE(ltx3.create(le));
        validate(ltx3, {{key,
                         {std::make_shared<InternalLedgerEntry const>(le),
                          nullptr}}});
    }
}

TEST_CASE("LedgerTxn createWithoutLoading and updateWithoutLoading",
          "[ledgertxn]")
{
    auto runTest = [&](Config::TestDbMode mode) {
        VirtualClock clock;
        auto app = createTestApplication(clock, getTestConfig(0, mode));

        LedgerEntry le =
            LedgerTestUtils::generateValidLedgerEntryWithExclusions(
                {CONFIG_SETTING});
        le.lastModifiedLedgerSeq = 1;
        LedgerKey key = LedgerEntryKey(le);

        SECTION("fails with children")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            LedgerTxn ltx2(ltx1);
            REQUIRE_THROWS_AS(ltx1.createWithoutLoading(le),
                              std::runtime_error);
            REQUIRE_THROWS_AS(ltx1.updateWithoutLoading(le),
                              std::runtime_error);
        }

        SECTION("fails if sealed")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            ltx1.getDelta();
            REQUIRE_THROWS_AS(ltx1.createWithoutLoading(le),
                              std::runtime_error);
            REQUIRE_THROWS_AS(ltx1.updateWithoutLoading(le),
                              std::runtime_error);
        }

        SECTION("when key does not exist")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            REQUIRE_NOTHROW(ltx1.createWithoutLoading(le));
            validate(ltx1, {{key,
                             {std::make_shared<InternalLedgerEntry const>(le),
                              nullptr}}});
        }

        SECTION("when key exists in self or parent")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            REQUIRE(ltx1.create(le));
            REQUIRE_NOTHROW(ltx1.updateWithoutLoading(le));

            LedgerTxn ltx2(ltx1);
            REQUIRE_NOTHROW(ltx2.updateWithoutLoading(le));
            validate(ltx2,
                     {{key,
                       {std::make_shared<InternalLedgerEntry const>(le),
                        std::make_shared<InternalLedgerEntry const>(le)}}});
        }

        SECTION("when key is active during overwrite")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            auto ltxe = ltx1.create(le);
            REQUIRE(ltxe);
            REQUIRE_THROWS_AS(ltx1.createWithoutLoading(le),
                              std::runtime_error);
            REQUIRE_THROWS_AS(ltx1.updateWithoutLoading(le),
                              std::runtime_error);
        }

        SECTION("when key exists in grandparent, erased in parent")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            REQUIRE(ltx1.create(le));

            LedgerTxn ltx2(ltx1);
            REQUIRE_NOTHROW(ltx2.erase(key));

            LedgerTxn ltx3(ltx2);
            REQUIRE_NOTHROW(ltx3.createWithoutLoading(le));
            validate(ltx3, {{key,
                             {std::make_shared<InternalLedgerEntry const>(le),
                              nullptr}}});
        }
    };

    SECTION("bucketlist")
    {
        runTest(Config::TESTDB_BUCKET_DB_PERSISTENT);
    }

    SECTION("in-memory")
    {
        runTest(Config::TESTDB_IN_MEMORY);
    }

#ifdef USE_POSTGRES
    SECTION("postgresql")
    {
        runTest(Config::TESTDB_POSTGRESQL);
    }
#endif
}

TEST_CASE("LedgerTxn erase", "[ledgertxn]")
{
    auto runTest = [&](Config::TestDbMode mode) {
        VirtualClock clock;
        auto app = createTestApplication(clock, getTestConfig(0, mode));

        LedgerEntry le =
            LedgerTestUtils::generateValidLedgerEntryWithExclusions(
                {CONFIG_SETTING});
        le.lastModifiedLedgerSeq = 1;
        LedgerKey key = LedgerEntryKey(le);

        SECTION("fails with children")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            REQUIRE(ltx1.create(le));

            LedgerTxn ltx2(ltx1);
            REQUIRE_THROWS_AS(ltx1.erase(key), std::runtime_error);
        }

        SECTION("fails if sealed")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            REQUIRE(ltx1.create(le));
            ltx1.getDelta();
            REQUIRE_THROWS_AS(ltx1.erase(key), std::runtime_error);
        }

        SECTION("fails for configuration")
        {
            auto configLe =
                LedgerTestUtils::generateValidLedgerEntryOfType(CONFIG_SETTING);
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            // The config entry should already be present in ledger in starting
            // with vNext.
            REQUIRE_THROWS_AS(ltx1.erase(LedgerEntryKey(configLe)),
                              std::runtime_error);
        }

        SECTION("when key does not exist")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            REQUIRE_THROWS_AS(ltx1.erase(key), std::runtime_error);
            validate(ltx1, {});
        }

        SECTION("when key exists in parent")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            REQUIRE(ltx1.create(le));

            LedgerTxn ltx2(ltx1);
            REQUIRE_NOTHROW(ltx2.erase(key));
            validate(
                ltx2,
                {{key,
                  {nullptr, std::make_shared<InternalLedgerEntry const>(le)}}});
        }

        SECTION("when key exists in grandparent, erased in parent")
        {
            le = LedgerTestUtils::generateValidLedgerEntryWithExclusions(
                {CONFIG_SETTING});
            le.lastModifiedLedgerSeq = 1;
            key = LedgerEntryKey(le);
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            REQUIRE(ltx1.create(le));

            LedgerTxn ltx2(ltx1);
            REQUIRE_NOTHROW(ltx2.erase(key));

            LedgerTxn ltx3(ltx2);
            REQUIRE_THROWS_AS(ltx3.erase(key), std::runtime_error);
            validate(ltx3, {});
        }
    };
    SECTION("bucketlist")
    {
        runTest(Config::TESTDB_BUCKET_DB_PERSISTENT);
    }

    SECTION("in-memory")
    {
        runTest(Config::TESTDB_IN_MEMORY);
    }

#ifdef USE_POSTGRES
    SECTION("postgresql")
    {
        runTest(Config::TESTDB_POSTGRESQL);
    }
#endif
}

TEST_CASE("LedgerTxn eraseWithoutLoading", "[ledgertxn]")
{
    auto runTest = [&](Config::TestDbMode mode) {
        VirtualClock clock;
        auto app = createTestApplication(clock, getTestConfig(0, mode));

        LedgerEntry le =
            LedgerTestUtils::generateValidLedgerEntryWithExclusions(
                {CONFIG_SETTING});
        le.lastModifiedLedgerSeq = 1;
        LedgerKey key = LedgerEntryKey(le);

        SECTION("fails with children")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            REQUIRE(ltx1.create(le));

            LedgerTxn ltx2(ltx1);
            REQUIRE_THROWS_AS(ltx1.eraseWithoutLoading(key),
                              std::runtime_error);
        }

        SECTION("fails if sealed")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            REQUIRE(ltx1.create(le));
            std::vector<LedgerEntry> init, live;
            std::vector<LedgerKey> dead;
            ltx1.getAllEntries(init, live, dead);
            REQUIRE_THROWS_AS(ltx1.eraseWithoutLoading(key),
                              std::runtime_error);
        }

        SECTION("fails for configuration")
        {
            auto configLe =
                LedgerTestUtils::generateValidLedgerEntryOfType(CONFIG_SETTING);
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            // The config entry should already be present in ledger in starting
            // with vNext.
            REQUIRE_THROWS_AS(ltx1.erase(LedgerEntryKey(configLe)),
                              std::runtime_error);
        }

        SECTION("when key does not exist")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            REQUIRE_NOTHROW(ltx1.eraseWithoutLoading(key));
            REQUIRE_THROWS_AS(ltx1.getDelta(), std::runtime_error);
            REQUIRE(ltx1.getNewestVersion(key).get() == nullptr);
        }

        SECTION("when key exists in parent")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            REQUIRE(ltx1.create(le));

            LedgerTxn ltx2(ltx1);
            REQUIRE_NOTHROW(ltx2.eraseWithoutLoading(key));
            REQUIRE_THROWS_AS(ltx2.getDelta(), std::runtime_error);
            REQUIRE(ltx2.getNewestVersion(key).get() == nullptr);
        }

        SECTION("when key exists in grandparent, erased in parent")
        {
            le = LedgerTestUtils::generateValidLedgerEntryWithExclusions(
                {CONFIG_SETTING});
            le.lastModifiedLedgerSeq = 1;
            key = LedgerEntryKey(le);
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            REQUIRE(ltx1.create(le));

            LedgerTxn ltx2(ltx1);
            REQUIRE_NOTHROW(ltx2.erase(key));

            LedgerTxn ltx3(ltx2);
            REQUIRE_NOTHROW(ltx3.eraseWithoutLoading(key));
            REQUIRE_THROWS_AS(ltx3.getDelta(), std::runtime_error);
            REQUIRE(ltx3.getNewestVersion(key).get() == nullptr);
        }
    };

    SECTION("bucketlist")
    {
        runTest(Config::TESTDB_BUCKET_DB_PERSISTENT);
    }

    SECTION("in-memory")
    {
        runTest(Config::TESTDB_IN_MEMORY);
    }

#ifdef USE_POSTGRES
    SECTION("postgresql")
    {
        runTest(Config::TESTDB_POSTGRESQL);
    }
#endif
}

static void
applyLedgerTxnUpdates(
    AbstractLedgerTxn& ltx,
    std::map<AccountID, std::pair<AccountID, int64_t>> const& updates)
{
    for (auto const& kv : updates)
    {
        auto ltxe = loadAccount(ltx, kv.first);
        if (ltxe && kv.second.second > 0)
        {
            auto& ae = ltxe.current().data.account();
            ae.inflationDest.activate() = kv.second.first;
            ae.balance = kv.second.second;
        }
        else if (ltxe)
        {
            ltxe.erase();
        }
        else
        {
            REQUIRE(kv.second.second > 0);
            LedgerEntry acc;
            acc.lastModifiedLedgerSeq = ltx.loadHeader().current().ledgerSeq;
            acc.data.type(ACCOUNT);

            auto& ae = acc.data.account();
            ae = LedgerTestUtils::generateValidAccountEntry();
            ae.accountID = kv.first;
            ae.inflationDest.activate() = kv.second.first;
            ae.balance = kv.second.second;

            ltx.create(acc);
        }
    }
}

static void
testInflationWinners(
    AbstractLedgerTxnParent& ltxParent, size_t maxWinners, int64_t minBalance,
    std::vector<std::tuple<AccountID, int64_t>> const& expected,
    std::vector<std::map<AccountID,
                         std::pair<AccountID, int64_t>>>::const_iterator begin,
    std::vector<std::map<AccountID, std::pair<AccountID, int64_t>>>::
        const_iterator const& end)
{
    REQUIRE(begin != end);
    LedgerTxn ltx(ltxParent);
    applyLedgerTxnUpdates(ltx, *begin);

    if (++begin != end)
    {
        testInflationWinners(ltx, maxWinners, minBalance, expected, begin, end);
    }
    else
    {
        auto winners = ltx.queryInflationWinners(maxWinners, minBalance);
        auto expectedIter = expected.begin();
        auto iter = winners.begin();
        while (expectedIter != expected.end() && iter != winners.end())
        {
            REQUIRE(*expectedIter ==
                    std::make_tuple(iter->accountID, iter->votes));
            ++expectedIter;
            ++iter;
        }
        REQUIRE(expectedIter == expected.end());
        REQUIRE(iter == winners.end());
    }
}

static void
testInflationWinners(
    size_t maxWinners, int64_t minBalance,
    std::vector<std::tuple<AccountID, int64_t>> const& expected,
    std::vector<std::map<AccountID, std::pair<AccountID, int64_t>>> const&
        updates)
{
    REQUIRE(!updates.empty());

    auto testAtRoot = [&](Application& app) {
        {
            LedgerTxn ltx1(app.getLedgerTxnRoot());
            applyLedgerTxnUpdates(ltx1, *updates.cbegin());
            ltx1.commit();
        }
        testInflationWinners(app.getLedgerTxnRoot(), maxWinners, minBalance,
                             expected, ++updates.cbegin(), updates.cend());
    };

    // first changes are in LedgerTxnRoot with cache
    if (updates.size() > 1)
    {
        VirtualClock clock;
        auto app = createTestApplication(
            clock, getTestConfig(0, Config::TESTDB_IN_MEMORY));

        testAtRoot(*app);
    }

    // first changes are in LedgerTxnRoot without cache
    if (updates.size() > 1)
    {
        VirtualClock clock;
        auto cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);
        cfg.ENTRY_CACHE_SIZE = 0;
        auto app = createTestApplication(clock, cfg);

        testAtRoot(*app);
    }

    // first changes are in child of LedgerTxnRoot
    {
        VirtualClock clock;
        auto app = createTestApplication(
            clock, getTestConfig(0, Config::TESTDB_IN_MEMORY));

        testInflationWinners(app->getLedgerTxnRoot(), maxWinners, minBalance,
                             expected, updates.cbegin(), updates.cend());
    }
}

TEST_CASE("LedgerTxn queryInflationWinners", "[ledgertxn]")
{
    int64_t const QUERY_VOTE_MINIMUM = 1000000000;

    auto a1 = LedgerTestUtils::generateValidAccountEntry().accountID;
    auto a2 = LedgerTestUtils::generateValidAccountEntry().accountID;
    auto a3 = LedgerTestUtils::generateValidAccountEntry().accountID;
    auto a4 = LedgerTestUtils::generateValidAccountEntry().accountID;

    auto inflationSort =
        [](std::vector<std::tuple<AccountID, int64_t>> winners) {
            std::sort(winners.begin(), winners.end(),
                      [](auto const& lhs, auto const& rhs) {
                          if (std::get<1>(lhs) == std::get<1>(rhs))
                          {
                              return KeyUtils::toStrKey(std::get<0>(lhs)) >
                                     KeyUtils::toStrKey(std::get<0>(rhs));
                          }
                          return std::get<1>(lhs) > std::get<1>(rhs);
                      });
            return winners;
        };

    SECTION("fails with children")
    {
        VirtualClock clock;
        auto app = createTestApplication(clock, getTestConfig());

        LedgerTxn ltx1(app->getLedgerTxnRoot());
        LedgerTxn ltx2(ltx1);
        REQUIRE_THROWS_AS(ltx1.queryInflationWinners(1, 1), std::runtime_error);
    }

    SECTION("fails if sealed")
    {
        VirtualClock clock;
        auto app = createTestApplication(clock, getTestConfig());

        LedgerTxn ltx1(app->getLedgerTxnRoot());
        ltx1.getDelta();
        REQUIRE_THROWS_AS(ltx1.queryInflationWinners(1, 1), std::runtime_error);
    }

    SECTION("empty parent")
    {
        SECTION("no voters")
        {
            testInflationWinners(1, QUERY_VOTE_MINIMUM, {}, {{}});
        }

        SECTION("one voter")
        {
            SECTION("below query minimum")
            {
                testInflationWinners(1, 1, {},
                                     {{{a1, {a2, QUERY_VOTE_MINIMUM - 1}}}});
            }

            SECTION("above query minimum")
            {
                testInflationWinners(1, 1, {{a2, QUERY_VOTE_MINIMUM}},
                                     {{{a1, {a2, QUERY_VOTE_MINIMUM}}}});
            }
        }

        SECTION("two voters")
        {
            SECTION("max one winner")
            {
                SECTION("same inflation destination")
                {
                    testInflationWinners(
                        1, QUERY_VOTE_MINIMUM,
                        {{a3, 2 * QUERY_VOTE_MINIMUM + 10}},
                        {{{a1, {a3, QUERY_VOTE_MINIMUM + 3}},
                          {a2, {a3, QUERY_VOTE_MINIMUM + 7}}}});

                    SECTION("with total near min votes boundary")
                    {
                        testInflationWinners(
                            1, 2 * QUERY_VOTE_MINIMUM + 10,
                            {{a3, 2 * QUERY_VOTE_MINIMUM + 10}},
                            {{{a1, {a3, QUERY_VOTE_MINIMUM + 3}},
                              {a2, {a3, QUERY_VOTE_MINIMUM + 7}}}});
                        testInflationWinners(
                            1, 2 * QUERY_VOTE_MINIMUM + 11, {},
                            {{{a1, {a3, QUERY_VOTE_MINIMUM + 3}},
                              {a2, {a3, QUERY_VOTE_MINIMUM + 7}}}});
                    }
                }

                SECTION("different inflation destinations")
                {
                    testInflationWinners(
                        1, QUERY_VOTE_MINIMUM, {{a4, QUERY_VOTE_MINIMUM + 7}},
                        {{{a1, {a3, QUERY_VOTE_MINIMUM + 3}},
                          {a2, {a4, QUERY_VOTE_MINIMUM + 7}}}});
                }
            }

            SECTION("max two winners")
            {
                SECTION("different inflation destinations")
                {
                    testInflationWinners(
                        2, QUERY_VOTE_MINIMUM,
                        inflationSort({{a3, QUERY_VOTE_MINIMUM + 3},
                                       {a4, QUERY_VOTE_MINIMUM + 7}}),
                        {{{a1, {a3, QUERY_VOTE_MINIMUM + 3}},
                          {a2, {a4, QUERY_VOTE_MINIMUM + 7}}}});
                    testInflationWinners(
                        2, QUERY_VOTE_MINIMUM + 5,
                        {{a4, QUERY_VOTE_MINIMUM + 7}},
                        {{{a1, {a3, QUERY_VOTE_MINIMUM + 3}},
                          {a2, {a4, QUERY_VOTE_MINIMUM + 7}}}});
                }
            }
        }
    }

    SECTION("one voter in parent")
    {
        SECTION("below query minimum")
        {
            testInflationWinners(1, 1, {},
                                 {{{a1, {a2, QUERY_VOTE_MINIMUM - 1}}}, {}});
        }

        SECTION("above query minimum")
        {
            testInflationWinners(1, 1, {{a2, QUERY_VOTE_MINIMUM}},
                                 {{{a1, {a2, QUERY_VOTE_MINIMUM}}}, {}});
        }

        SECTION("modified balance")
        {
            SECTION("from above to below query minimum")
            {
                testInflationWinners(1, 1, {},
                                     {{{a1, {a2, QUERY_VOTE_MINIMUM}}},
                                      {{a1, {a2, QUERY_VOTE_MINIMUM - 1}}}});
            }

            SECTION("from below to above query minimum")
            {
                testInflationWinners(1, 1, {{a2, QUERY_VOTE_MINIMUM}},
                                     {{{a1, {a2, QUERY_VOTE_MINIMUM - 1}}},
                                      {{a1, {a2, QUERY_VOTE_MINIMUM}}}});
            }
        }

        SECTION("modified inflation destination")
        {
            testInflationWinners(2, QUERY_VOTE_MINIMUM,
                                 {{a3, QUERY_VOTE_MINIMUM}},
                                 {{{a1, {a2, QUERY_VOTE_MINIMUM}}},
                                  {{a1, {a3, QUERY_VOTE_MINIMUM}}}});
        }

        SECTION("other voter")
        {
            SECTION("max one winner")
            {
                SECTION("same inflation destination")
                {
                    testInflationWinners(
                        1, QUERY_VOTE_MINIMUM,
                        {{a3, 2 * QUERY_VOTE_MINIMUM + 10}},
                        {{{a1, {a3, QUERY_VOTE_MINIMUM + 3}}},
                         {{a2, {a3, QUERY_VOTE_MINIMUM + 7}}}});

                    SECTION("with total near min votes boundary")
                    {
                        testInflationWinners(
                            1, 2 * QUERY_VOTE_MINIMUM + 10,
                            {{a3, 2 * QUERY_VOTE_MINIMUM + 10}},
                            {{{a1, {a3, QUERY_VOTE_MINIMUM + 3}}},
                             {{a2, {a3, QUERY_VOTE_MINIMUM + 7}}}});
                        testInflationWinners(
                            1, 2 * QUERY_VOTE_MINIMUM + 11, {},
                            {{{a1, {a3, QUERY_VOTE_MINIMUM + 3}}},
                             {{a2, {a3, QUERY_VOTE_MINIMUM + 7}}}});
                    }
                }

                SECTION("different inflation destinations")
                {
                    testInflationWinners(
                        1, QUERY_VOTE_MINIMUM, {{a4, QUERY_VOTE_MINIMUM + 7}},
                        {{{a1, {a3, QUERY_VOTE_MINIMUM + 3}}},
                         {{a2, {a4, QUERY_VOTE_MINIMUM + 7}}}});
                    testInflationWinners(
                        1, QUERY_VOTE_MINIMUM, {{a3, QUERY_VOTE_MINIMUM + 7}},
                        {{{a1, {a3, QUERY_VOTE_MINIMUM + 7}}},
                         {{a2, {a4, QUERY_VOTE_MINIMUM + 3}}}});
                }
            }

            SECTION("max two winners")
            {
                SECTION("different inflation destinations")
                {
                    testInflationWinners(
                        2, QUERY_VOTE_MINIMUM,
                        inflationSort({{a3, QUERY_VOTE_MINIMUM + 3},
                                       {a4, QUERY_VOTE_MINIMUM + 7}}),
                        {{{a1, {a3, QUERY_VOTE_MINIMUM + 3}}},
                         {{a2, {a4, QUERY_VOTE_MINIMUM + 7}}}});
                    testInflationWinners(
                        2, QUERY_VOTE_MINIMUM + 5,
                        {{a4, QUERY_VOTE_MINIMUM + 7}},
                        {{{a1, {a3, QUERY_VOTE_MINIMUM + 3}}},
                         {{a2, {a4, QUERY_VOTE_MINIMUM + 7}}}});
                    testInflationWinners(
                        2, QUERY_VOTE_MINIMUM + 5,
                        {{a3, QUERY_VOTE_MINIMUM + 7}},
                        {{{a1, {a3, QUERY_VOTE_MINIMUM + 7}}},
                         {{a2, {a4, QUERY_VOTE_MINIMUM + 3}}}});
                }
            }
        }
    }

    SECTION("two voters in parent")
    {
        SECTION("max one winner")
        {
            SECTION("same inflation destination")
            {
                testInflationWinners(1, QUERY_VOTE_MINIMUM,
                                     {{a3, 2 * QUERY_VOTE_MINIMUM + 10}},
                                     {{{a1, {a3, QUERY_VOTE_MINIMUM + 3}},
                                       {a2, {a3, QUERY_VOTE_MINIMUM + 7}}},
                                      {}});

                SECTION("with total near min votes boundary")
                {
                    testInflationWinners(1, 2 * QUERY_VOTE_MINIMUM + 10,
                                         {{a3, 2 * QUERY_VOTE_MINIMUM + 10}},
                                         {{{a1, {a3, QUERY_VOTE_MINIMUM + 3}},
                                           {a2, {a3, QUERY_VOTE_MINIMUM + 7}}},
                                          {}});
                    testInflationWinners(1, 2 * QUERY_VOTE_MINIMUM + 11, {},
                                         {{{a1, {a3, QUERY_VOTE_MINIMUM + 3}},
                                           {a2, {a3, QUERY_VOTE_MINIMUM + 7}}},
                                          {}});
                }
            }

            SECTION("different inflation destinations")
            {
                testInflationWinners(1, QUERY_VOTE_MINIMUM,
                                     {{a4, QUERY_VOTE_MINIMUM + 7}},
                                     {{{a1, {a3, QUERY_VOTE_MINIMUM + 3}},
                                       {a2, {a4, QUERY_VOTE_MINIMUM + 7}}},
                                      {}});
            }
        }

        SECTION("max two winners")
        {
            SECTION("different inflation destinations")
            {
                testInflationWinners(
                    2, QUERY_VOTE_MINIMUM,
                    inflationSort({{a3, QUERY_VOTE_MINIMUM + 3},
                                   {a4, QUERY_VOTE_MINIMUM + 7}}),
                    {{{a1, {a3, QUERY_VOTE_MINIMUM + 3}},
                      {a2, {a4, QUERY_VOTE_MINIMUM + 7}}},
                     {}});
                testInflationWinners(2, QUERY_VOTE_MINIMUM + 5,
                                     {{a4, QUERY_VOTE_MINIMUM + 7}},
                                     {{{a1, {a3, QUERY_VOTE_MINIMUM + 3}},
                                       {a2, {a4, QUERY_VOTE_MINIMUM + 7}}},
                                      {}});
            }
        }
    }
}

TEST_CASE("LedgerTxn loadHeader", "[ledgertxn]")
{
    auto runTest = [&](Config::TestDbMode mode) {
        VirtualClock clock;
        auto app = createTestApplication(clock, getTestConfig(0, mode));

        LedgerHeader lh = autocheck::generator<LedgerHeader>()(5);

        SECTION("fails with children")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            LedgerTxn ltx2(ltx1);
            REQUIRE_THROWS_AS(ltx1.loadHeader(), std::runtime_error);
        }

        SECTION("fails if sealed")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            ltx1.getDelta();
            REQUIRE_THROWS_AS(ltx1.loadHeader(), std::runtime_error);
        }

        SECTION("fails if header already loaded")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            auto lhe = ltx1.loadHeader();
            REQUIRE(lhe);
            REQUIRE_THROWS_AS(ltx1.loadHeader(), std::runtime_error);
        }

        SECTION("check after update")
        {
            LedgerTxn ltx1(app->getLedgerTxnRoot());
            auto lhPrev = ltx1.loadHeader().current();
            ltx1.loadHeader().current() = lh;

            auto delta = ltx1.getDelta();
            REQUIRE(delta.header.current == lh);
            REQUIRE(delta.header.previous == lhPrev);
        }
    };

    SECTION("bucketlist")
    {
        runTest(Config::TESTDB_BUCKET_DB_PERSISTENT);
    }

    SECTION("in-memory")
    {
        runTest(Config::TESTDB_IN_MEMORY);
    }

#ifdef USE_POSTGRES
    SECTION("postgresql")
    {
        runTest(Config::TESTDB_POSTGRESQL);
    }
#endif
}

TEST_CASE_VERSIONS("LedgerTxn load", "[ledgertxn]")
{
    auto runTest = [&](Config::TestDbMode mode) {
        VirtualClock clock;
        auto app = createTestApplication(clock, getTestConfig(0, mode));

        SECTION("use generated entry")
        {
            LedgerEntry le =
                LedgerTestUtils::generateValidLedgerEntryWithExclusions(
                    {CONFIG_SETTING});
            le.lastModifiedLedgerSeq = 1;
            LedgerKey key = LedgerEntryKey(le);

            for_versions_from(18, *app, [&] {
                SECTION("fails with children")
                {
                    LedgerTxn ltx1(app->getLedgerTxnRoot());
                    LedgerTxn ltx2(ltx1);
                    REQUIRE_THROWS_AS(ltx1.load(key), std::runtime_error);
                }

                SECTION("fails if sealed")
                {
                    LedgerTxn ltx1(app->getLedgerTxnRoot());
                    ltx1.getDelta();
                    REQUIRE_THROWS_AS(ltx1.load(key), std::runtime_error);
                }

                SECTION("when key does not exist")
                {
                    LedgerTxn ltx1(app->getLedgerTxnRoot());
                    REQUIRE(!ltx1.load(key));
                    validate(ltx1, {});
                }

                SECTION("when key exists in parent")
                {
                    LedgerTxn ltx1(app->getLedgerTxnRoot());
                    REQUIRE(ltx1.create(le));

                    LedgerTxn ltx2(ltx1);
                    REQUIRE(ltx2.load(key));
                    validate(
                        ltx2,
                        {{key,
                          {std::make_shared<InternalLedgerEntry const>(le),
                           std::make_shared<InternalLedgerEntry const>(le)}}});
                }

                SECTION("when key exists in grandparent, erased in parent")
                {
                    key = LedgerEntryKey(le);
                    LedgerTxn ltx1(app->getLedgerTxnRoot());
                    REQUIRE(ltx1.create(le));

                    LedgerTxn ltx2(ltx1);
                    REQUIRE_NOTHROW(ltx2.erase(key));

                    LedgerTxn ltx3(ltx2);
                    REQUIRE(!ltx3.load(key));
                    validate(ltx3, {});
                }

                SECTION("check for init after child commits")
                {
                    LedgerTxn ltx1(app->getLedgerTxnRoot());
                    REQUIRE(ltx1.create(le));

                    LedgerTxn ltx2(ltx1);
                    REQUIRE(ltx2.load(key));
                    ltx2.commit();

                    REQUIRE(ltx1.load(key));
                }

                SECTION("create, deactivate, and load")
                {
                    LedgerTxn ltx1(app->getLedgerTxnRoot());
                    auto ltxe = ltx1.create(le);
                    ltxe.deactivate();

                    REQUIRE(ltx1.load(key));
                    ltx1.commit();
                }
            });
        }
    };

    SECTION("bucketlist")
    {
        runTest(Config::TESTDB_BUCKET_DB_PERSISTENT);
    }

    SECTION("in-memory")
    {
        runTest(Config::TESTDB_IN_MEMORY);
    }

#ifdef USE_POSTGRES
    SECTION("postgresql")
    {
        runTest(Config::TESTDB_POSTGRESQL);
    }
#endif
}

TEST_CASE("LedgerTxn loadWithoutRecord", "[ledgertxn]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());

    LedgerEntry le = LedgerTestUtils::generateValidLedgerEntryWithExclusions(
        {CONFIG_SETTING});
    le.lastModifiedLedgerSeq = 1;
    LedgerKey key = LedgerEntryKey(le);

    SECTION("fails with children")
    {
        LedgerTxn ltx1(app->getLedgerTxnRoot());
        LedgerTxn ltx2(ltx1);
        REQUIRE_THROWS_AS(ltx1.loadWithoutRecord(key), std::runtime_error);
    }

    SECTION("fails if sealed")
    {
        LedgerTxn ltx1(app->getLedgerTxnRoot());
        ltx1.getDelta();
        REQUIRE_THROWS_AS(ltx1.loadWithoutRecord(key), std::runtime_error);
    }

    SECTION("when key does not exist")
    {
        LedgerTxn ltx1(app->getLedgerTxnRoot());
        REQUIRE(!ltx1.loadWithoutRecord(key));
        validate(ltx1, {});
    }

    SECTION("when key exists in parent")
    {
        LedgerTxn ltx1(app->getLedgerTxnRoot());
        REQUIRE(ltx1.create(le));

        LedgerTxn ltx2(ltx1);
        REQUIRE(ltx2.loadWithoutRecord(key));
        validate(ltx2, {});
    }

    SECTION("when key exists in grandparent, erased in parent")
    {
        LedgerTxn ltx1(app->getLedgerTxnRoot());
        REQUIRE(ltx1.create(le));

        LedgerTxn ltx2(ltx1);
        REQUIRE_NOTHROW(ltx2.erase(key));

        LedgerTxn ltx3(ltx2);
        REQUIRE(!ltx3.loadWithoutRecord(key));
        validate(ltx3, {});
    }
}

static void
applyLedgerTxnUpdates(
    AbstractLedgerTxn& ltx,
    std::map<std::pair<AccountID, int64_t>,
             std::tuple<Asset, Asset, int64_t>> const& updates)
{
    for (auto const& kv : updates)
    {
        auto ltxe = loadOffer(ltx, kv.first.first, kv.first.second);
        if (ltxe && std::get<2>(kv.second) > 0)
        {
            auto& oe = ltxe.current().data.offer();
            std::tie(oe.buying, oe.selling, oe.amount) = kv.second;
        }
        else if (ltxe)
        {
            ltxe.erase();
        }
        else
        {
            REQUIRE(std::get<2>(kv.second) > 0);
            LedgerEntry offer;
            offer.lastModifiedLedgerSeq = ltx.loadHeader().current().ledgerSeq;
            offer.data.type(OFFER);

            auto& oe = offer.data.offer();
            oe = LedgerTestUtils::generateValidOfferEntry();
            std::tie(oe.sellerID, oe.offerID) = kv.first;
            std::tie(oe.buying, oe.selling, oe.amount) = kv.second;

            ltx.create(offer);
        }
    }
}

static void
testAllOffers(
    AbstractLedgerTxnParent& ltxParent,
    std::map<AccountID,
             std::vector<std::tuple<int64_t, Asset, Asset, int64_t>>> const&
        expected,
    std::vector<std::map<std::pair<AccountID, int64_t>,
                         std::tuple<Asset, Asset, int64_t>>>::const_iterator
        begin,
    std::vector<
        std::map<std::pair<AccountID, int64_t>,
                 std::tuple<Asset, Asset, int64_t>>>::const_iterator const& end)
{
    REQUIRE(begin != end);
    LedgerTxn ltx(ltxParent);
    applyLedgerTxnUpdates(ltx, *begin);

    if (++begin != end)
    {
        testAllOffers(ltx, expected, begin, end);
    }
    else
    {
        auto offers = ltx.loadAllOffers();
        auto expectedIter = expected.begin();
        auto iter = offers.begin();
        while (expectedIter != expected.end() && iter != offers.end())
        {
            REQUIRE(expectedIter->first == iter->first);

            auto expectedInner = expectedIter->second;
            auto const& inner = iter->second;

            REQUIRE(expectedInner.size() == inner.size());
            for (auto& innerCur : inner)
            {
                auto const& oe = innerCur.current().data.offer();
                auto d = std::make_tuple(oe.offerID, oe.buying, oe.selling,
                                         oe.amount);
                auto expectedInnerIter =
                    std::find(expectedInner.begin(), expectedInner.end(), d);
                REQUIRE(expectedInnerIter != expectedInner.end());
                expectedInner.erase(expectedInnerIter);
            }

            ++expectedIter;
            ++iter;
        }
        REQUIRE(expectedIter == expected.end());
        REQUIRE(iter == offers.end());
    }
}

static void
testAllOffers(
    std::map<AccountID,
             std::vector<std::tuple<int64_t, Asset, Asset, int64_t>>> const&
        expected,
    std::vector<std::map<std::pair<AccountID, int64_t>,
                         std::tuple<Asset, Asset, int64_t>>> const& updates,
    Config::TestDbMode mode)
{
    REQUIRE(!updates.empty());

    auto testAtRoot = [&](Application& app) {
        {
            LedgerTxn ltx1(app.getLedgerTxnRoot());
            applyLedgerTxnUpdates(ltx1, *updates.cbegin());
            ltx1.commit();
        }
        testAllOffers(app.getLedgerTxnRoot(), expected, ++updates.cbegin(),
                      updates.cend());
    };

    // first changes are in LedgerTxnRoot with cache
    if (updates.size() > 1)
    {
        VirtualClock clock;
        auto app = createTestApplication(clock, getTestConfig(0, mode));

        testAtRoot(*app);
    }

    // first changes are in LedgerTxnRoot without cache
    if (updates.size() > 1)
    {
        VirtualClock clock;
        auto cfg = getTestConfig(0, mode);
        cfg.ENTRY_CACHE_SIZE = 0;
        auto app = createTestApplication(clock, cfg);

        testAtRoot(*app);
    }

    // first changes are in child of LedgerTxnRoot
    {
        VirtualClock clock;
        auto app = createTestApplication(clock, getTestConfig(0, mode));

        testAllOffers(app->getLedgerTxnRoot(), expected, updates.cbegin(),
                      updates.cend());
    }
}

TEST_CASE("LedgerTxn loadAllOffers", "[ledgertxn]")
{
    auto runTest = [&](Config::TestDbMode mode) {
        auto a1 = LedgerTestUtils::generateValidAccountEntry().accountID;
        auto a2 = LedgerTestUtils::generateValidAccountEntry().accountID;

        Asset buying = LedgerTestUtils::generateValidOfferEntry().buying;
        Asset selling = LedgerTestUtils::generateValidOfferEntry().selling;

        SECTION("fails with children")
        {
            VirtualClock clock;
            auto app = createTestApplication(clock, getTestConfig(0, mode));

            LedgerTxn ltx1(app->getLedgerTxnRoot());
            LedgerTxn ltx2(ltx1);
            REQUIRE_THROWS_AS(ltx1.loadAllOffers(), std::runtime_error);
        }

        SECTION("fails if sealed")
        {
            VirtualClock clock;
            auto app = createTestApplication(clock, getTestConfig(0, mode));

            LedgerTxn ltx1(app->getLedgerTxnRoot());
            ltx1.getDelta();
            REQUIRE_THROWS_AS(ltx1.loadAllOffers(), std::runtime_error);
        }

        SECTION("empty parent")
        {
            SECTION("no offers")
            {
                testAllOffers({}, {{}}, mode);
            }

            SECTION("two offers")
            {
                SECTION("same account")
                {
                    testAllOffers(
                        {{a1,
                          {{1, buying, selling, 1}, {2, buying, selling, 1}}}},
                        {{{{a1, 1}, {buying, selling, 1}},
                          {{a1, 2}, {buying, selling, 1}}}},
                        mode);
                }

                SECTION("different accounts")
                {
                    testAllOffers({{a1, {{1, buying, selling, 1}}},
                                   {a2, {{2, buying, selling, 1}}}},
                                  {{{{a1, 1}, {buying, selling, 1}},
                                    {{a2, 2}, {buying, selling, 1}}}},
                                  mode);
                }
            }
        }

        SECTION("one offer in parent")
        {
            SECTION("erased in child")
            {
                testAllOffers({},
                              {{{{a1, 1}, {buying, selling, 1}}},
                               {{{a1, 1}, {buying, selling, 0}}}},
                              mode);
            }

            SECTION("modified assets in child")
            {
                testAllOffers({{a1, {{1, selling, buying, 1}}}},
                              {{{{a1, 1}, {buying, selling, 1}}},
                               {{{a1, 1}, {selling, buying, 1}}}},
                              mode);
            }

            SECTION("modified amount in child")
            {
                testAllOffers({{a1, {{1, buying, selling, 7}}}},
                              {{{{a1, 1}, {buying, selling, 1}}},
                               {{{a1, 1}, {buying, selling, 7}}}},
                              mode);
            }

            SECTION("other offer in child")
            {
                SECTION("same account")
                {
                    testAllOffers(
                        {{a1,
                          {{1, buying, selling, 1}, {2, buying, selling, 1}}}},
                        {{{{a1, 1}, {buying, selling, 1}}},
                         {{{a1, 2}, {buying, selling, 1}}}},
                        mode);
                    testAllOffers(
                        {{a1,
                          {{1, buying, selling, 1}, {2, buying, selling, 1}}}},
                        {{{{a1, 2}, {buying, selling, 1}}},
                         {{{a1, 1}, {buying, selling, 1}}}},
                        mode);
                }

                SECTION("different accounts")
                {
                    testAllOffers({{a1, {{1, buying, selling, 1}}},
                                   {a2, {{2, buying, selling, 1}}}},
                                  {{{{a1, 1}, {buying, selling, 1}}},
                                   {{{a2, 2}, {buying, selling, 1}}}},
                                  mode);
                }
            }
        }

        SECTION("two offers in parent")
        {
            SECTION("same account")
            {
                testAllOffers(
                    {{a1, {{1, buying, selling, 1}, {2, buying, selling, 1}}}},
                    {{{{a1, 1}, {buying, selling, 1}},
                      {{a1, 2}, {buying, selling, 1}}},
                     {}},
                    mode);
            }

            SECTION("different accounts")
            {
                testAllOffers({{a1, {{1, buying, selling, 1}}},
                               {a2, {{2, buying, selling, 1}}}},
                              {{{{a1, 1}, {buying, selling, 1}},
                                {{a2, 2}, {buying, selling, 1}}},
                               {}},
                              mode);
            }
        }
    };

    SECTION("bucketlist")
    {
        runTest(Config::TESTDB_BUCKET_DB_PERSISTENT);
    }

    SECTION("in-memory")
    {
        runTest(Config::TESTDB_IN_MEMORY);
    }

#ifdef USE_POSTGRES
    SECTION("postgresql")
    {
        runTest(Config::TESTDB_POSTGRESQL);
    }
#endif
}

static void
applyLedgerTxnUpdates(
    AbstractLedgerTxn& ltx,
    std::map<std::pair<AccountID, int64_t>,
             std::tuple<Asset, Asset, Price, int64_t>> const& updates)
{
    for (auto const& kv : updates)
    {
        auto ltxe = loadOffer(ltx, kv.first.first, kv.first.second);
        if (ltxe && std::get<3>(kv.second) > 0)
        {
            auto& oe = ltxe.current().data.offer();
            std::tie(oe.buying, oe.selling, oe.price, oe.amount) = kv.second;
        }
        else if (ltxe)
        {
            ltxe.erase();
        }
        else
        {
            REQUIRE(std::get<3>(kv.second) > 0);
            LedgerEntry offer;
            offer.lastModifiedLedgerSeq = ltx.loadHeader().current().ledgerSeq;
            offer.data.type(OFFER);

            auto& oe = offer.data.offer();
            oe = LedgerTestUtils::generateValidOfferEntry();
            std::tie(oe.sellerID, oe.offerID) = kv.first;
            std::tie(oe.buying, oe.selling, oe.price, oe.amount) = kv.second;

            ltx.create(offer);
        }
    }
}

static void
testBestOffer(
    AbstractLedgerTxnParent& ltxParent, Asset const& buying,
    Asset const& selling,
    std::vector<std::tuple<int64_t, Asset, Asset, Price, int64_t>> const&
        expected,
    std::vector<std::map<std::pair<AccountID, int64_t>,
                         std::tuple<Asset, Asset, Price, int64_t>>>::
        const_iterator begin,
    std::vector<std::map<std::pair<AccountID, int64_t>,
                         std::tuple<Asset, Asset, Price, int64_t>>>::
        const_iterator const& end)
{
    REQUIRE(begin != end);
    LedgerTxn ltx(ltxParent);
    applyLedgerTxnUpdates(ltx, *begin);

    if (++begin != end)
    {
        testBestOffer(ltx, buying, selling, expected, begin, end);
    }
    else
    {
        auto offer = ltx.loadBestOffer(buying, selling);
        if (offer)
        {
            auto const& oe = offer.current().data.offer();
            REQUIRE(expected.size() == 1);
            REQUIRE(expected.front() == std::make_tuple(oe.offerID, oe.buying,
                                                        oe.selling, oe.price,
                                                        oe.amount));
        }
        else
        {
            REQUIRE(expected.empty());
        }
    }
}

static void
testBestOffer(
    Asset const& buying, Asset const& selling,
    std::vector<std::tuple<int64_t, Asset, Asset, Price, int64_t>> const&
        expected,
    std::vector<std::map<std::pair<AccountID, int64_t>,
                         std::tuple<Asset, Asset, Price, int64_t>>>
        updates,
    Config::TestDbMode mode)
{
    REQUIRE(!updates.empty());

    auto testAtRoot = [&](Application& app) {
        {
            LedgerTxn ltx1(app.getLedgerTxnRoot());
            applyLedgerTxnUpdates(ltx1, *updates.cbegin());
            ltx1.commit();
        }
        testBestOffer(app.getLedgerTxnRoot(), buying, selling, expected,
                      ++updates.cbegin(), updates.cend());
    };

    // first changes are in LedgerTxnRoot with cache
    if (updates.size() > 1)
    {
        VirtualClock clock;
        auto app = createTestApplication(clock, getTestConfig(0, mode));

        testAtRoot(*app);
    }

    // first changes are in LedgerTxnRoot without cache
    if (updates.size() > 1)
    {
        VirtualClock clock;
        auto cfg = getTestConfig(0, mode);
        cfg.ENTRY_CACHE_SIZE = 0;
        auto app = createTestApplication(clock, cfg);

        testAtRoot(*app);
    }

    // first changes are in child of LedgerTxnRoot
    {
        VirtualClock clock;
        auto app = createTestApplication(clock, getTestConfig(0, mode));

        testBestOffer(app->getLedgerTxnRoot(), buying, selling, expected,
                      updates.cbegin(), updates.cend());
    }
}

TEST_CASE("LedgerTxn loadBestOffer", "[ledgertxn]")
{
    auto runTest = [&](Config::TestDbMode mode) {
        auto a1 = LedgerTestUtils::generateValidAccountEntry().accountID;
        auto a2 = LedgerTestUtils::generateValidAccountEntry().accountID;

        Asset buying = LedgerTestUtils::generateValidOfferEntry().buying;
        Asset selling = LedgerTestUtils::generateValidOfferEntry().selling;
        REQUIRE(!(buying == selling));

        SECTION("fails with children")
        {
            VirtualClock clock;
            auto app = createTestApplication(clock, getTestConfig(0, mode));

            LedgerTxn ltx1(app->getLedgerTxnRoot());
            LedgerTxn ltx2(ltx1);
            REQUIRE_THROWS_AS(ltx1.loadBestOffer(buying, selling),
                              std::runtime_error);
        }

        SECTION("fails if sealed")
        {
            VirtualClock clock;
            auto app = createTestApplication(clock, getTestConfig(0, mode));

            LedgerTxn ltx1(app->getLedgerTxnRoot());
            ltx1.getDelta();
            REQUIRE_THROWS_AS(ltx1.loadBestOffer(buying, selling),
                              std::runtime_error);
        }

        SECTION("fails with active entries")
        {
            VirtualClock clock;
            auto app = createTestApplication(clock, getTestConfig(0, mode));

            LedgerTxn ltx1(app->getLedgerTxnRoot());
            auto ltxe = ltx1.create(
                LedgerTestUtils::generateValidLedgerEntryWithExclusions(
                    {CONFIG_SETTING}));
            REQUIRE_THROWS_AS(ltx1.getBestOffer(buying, selling),
                              std::runtime_error);
            REQUIRE_THROWS_AS(
                ltx1.getBestOffer(buying, selling, {Price{1, 1}, 1}),
                std::runtime_error);
            REQUIRE_THROWS_AS(ltx1.loadBestOffer(buying, selling),
                              std::runtime_error);
        }

        SECTION("empty parent")
        {
            SECTION("no offers")
            {
                testBestOffer(buying, selling, {}, {{}}, mode);
            }

            SECTION("two offers")
            {
                SECTION("same assets")
                {
                    SECTION("same price")
                    {
                        testBestOffer(
                            buying, selling,
                            {{1, buying, selling, Price{1, 1}, 1}},
                            {{{{a1, 1}, {buying, selling, Price{1, 1}, 1}},
                              {{a1, 2}, {buying, selling, Price{1, 1}, 1}}}},
                            mode);
                    }

                    SECTION("different price")
                    {
                        testBestOffer(
                            buying, selling,
                            {{2, buying, selling, Price{1, 1}, 1}},
                            {{{{a1, 1}, {buying, selling, Price{2, 1}, 1}},
                              {{a1, 2}, {buying, selling, Price{1, 1}, 1}}}},
                            mode);
                        testBestOffer(
                            buying, selling,
                            {{1, buying, selling, Price{1, 1}, 1}},
                            {{{{a1, 1}, {buying, selling, Price{1, 1}, 1}},
                              {{a1, 2}, {buying, selling, Price{2, 1}, 1}}}},
                            mode);
                    }
                }

                SECTION("different assets")
                {
                    testBestOffer(
                        buying, selling, {{1, buying, selling, Price{1, 1}, 1}},
                        {{{{a1, 1}, {buying, selling, Price{1, 1}, 1}},
                          {{a1, 2}, {selling, buying, Price{1, 1}, 1}}}},
                        mode);
                    testBestOffer(
                        buying, selling, {{2, buying, selling, Price{1, 1}, 1}},
                        {{{{a1, 1}, {selling, buying, Price{1, 1}, 1}},
                          {{a1, 2}, {buying, selling, Price{1, 1}, 1}}}},
                        mode);
                }
            }
        }

        SECTION("one offer in parent")
        {
            SECTION("erased in child")
            {
                testBestOffer(buying, selling, {},
                              {{{{a1, 1}, {buying, selling, Price{1, 1}, 1}}},
                               {{{a1, 1}, {buying, selling, Price{1, 1}, 0}}}},
                              mode);
            }

            SECTION("modified assets in child")
            {
                testBestOffer(buying, selling, {},
                              {{{{a1, 1}, {buying, selling, Price{1, 1}, 1}}},
                               {{{a1, 1}, {selling, buying, Price{1, 1}, 1}}}},
                              mode);
                testBestOffer(buying, selling,
                              {{1, buying, selling, Price{1, 1}, 1}},
                              {{{{a1, 1}, {selling, buying, Price{1, 1}, 1}}},
                               {{{a1, 1}, {buying, selling, Price{1, 1}, 1}}}},
                              mode);
            }

            SECTION("modified price and amount in child")
            {
                testBestOffer(buying, selling,
                              {{1, buying, selling, Price{2, 1}, 7}},
                              {{{{a1, 1}, {buying, selling, Price{1, 1}, 1}}},
                               {{{a1, 1}, {buying, selling, Price{2, 1}, 7}}}},
                              mode);
            }

            SECTION("other offer in child")
            {
                testBestOffer(buying, selling,
                              {{1, buying, selling, Price{1, 1}, 1}},
                              {{{{a1, 1}, {buying, selling, Price{1, 1}, 1}}},
                               {{{a1, 2}, {buying, selling, Price{1, 1}, 1}}}},
                              mode);
                testBestOffer(buying, selling,
                              {{1, buying, selling, Price{1, 1}, 1}},
                              {{{{a1, 2}, {buying, selling, Price{1, 1}, 1}}},
                               {{{a1, 1}, {buying, selling, Price{1, 1}, 1}}}},
                              mode);

                testBestOffer(buying, selling,
                              {{2, buying, selling, Price{1, 2}, 1}},
                              {{{{a1, 1}, {buying, selling, Price{1, 1}, 1}}},
                               {{{a1, 2}, {buying, selling, Price{1, 2}, 1}}}},
                              mode);
                testBestOffer(buying, selling,
                              {{2, buying, selling, Price{1, 2}, 1}},
                              {{{{a1, 2}, {buying, selling, Price{1, 2}, 1}}},
                               {{{a1, 1}, {buying, selling, Price{1, 1}, 1}}}},
                              mode);
            }
        }

        SECTION("two offers in parent")
        {
            SECTION("erased in child")
            {
                testBestOffer(buying, selling,
                              {{2, buying, selling, Price{1, 1}, 1}},
                              {{{{a1, 1}, {buying, selling, Price{1, 1}, 1}},
                                {{a1, 2}, {buying, selling, Price{1, 1}, 1}}},
                               {{{a1, 1}, {buying, selling, Price{1, 1}, 0}}}},
                              mode);
            }

            SECTION("modified assets in child")
            {
                testBestOffer(buying, selling,
                              {{2, buying, selling, Price{1, 1}, 1}},
                              {{{{a1, 1}, {buying, selling, Price{1, 1}, 1}},
                                {{a1, 2}, {buying, selling, Price{1, 1}, 1}}},
                               {{{a1, 1}, {selling, buying, Price{1, 1}, 0}}}},
                              mode);
            }
        }

        SECTION("load previously loaded offer and prefetch accounts/trustlines")
        {
            VirtualClock clock;
            auto cfg = getTestConfig(0, mode);
            TempReduceLimitsForTesting limitUpdater(100, 10);

            // Enough space to store an offer, 1002 accounts (to keep a missing
            // account from evicting a prefetched account), and 2000 trustlines
            cfg.ENTRY_CACHE_SIZE = 3003;
            auto app = createTestApplication(clock, cfg);

            auto& root = app->getLedgerTxnRoot();

            LedgerEntry offer;
            offer.data.type(OFFER);

            auto& oe = offer.data.offer();
            oe = LedgerTestUtils::generateValidOfferEntry();
            oe.offerID = 1;

            // The comments below are written assuming getMaxOffersToCross()
            //  is 1000
            int64_t numOffers = getMaxOffersToCross() + 2;
            auto accounts =
                LedgerTestUtils::generateValidAccountEntries(numOffers);
            // First create 1002 offers that have different accounts and
            // trustlines
            LedgerTxn ltx1(root);
            for (int i = 0; i < numOffers; ++i)
            {
                oe.sellerID = accounts[i].accountID;
                ltx1.create(offer);
                ++oe.offerID;
            }

            // Commit into the database since this is a child of root
            ltx1.commit();

            // Derive from this LedgerTxn from here on out so the offers loaded
            // in root don't get cleared
            LedgerTxn ltx2(root);
            {
                LedgerTxn ltx3(ltx2);
                // Load all offers into best offers by requesting an offerID
                // that is greater than any we created above. Since an offer
                // will not be found, all offers will be loaded into bestOffers,
                // but mEntryCache will remain empty.
                ltx3.getBestOffer(oe.buying, oe.selling, {oe.price, numOffers});
            }

            auto preLoadPrefetchHitRate = root.getPrefetchHitRate();
            REQUIRE(preLoadPrefetchHitRate == 0);

            // This should lead to prefetching even though the offers were
            // already loaded. The offersIDs are in the range [1, 1002]. Verify
            // that the prefetching worked by checking the prefetch hit rate
            // after loading the accounts. mEntryCache should be empty prior to
            // this getBestOffer call, so no evictions should happen.

            auto loadOfferAndPrefetch = [&](int64_t offerID) {
                ltx2.getBestOffer(oe.buying, oe.selling, {oe.price, offerID});

                for (auto const& account : accounts)
                {
                    loadAccount(ltx2, account.accountID);
                }

                // Prefetch doesn't work in in-memory mode, but this is for
                // testing only so we only care about accuracy
                if (mode != Config::TESTDB_IN_MEMORY)
                {
                    // Note that we can't prefetch for more than 1000 offers
                    double expectedPrefetchHitRate =
                        std::min(numOffers - offerID,
                                 static_cast<int64_t>(getMaxOffersToCross())) /
                        static_cast<double>(accounts.size());
                    REQUIRE(fabs(expectedPrefetchHitRate -
                                 ltx2.getPrefetchHitRate()) < .000001);
                    REQUIRE(preLoadPrefetchHitRate < ltx2.getPrefetchHitRate());
                }
            };

            SECTION("prefetch for all worse remaining offers")
            {
                // There are 1000 better offers than offerID 2
                loadOfferAndPrefetch(numOffers - getMaxOffersToCross());
            }
            SECTION("prefetch for the next MAX_OFFERS_TO_CROSS offers")
            {
                // There are 1001 better offers than offerID 1. Should still
                // only prefetch for 1000
                loadOfferAndPrefetch(numOffers - getMaxOffersToCross() - 1);
            }
            SECTION("prefetch less than MAX_OFFERS_TO_CROSS offers")
            {
                loadOfferAndPrefetch(numOffers / 2);
            }
        }
    };

    SECTION("bucketlist")
    {
        runTest(Config::TESTDB_BUCKET_DB_PERSISTENT);
    }

    // This mode is only used in testing, but we should still make sure it works
    // for other tests that leverage it
    SECTION("in-memory")
    {
        runTest(Config::TESTDB_IN_MEMORY);
    }

#ifdef USE_POSTGRES
    SECTION("postgresql")
    {
        runTest(Config::TESTDB_POSTGRESQL);
    }
#endif
}

static void
testOffersByAccountAndAsset(
    AbstractLedgerTxnParent& ltxParent, AccountID const& accountID,
    Asset const& asset,
    std::vector<std::tuple<int64_t, Asset, Asset, int64_t>> const& expected,
    std::vector<std::map<std::pair<AccountID, int64_t>,
                         std::tuple<Asset, Asset, int64_t>>>::const_iterator
        begin,
    std::vector<
        std::map<std::pair<AccountID, int64_t>,
                 std::tuple<Asset, Asset, int64_t>>>::const_iterator const& end)
{
    REQUIRE(begin != end);
    LedgerTxn ltx(ltxParent);
    applyLedgerTxnUpdates(ltx, *begin);

    if (++begin != end)
    {
        testOffersByAccountAndAsset(ltx, accountID, asset, expected, begin,
                                    end);
    }
    else
    {
        auto offers = ltx.loadOffersByAccountAndAsset(accountID, asset);
        REQUIRE(expected.size() == offers.size());
        auto expected2 = expected;

        for (auto& curoff : offers)
        {
            auto const& oe = curoff.current().data.offer();
            auto expo =
                std::make_tuple(oe.offerID, oe.buying, oe.selling, oe.amount);
            auto it = std::find(expected2.begin(), expected2.end(), expo);
            REQUIRE(it != expected2.end());
            expected2.erase(it);
        }
    }
}

static void
testOffersByAccountAndAsset(
    AccountID const& accountID, Asset const& asset,
    std::vector<std::tuple<int64_t, Asset, Asset, int64_t>> const& expected,
    std::vector<std::map<std::pair<AccountID, int64_t>,
                         std::tuple<Asset, Asset, int64_t>>>
        updates)
{
    REQUIRE(!updates.empty());

    auto testAtRoot = [&](Application& app) {
        {
            LedgerTxn ltx1(app.getLedgerTxnRoot());
            applyLedgerTxnUpdates(ltx1, *updates.cbegin());
            ltx1.commit();
        }
        testOffersByAccountAndAsset(app.getLedgerTxnRoot(), accountID, asset,
                                    expected, ++updates.cbegin(),
                                    updates.cend());
    };

    // first changes are in LedgerTxnRoot with cache
    if (updates.size() > 1)
    {
        VirtualClock clock;
        auto app = createTestApplication(clock, getTestConfig());

        testAtRoot(*app);
    }

    // first changes are in LedgerTxnRoot without cache
    if (updates.size() > 1)
    {
        VirtualClock clock;
        auto cfg = getTestConfig();
        cfg.ENTRY_CACHE_SIZE = 0;
        auto app = createTestApplication(clock, cfg);

        testAtRoot(*app);
    }

    // first changes are in child of LedgerTxnRoot
    {
        VirtualClock clock;
        auto app = createTestApplication(clock, getTestConfig());

        testOffersByAccountAndAsset(app->getLedgerTxnRoot(), accountID, asset,
                                    expected, updates.cbegin(), updates.cend());
    }
}

TEST_CASE("LedgerTxn loadOffersByAccountAndAsset", "[ledgertxn]")
{
    auto a1 = LedgerTestUtils::generateValidAccountEntry().accountID;
    auto a2 = LedgerTestUtils::generateValidAccountEntry().accountID;

    Asset native(ASSET_TYPE_NATIVE);
    Asset buying = LedgerTestUtils::generateValidOfferEntry().buying;
    Asset selling = LedgerTestUtils::generateValidOfferEntry().selling;
    REQUIRE(buying.type() != ASSET_TYPE_NATIVE);
    REQUIRE(selling.type() != ASSET_TYPE_NATIVE);
    REQUIRE(!(buying == selling));

    SECTION("fails with children")
    {
        VirtualClock clock;
        auto app = createTestApplication(clock, getTestConfig());

        LedgerTxn ltx1(app->getLedgerTxnRoot());
        LedgerTxn ltx2(ltx1);
        REQUIRE_THROWS_AS(ltx1.loadOffersByAccountAndAsset(a1, buying),
                          std::runtime_error);
    }

    SECTION("fails if sealed")
    {
        VirtualClock clock;
        auto app = createTestApplication(clock, getTestConfig());

        LedgerTxn ltx1(app->getLedgerTxnRoot());
        ltx1.getDelta();
        REQUIRE_THROWS_AS(ltx1.loadOffersByAccountAndAsset(a1, buying),
                          std::runtime_error);
    }

    SECTION("empty parent")
    {
        SECTION("no offers")
        {
            testOffersByAccountAndAsset(a1, buying, {}, {{}});
        }

        SECTION("two offers")
        {
            testOffersByAccountAndAsset(
                a1, buying, {{1, buying, native, 1}, {2, buying, native, 1}},
                {{{{a1, 1}, {buying, native, 1}},
                  {{a1, 2}, {buying, native, 1}}}});
        }
    }

    SECTION("one offer in parent")
    {
        SECTION("erased in child")
        {
            testOffersByAccountAndAsset(a1, buying, {},
                                        {{{{a1, 1}, {buying, native, 1}}},
                                         {{{a1, 1}, {buying, native, 0}}}});
            testOffersByAccountAndAsset(a1, buying, {},
                                        {{{{a1, 1}, {native, buying, 1}}},
                                         {{{a1, 1}, {native, buying, 0}}}});
        }

        SECTION("modified assets in child")
        {
            testOffersByAccountAndAsset(a1, buying, {},
                                        {{{{a1, 1}, {buying, native, 1}}},
                                         {{{a1, 1}, {selling, native, 1}}}});
            testOffersByAccountAndAsset(a1, buying, {{1, buying, native, 1}},
                                        {{{{a1, 1}, {selling, native, 1}}},
                                         {{{a1, 1}, {buying, native, 1}}}});

            testOffersByAccountAndAsset(a1, buying, {},
                                        {{{{a1, 1}, {native, buying, 1}}},
                                         {{{a1, 1}, {native, selling, 1}}}});
            testOffersByAccountAndAsset(a1, buying, {{1, native, buying, 1}},
                                        {{{{a1, 1}, {native, selling, 1}}},
                                         {{{a1, 1}, {native, buying, 1}}}});

            testOffersByAccountAndAsset(a1, buying, {{1, native, buying, 1}},
                                        {{{{a1, 1}, {buying, native, 1}}},
                                         {{{a1, 1}, {native, buying, 1}}}});
            testOffersByAccountAndAsset(a1, buying, {{1, buying, native, 1}},
                                        {{{{a1, 1}, {native, buying, 1}}},
                                         {{{a1, 1}, {buying, native, 1}}}});
        }

        SECTION("modified amount in child")
        {
            testOffersByAccountAndAsset(a1, buying, {{1, buying, native, 7}},
                                        {{{{a1, 1}, {buying, native, 1}}},
                                         {{{a1, 1}, {buying, native, 7}}}});
        }

        SECTION("other offer in child")
        {
            testOffersByAccountAndAsset(
                a1, buying, {{1, buying, native, 1}, {2, buying, native, 1}},
                {{{{a1, 1}, {buying, native, 1}}},
                 {{{a1, 2}, {buying, native, 1}}}});
        }
    }

    SECTION("two offers in parent")
    {
        testOffersByAccountAndAsset(
            a1, buying, {{1, buying, native, 1}, {2, buying, native, 1}},
            {{{{a1, 1}, {buying, native, 1}}, {{a1, 2}, {buying, native, 1}}},
             {}});
        testOffersByAccountAndAsset(
            a1, buying, {{1, buying, native, 1}, {2, native, buying, 1}},
            {{{{a1, 1}, {buying, native, 1}}, {{a1, 2}, {native, buying, 1}}},
             {}});
    }
}

TEST_CASE("LedgerTxn unsealHeader", "[ledgertxn]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());

    auto doNothing = [](LedgerHeader&) {};

    SECTION("fails if not sealed")
    {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        REQUIRE_THROWS_AS(ltx.unsealHeader(doNothing), std::runtime_error);
    }

    SECTION("fails if header is active")
    {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        std::vector<LedgerEntry> init, live;
        std::vector<LedgerKey> dead;
        ltx.getAllEntries(init, live, dead);
        ltx.unsealHeader([&ltx, &doNothing](LedgerHeader&) {
            REQUIRE_THROWS_AS(ltx.unsealHeader(doNothing), std::runtime_error);
        });
    }

    SECTION("deactivates header on completion")
    {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        std::vector<LedgerEntry> init, live;
        std::vector<LedgerKey> dead;
        ltx.getAllEntries(init, live, dead);
        REQUIRE_NOTHROW(ltx.unsealHeader(doNothing));
        REQUIRE_NOTHROW(ltx.unsealHeader(doNothing));
    }
}

TEST_CASE("LedgerTxnEntry and LedgerTxnHeader move assignment", "[ledgertxn]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());

    auto& root = app->getLedgerTxnRoot();
    auto lh = root.getHeader();

    LedgerEntry le1 = LedgerTestUtils::generateValidLedgerEntryWithExclusions(
        {CONFIG_SETTING});
    LedgerKey key1 = LedgerEntryKey(le1);
    LedgerEntry le2 = LedgerTestUtils::generateValidLedgerEntryWithExclusions(
        {CONFIG_SETTING});
    LedgerKey key2 = LedgerEntryKey(le2);

    SECTION("assign self")
    {
        SECTION("entry")
        {
            LedgerTxn ltx(root, false);
            auto entry1 = ltx.create(le1);
            // Avoid warning for explicit move-to-self
            LedgerTxnEntry& entryRef = entry1;
            entry1 = std::move(entryRef);
            REQUIRE(entry1.current() == le1);
            REQUIRE_THROWS_AS(ltx.load(key1), std::runtime_error);
            REQUIRE_THROWS_AS(ltx.loadWithoutRecord(key1), std::runtime_error);
        }

        SECTION("const entry")
        {
            LedgerTxn ltx(root, false);
            ltx.create(le1);
            auto entry1 = ltx.loadWithoutRecord(key1);
            // Avoid warning for explicit move-to-self
            ConstLedgerTxnEntry& entryRef = entry1;
            entry1 = std::move(entryRef);
            REQUIRE(entry1.current() == le1);
            REQUIRE_THROWS_AS(ltx.load(key1), std::runtime_error);
            REQUIRE_THROWS_AS(ltx.loadWithoutRecord(key1), std::runtime_error);
        }

        SECTION("header")
        {
            LedgerTxn ltx(root, false);
            auto header = ltx.loadHeader();
            // Avoid warning for explicit move-to-self
            LedgerTxnHeader& headerRef = header;
            header = std::move(headerRef);
            REQUIRE(header.current() == lh);
            REQUIRE_THROWS_AS(ltx.loadHeader(), std::runtime_error);
        }
    }

    SECTION("assign other")
    {
        SECTION("entry")
        {
            LedgerTxn ltx(root, false);
            auto entry1 = ltx.create(le1);
            auto entry2 = ltx.create(le2);
            entry1 = std::move(entry2);
            REQUIRE(entry1.current() == le2);
            REQUIRE_THROWS_AS(ltx.load(key2), std::runtime_error);
            REQUIRE(ltx.load(key1).current() == le1);
            REQUIRE(ltx.loadWithoutRecord(key1).current() == le1);
        }

        SECTION("const entry")
        {
            LedgerTxn ltx(root, false);
            ltx.create(le1);
            ltx.create(le2);
            auto entry1 = ltx.loadWithoutRecord(key1);
            auto entry2 = ltx.loadWithoutRecord(key2);
            entry1 = std::move(entry2);
            REQUIRE(entry1.current() == le2);
            REQUIRE_THROWS_AS(ltx.load(key2), std::runtime_error);
            REQUIRE(ltx.load(key1).current() == le1);
            REQUIRE(ltx.loadWithoutRecord(key1).current() == le1);
        }

        SECTION("header")
        {
            LedgerTxn ltx(root, false);
            auto header1 = ltx.loadHeader();
            LedgerTxnHeader header2 = std::move(header1);
            REQUIRE(header2.current() == lh);
            REQUIRE_THROWS_AS(ltx.loadHeader(), std::runtime_error);
        }
    }
}

TEST_CASE("LedgerTxnRoot prefetch classic entries", "[ledgertxn]")
{
    auto runTest = [&](Config cfg) {
        VirtualClock clock;
        cfg.ENTRY_CACHE_SIZE = 1000;
        cfg.PREFETCH_BATCH_SIZE = cfg.ENTRY_CACHE_SIZE / 10;

        UnorderedSet<LedgerKey> keysToPrefetch;
        auto app = createTestApplication(clock, cfg);
        auto& root = app->getLedgerTxnRoot();

        auto entries = LedgerTestUtils::generateValidUniqueLedgerEntries(
            cfg.ENTRY_CACHE_SIZE + 1);
        std::set<LedgerEntry> entrySet;
        LedgerTxn ltx(root);
        for (auto e : entries)
        {
            ltx.createWithoutLoading(e);
            keysToPrefetch.emplace(LedgerEntryKey(e));

            // Insert entry into set with correct lastModifiedLedgerSeq value so
            // we can check prefetch results later
            e.lastModifiedLedgerSeq = 1;
            entrySet.emplace(e);
        }
        if (!cfg.MODE_USES_IN_MEMORY_LEDGER)
        {
            std::vector<LedgerEntry> ledgerVect{entrySet.begin(),
                                                entrySet.end()};
            LedgerHeader lh;
            lh.ledgerVersion = app->getLedgerManager()
                                   .getLastClosedLedgerHeader()
                                   .header.ledgerVersion;
            lh.ledgerSeq = 2;
            BucketTestUtils::addLiveBatchAndUpdateSnapshot(*app, lh, {},
                                                           ledgerVect, {});
        }
        ltx.commit();

        SECTION("prefetch normally")
        {
            LedgerTxn ltx2(root);
            UnorderedSet<LedgerKey> smallSet;
            for (auto const& k : keysToPrefetch)
            {
                smallSet.emplace(k);
                if (smallSet.size() > (cfg.ENTRY_CACHE_SIZE / 3))
                {
                    break;
                }
            }

            REQUIRE(root.prefetchClassic(smallSet) == smallSet.size());

            // Check that prefetch results are actually correct
            for (auto const& k : smallSet)
            {
                auto txle = ltx2.load(k);
                REQUIRE(txle);
                REQUIRE(entrySet.find(txle.current()) != entrySet.end());
            }

            // 100% hit rate but make it floating point
            REQUIRE(fabs(ltx2.getPrefetchHitRate() - 1.0f) <
                    std::numeric_limits<float>::epsilon());
            ltx2.commit();
        }
        SECTION("prefetch more than ENTRY_CACHE_SIZE entries")
        {
            LedgerTxn ltx2(root);
            REQUIRE(root.prefetchClassic(keysToPrefetch) ==
                    keysToPrefetch.size());
            ltx2.commit();
        }
    };

    SECTION("bucketlist")
    {
        runTest(getTestConfig(Config::TESTDB_BUCKET_DB_PERSISTENT));
    }

#ifdef USE_POSTGRES
    SECTION("postgresql")
    {
        runTest(getTestConfig(0, Config::TESTDB_POSTGRESQL));
    }
#endif
}

TEST_CASE("Create performance benchmark", "[!hide][createbench]")
{
    auto runTest = [&](Config::TestDbMode mode, bool loading) {
        VirtualClock clock;
        Config cfg(getTestConfig(0, mode));
        Application::pointer app = createTestApplication(clock, cfg);

        size_t n = 0xffff, batch = 0xfff;

        std::vector<LedgerEntry> entries;
        for (auto i = 0; i < 10; ++i)
        {
            // First add some bulking entries so we're not using a
            // totally empty database.
            entries =
                LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                    {OFFER}, n);
            LedgerTxn ltx(app->getLedgerTxnRoot());
            for (auto e : entries)
            {
                ltx.createWithoutLoading(e);
            }
            ltx.commit();
        }

        // Then do some precise timed creates.
        entries = LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
            {OFFER}, n);
        auto& m =
            app->getMetrics().NewMeter({"ledger", "create", "commit"}, "entry");
        while (!entries.empty())
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            for (size_t i = 0; !entries.empty() && i < batch; ++i)
            {
                if (loading)
                {
                    ltx.create(entries.back());
                }
                else
                {
                    ltx.createWithoutLoading(entries.back());
                }
                entries.pop_back();
            }
            ltx.commit();
            m.Mark(batch);
            CLOG_INFO(Ledger, "benchmark create rate: {} entries/sec {}",
                      m.mean_rate(), (loading ? "(loading)" : "(non-loading)"));
        }
    };

    SECTION("sqlite")
    {
        runTest(Config::TESTDB_BUCKET_DB_PERSISTENT, true);
        runTest(Config::TESTDB_BUCKET_DB_PERSISTENT, false);
    }

#ifdef USE_POSTGRES
    SECTION("postgresql")
    {
        runTest(Config::TESTDB_POSTGRESQL, true);
        runTest(Config::TESTDB_POSTGRESQL, false);
    }
#endif
}

TEST_CASE("Erase performance benchmark", "[!hide][erasebench]")
{
    auto runTest = [&](Config::TestDbMode mode, bool loading) {
        VirtualClock clock;
        Config cfg(getTestConfig(0, mode));
        Application::pointer app = createTestApplication(clock, cfg);

        size_t n = 0xffff, batch = 0xfff;

        std::vector<LedgerEntry> entries;
        for (auto i = 0; i < 10; ++i)
        {
            // First add some bulking entries so we're not using a
            // totally empty database.
            entries =
                LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                    {OFFER}, n);
            LedgerTxn ltx(app->getLedgerTxnRoot());
            for (auto e : entries)
            {
                ltx.createWithoutLoading(e);
            }
            ltx.commit();
        }

        // Then do some precise timed erases.
        auto& m =
            app->getMetrics().NewMeter({"ledger", "erase", "commit"}, "entry");
        while (!entries.empty())
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            for (size_t i = 0; !entries.empty() && i < batch; ++i)
            {
                if (loading)
                {
                    ltx.erase(LedgerEntryKey(entries.back()));
                }
                else
                {
                    ltx.eraseWithoutLoading(LedgerEntryKey(entries.back()));
                }
                entries.pop_back();
            }
            ltx.commit();
            m.Mark(batch);
            CLOG_INFO(Ledger, "benchmark erase rate: {} entries/sec {}",
                      m.mean_rate(), (loading ? "(loading)" : "(non-loading)"));
        }
    };

    SECTION("sqlite")
    {
        runTest(Config::TESTDB_BUCKET_DB_PERSISTENT, true);
        runTest(Config::TESTDB_BUCKET_DB_PERSISTENT, false);
    }

#ifdef USE_POSTGRES
    SECTION("postgresql")
    {
        runTest(Config::TESTDB_POSTGRESQL, true);
        runTest(Config::TESTDB_POSTGRESQL, false);
    }
#endif
}

TEST_CASE("LedgerTxnRoot prefetch soroban entries", "[ledgertxn]")
{
    Config cfg = getTestConfig();
    cfg.ENTRY_CACHE_SIZE = 10;

    // Test setup.
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);
    UnorderedSet<LedgerKey> keysToPrefetch;
    auto& root = app->getLedgerTxnRoot();
    LedgerTxn ltx(root);

    auto lkMeterCold = std::make_unique<LedgerKeyMeter>();
    auto lkMeterHot = std::make_unique<LedgerKeyMeter>();

    auto contractDataEntry =
        LedgerTestUtils::generateValidLedgerEntryOfType(CONTRACT_DATA);
    contractDataEntry.lastModifiedLedgerSeq = 1;
    ltx.createWithoutLoading(contractDataEntry);

    auto classicEntry = LedgerTestUtils::generateValidLedgerEntryOfType(OFFER);
    classicEntry.lastModifiedLedgerSeq = 1;
    ltx.createWithoutLoading(classicEntry);

    LedgerEntry TTLEntry;
    TTLEntry.data.type(TTL);
    TTLEntry.data.ttl().keyHash = getTTLKey(contractDataEntry).ttl().keyHash;
    TTLEntry.data.ttl().liveUntilLedgerSeq =
        contractDataEntry.lastModifiedLedgerSeq + 1;

    auto deadEntry =
        LedgerTestUtils::generateValidLedgerEntryOfType(CONTRACT_DATA);
    auto deadKey = LedgerEntryKey(deadEntry);
    ltx.eraseWithoutLoading(deadKey);

    // Insert all entries into the database.
    std::vector<LedgerEntry> ledgerVect{classicEntry, contractDataEntry,
                                        TTLEntry};
    std::vector<LedgerKey> deadKeyVect{deadKey};
    LedgerHeader lh;
    lh.ledgerVersion = app->getLedgerManager()
                           .getLastClosedLedgerHeader()
                           .header.ledgerVersion;
    lh.ledgerSeq = 2;
    BucketTestUtils::addLiveBatchAndUpdateSnapshot(*app, lh, {}, ledgerVect,
                                                   deadKeyVect);
    ltx.commit();

    auto addTxn = [&](bool enoughQuota, std::vector<LedgerEntry> entries,
                      std::vector<LedgerKey> deadKeys = {}) {
        SorobanResources resources;

        for (auto const& e : entries)
        {
            auto k = LedgerEntryKey(e);
            keysToPrefetch.emplace(k);
            if (k.type() != TTL)
            {
                resources.readBytes += xdr::xdr_size(e);
                // Randomly add the key to either the read or write set.
                if (stellar::rand_flip())
                {
                    resources.footprint.readOnly.emplace_back(k);
                }
                else
                {
                    resources.footprint.readWrite.emplace_back(k);
                }
            }
        }
        if (!enoughQuota)
        {
            resources.readBytes -= 1;
        }

        for (auto& k : deadKeys)
        {
            resources.footprint.readOnly.emplace_back(k);
        }

        lkMeterHot->addTxn(resources);
        lkMeterCold->addTxn(resources);
    };

    auto checkPrefetch = [&](std::set<LedgerKey> const& expectedSuccessKeys) {
        LedgerTxn ltx2(root);
        auto numLoadedCold =
            root.prefetchSoroban(keysToPrefetch, lkMeterCold.get());
        REQUIRE(numLoadedCold == expectedSuccessKeys.size());

        auto preLoadPrefetchHitRate = root.getPrefetchHitRate();
        REQUIRE(preLoadPrefetchHitRate == 0);
        for (auto const& k : expectedSuccessKeys)
        {
            ltx2.load(k);
        }

        auto numLoadedHot =
            root.prefetchSoroban(keysToPrefetch, lkMeterHot.get());
        REQUIRE(numLoadedHot == 0);
        // 100% hit rate but make it floating point
        REQUIRE(fabs(ltx2.getPrefetchHitRate() - 1.0f) <
                std::numeric_limits<float>::epsilon());
    };

    SECTION("all keys have quota")
    {
        auto tx1Entries =
            std::vector<LedgerEntry>{classicEntry, contractDataEntry, TTLEntry};
        auto tx2Entries =
            std::vector<LedgerEntry>{classicEntry, contractDataEntry, TTLEntry};
        addTxn(true /* enough quota */, tx1Entries);
        addTxn(false, tx2Entries);
        std::set<LedgerKey> expectedSuccessKeys;
        for (auto const& e : tx1Entries)
        {
            expectedSuccessKeys.emplace(LedgerEntryKey(e));
        }
        checkPrefetch(expectedSuccessKeys);
    }

    SECTION("dead keys don't affect quota")
    {
        auto deadKeys = std::vector<LedgerKey>{deadKey};
        auto tx1Entries =
            std::vector<LedgerEntry>{classicEntry, contractDataEntry, TTLEntry};
        addTxn(true /* enough quota */, tx1Entries, deadKeys);
        std::set<LedgerKey> expectedSuccessKeys;
        for (auto const& e : tx1Entries)
        {
            expectedSuccessKeys.emplace(LedgerEntryKey(e));
        }
        checkPrefetch(expectedSuccessKeys);
    }

    SECTION("don't load entries without quota")
    {
        auto tx1Entries =
            std::vector<LedgerEntry>{classicEntry, contractDataEntry, TTLEntry};
        addTxn(false /* enough quota */, tx1Entries);
        std::set<LedgerKey> expectedSuccessKeys;
        expectedSuccessKeys.emplace(LedgerEntryKey(TTLEntry));
        // Keys are loaded according to the iteration order of the set.
        // Whichever entry is loaded first will succeed. The other will fail.
        // Classic entries are strictly less than soroban entries, so we expect
        // classicEntry will be loaded and contractDataEntry will not.
        expectedSuccessKeys.emplace(LedgerEntryKey(classicEntry));
        checkPrefetch(expectedSuccessKeys);
    }
    SECTION("non existent entries should not affect quota")
    {
        // Prefetch an entry which has not been added to the database.
        auto nonExistentEntry =
            LedgerTestUtils::generateValidLedgerEntryOfType(CONTRACT_DATA);
        // Both should succeed, as the non-existent entry is not metered and
        // should result in a null entry in the cache.
        addTxn(false /* not enough */, {contractDataEntry, nonExistentEntry});
        std::set<LedgerKey> expectedSuccessKeys{
            LedgerEntryKey(contractDataEntry),
            LedgerEntryKey(nonExistentEntry)};
        checkPrefetch(expectedSuccessKeys);
    }
}

TEST_CASE("LedgerKeyMeter tests")
{
    LedgerKeyMeter lkMeter{};
    auto entry = LedgerTestUtils::generateValidLedgerEntryWithTypes(
        {CONTRACT_CODE, CONTRACT_DATA}, 1);
    auto key = LedgerEntryKey(entry);
    auto entrySize = xdr::xdr_size(entry);
    UnorderedSet<LedgerKey> keys;
    keys.emplace(key);
    SorobanResources resources;
    resources.readBytes = entrySize;
    resources.footprint.readOnly = {key};
    lkMeter.addTxn(resources);

    REQUIRE(lkMeter.canLoad(key, entrySize));
    REQUIRE(!lkMeter.canLoad(key, entrySize + 1));
    REQUIRE(lkMeter.canLoad(key, entrySize - 1));
    REQUIRE(lkMeter.canLoad(key, 0));

    // Adding another txn with less readQuota should not change the
    // fact the key can be loaded.
    resources.readBytes = 0;
    resources.footprint.readOnly = {key};
    lkMeter.addTxn(resources);
    REQUIRE(lkMeter.canLoad(key, entrySize));
    // Consume size(entry) of the read quota of each transaction which
    // contains key.
    lkMeter.updateReadQuotasForKey(key, entrySize);
    // After updating, the read quota for the key should be zero.
    REQUIRE(!lkMeter.canLoad(key, 1));
    // Add another transaction with the same key and 2 * entrySize read quota.
    resources.readBytes = 2 * entrySize;
    resources.footprint.readOnly = {key};
    lkMeter.addTxn(resources);
    REQUIRE(lkMeter.canLoad(key, 2 * entrySize));
    lkMeter.updateReadQuotasForKey(key, entrySize);
    // After updating, the read quota should be equal the entry size (as
    // the original quota was double)
    REQUIRE(lkMeter.canLoad(key, entrySize));
    // TTL keys are not part of the footprint and therefore not metered (i.e.
    // always loadable).
    auto ttlKey = getTTLKey(key);
    REQUIRE(lkMeter.canLoad(ttlKey, std::numeric_limits<uint32_t>::max()));
    // The ttlKey is not metered, so this should not have any effect.
    lkMeter.updateReadQuotasForKey(ttlKey,
                                   std::numeric_limits<uint32_t>::max());
    REQUIRE(lkMeter.canLoad(ttlKey, std::numeric_limits<std::uint32_t>::max()));
}

TEST_CASE("Load best offers benchmark", "[!hide][bestoffersbench]")
{
    auto getTimeScope = [](Application& app, std::string const& phase) {
        return app.getMetrics()
            .NewTimer({"bestoffers", "benchmark", phase})
            .TimeScope();
    };

    auto getTimeSpent = [](Application& app, std::string const& phase) {
        auto time =
            app.getMetrics().NewTimer({"bestoffers", "benchmark", phase}).sum();
        return phase + ": " + std::to_string(time) + " ms";
    };

    auto generateAssets = [](size_t numAssets, size_t numIssuers) {
        CLOG_WARNING(Ledger, "Generating issuers");
        REQUIRE(numIssuers >= 1);
        std::vector<AccountID> issuers;
        for (size_t i = 1; i < numIssuers; ++i)
        {
            issuers.emplace_back(autocheck::generator<AccountID>()(5));
        }

        CLOG_WARNING(Ledger, "Generating assets");
        REQUIRE(numAssets >= 2);
        std::vector<Asset> assets;
        assets.emplace_back(ASSET_TYPE_NATIVE);
        for (size_t i = 1; i < numAssets; ++i)
        {
            size_t issuerIndex =
                autocheck::generator<size_t>()(issuers.size() - 1);
            REQUIRE(issuerIndex < issuers.size());

            Asset a(ASSET_TYPE_CREDIT_ALPHANUM4);
            strToAssetCode(a.alphaNum4().assetCode, "A" + std::to_string(i));
            a.alphaNum4().issuer = issuers[issuerIndex];
            assets.emplace_back(a);
        }
        return assets;
    };

    auto selectAssetPair = [](std::vector<Asset> const& assets) {
        REQUIRE(assets.size() >= 2);
        size_t maxIndex = assets.size() - 1;

        size_t firstIndex = autocheck::generator<size_t>()(maxIndex);
        size_t secondIndex;
        do
        {
            secondIndex = autocheck::generator<size_t>()(maxIndex);
        } while (firstIndex == secondIndex);

        return std::make_pair(assets[firstIndex], assets[secondIndex]);
    };

    auto generateEntries = [&](size_t numOffers,
                               std::vector<Asset> const& assets) {
        CLOG_WARNING(Ledger, "Generating offers");
        std::vector<LedgerEntry> offers;
        offers.reserve(numOffers);
        for (size_t i = 0; i < numOffers; ++i)
        {
            LedgerEntry le;
            le.data.type(OFFER);
            le.lastModifiedLedgerSeq = 1;
            le.data.offer() = LedgerTestUtils::generateValidOfferEntry();

            auto& oe = le.data.offer();
            auto assetPair = selectAssetPair(assets);
            oe.selling = assetPair.first;
            oe.buying = assetPair.second;

            offers.emplace_back(le);
        }
        return offers;
    };

    auto writeEntries = [&](Application& app,
                            std::vector<LedgerEntry> const& offers) {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        {
            CLOG_WARNING(Ledger, "Creating offers");
            auto timer = getTimeScope(app, "create");
            for (auto const& le : offers)
            {
                ltx.create(le);
            }
        }

        {
            CLOG_WARNING(Ledger, "Writing offers");
            auto timer = getTimeScope(app, "write");
            ltx.commit();
        }
    };

    auto loadBestOffers = [&](Application& app, Asset const& buying,
                              Asset const& selling,
                              std::vector<LedgerEntry> const& sortedOffers) {
        auto timer = getTimeScope(app, "load");

        size_t numOffers = 0;
        LedgerTxn ltx(app.getLedgerTxnRoot());
        while (auto le = ltx.loadBestOffer(buying, selling))
        {
            REQUIRE(le.current() == sortedOffers[numOffers]);
            ++numOffers;
            le.erase();
        }
    };

    auto runTest = [&](Config::TestDbMode mode, size_t numAssets,
                       size_t numIssuers, size_t numOffers) {
        VirtualClock clock;
        Config cfg(getTestConfig(0, mode));
        cfg.ENTRY_CACHE_SIZE = 100000;
        Application::pointer app = createTestApplication(clock, cfg);

        CLOG_WARNING(
            Ledger,
            "Generating {} offers buying and selling {} assets with {} issuers",
            numOffers, numAssets, numIssuers);
        auto assets = generateAssets(numAssets, numIssuers);
        auto offers = generateEntries(numOffers, assets);

        std::map<std::pair<Asset, Asset>, std::vector<LedgerEntry>>
            sortedOffers;
        for (auto const& offer : offers)
        {
            auto const& oe = offer.data.offer();
            auto& vec = sortedOffers[std::make_pair(oe.buying, oe.selling)];
            vec.emplace_back(offer);
        }
        for (auto& kv : sortedOffers)
        {
            std::sort(kv.second.begin(), kv.second.end(),
                      (bool (*)(LedgerEntry const&,
                                LedgerEntry const&))isBetterOffer);
        }

        writeEntries(*app, offers);

        CLOG_WARNING(Ledger, "Loading best offers");
        for (auto const& buying : assets)
        {
            for (auto const& selling : assets)
            {
                if (!(buying == selling))
                {
                    loadBestOffers(
                        *app, buying, selling,
                        sortedOffers[std::make_pair(buying, selling)]);
                }
            }
        }

        CLOG_WARNING(Ledger, "Done ({}, {}, {})", getTimeSpent(*app, "create"),
                     getTimeSpent(*app, "write"), getTimeSpent(*app, "load"));
    };

#ifdef USE_POSTGRES
    SECTION("postgres")
    {
        runTest(Config::TESTDB_POSTGRESQL, 10, 5, 25000);
    }
#endif

    SECTION("sqlite")
    {
        runTest(Config::TESTDB_BUCKET_DB_PERSISTENT, 10, 5, 25000);
    }
}

typedef UnorderedMap<AssetPair, std::vector<LedgerEntry>, AssetPairHash>
    OrderBook;
typedef UnorderedMap<
    AssetPair, std::map<OfferDescriptor, LedgerKey, IsBetterOfferComparator>,
    AssetPairHash>
    SortedOrderBook;

static void
checkOrderBook(LedgerTxn& ltx, OrderBook const& expected)
{
    SortedOrderBook sortedExpected;
    for (auto const& kv : expected)
    {
        auto& inner = sortedExpected[kv.first];
        for (auto const& le : kv.second)
        {
            auto const& oe = le.data.offer();
            inner.insert({{oe.price, oe.offerID}, LedgerEntryKey(le)});
        }
    }

    auto check = [](auto const& lhs, auto const& rhs) {
        for (auto const& kv : lhs)
        {
            auto iter = rhs.find(kv.first);
            if (kv.second.empty())
            {
                REQUIRE((iter == rhs.end() || iter->second.empty()));
            }
            else
            {
                REQUIRE((iter != rhs.end() && iter->second == kv.second));
            }
        }
    };

    auto ltxOb = ltx.getOrderBook();
    check(ltxOb, sortedExpected);
    check(sortedExpected, ltxOb);
}

static LedgerEntry
generateOfferWithSameAssets(LedgerEntry const& leBase)
{
    LedgerEntry le;
    le.data.type(OFFER);
    auto& oe = le.data.offer();
    oe = LedgerTestUtils::generateValidOfferEntry();
    oe.buying = leBase.data.offer().buying;
    oe.selling = leBase.data.offer().selling;
    return le;
}

static LedgerEntry
generateOfferWithSameKeyAndAssets(LedgerEntry const& leBase)
{
    LedgerEntry le = generateLedgerEntryWithSameKey(leBase);
    auto& oe = le.data.offer();
    oe.buying = leBase.data.offer().buying;
    oe.selling = leBase.data.offer().selling;
    return le;
}

static LedgerEntry
generateOfferWithSameKeyAndSwappedAssets(LedgerEntry const& leBase)
{
    LedgerEntry le = generateLedgerEntryWithSameKey(leBase);
    auto& oe = le.data.offer();
    oe.buying = leBase.data.offer().selling;
    oe.selling = leBase.data.offer().buying;
    return le;
}

TEST_CASE("LedgerTxn in memory order book", "[ledgertxn]")
{
    auto runTest = [&](Config::TestDbMode mode) {
        VirtualClock clock;
        auto app = createTestApplication(clock, getTestConfig(0, mode));

        SECTION("one offer, one asset pair")
        {
            LedgerEntry le1;
            le1.data.type(OFFER);
            le1.data.offer() = LedgerTestUtils::generateValidOfferEntry();
            LedgerEntry le2 = generateOfferWithSameKeyAndAssets(le1);
            AssetPair assets{le1.data.offer().buying, le1.data.offer().selling};

            LedgerTxn ltx(app->getLedgerTxnRoot());
            {
                auto lte = ltx.create(le1);
                checkOrderBook(ltx, {});
            }
            checkOrderBook(ltx, {{assets, {le1}}});

            {
                auto lte = ltx.load(LedgerEntryKey(le1));
                checkOrderBook(ltx, {});
            }
            checkOrderBook(ltx, {{assets, {le1}}});

            {
                auto lte = ltx.load(LedgerEntryKey(le1));
                lte.current() = le2;
                checkOrderBook(ltx, {});
            }
            checkOrderBook(ltx, {{assets, {le2}}});

            SECTION("erase without loading")
            {
                ltx.erase(LedgerEntryKey(le1));
                checkOrderBook(ltx, {});
            }
            SECTION("erase after loading")
            {
                ltx.load(LedgerEntryKey(le1)).erase();
                checkOrderBook(ltx, {});
            }
        }

        SECTION("two offers, one asset pair")
        {
            LedgerEntry le1a;
            le1a.data.type(OFFER);
            le1a.data.offer() = LedgerTestUtils::generateValidOfferEntry();
            LedgerEntry le1b = generateOfferWithSameKeyAndAssets(le1a);
            LedgerEntry le2a = generateOfferWithSameAssets(le1a);
            LedgerEntry le2b = generateOfferWithSameKeyAndAssets(le2a);
            AssetPair assets{le1a.data.offer().buying,
                             le1a.data.offer().selling};

            LedgerTxn ltx(app->getLedgerTxnRoot());
            {
                auto lte1 = ltx.create(le1a);
                auto lte2 = ltx.create(le2a);
                checkOrderBook(ltx, {});
            }
            checkOrderBook(ltx, {{assets, {le1a, le2a}}});

            {
                auto lte1 = ltx.load(LedgerEntryKey(le1a));
                checkOrderBook(ltx, {{assets, {le2a}}});
            }
            checkOrderBook(ltx, {{assets, {le1a, le2a}}});
            {
                auto lte2 = ltx.load(LedgerEntryKey(le2a));
                checkOrderBook(ltx, {{assets, {le1a}}});
            }
            checkOrderBook(ltx, {{assets, {le1a, le2a}}});

            {
                auto lte1 = ltx.load(LedgerEntryKey(le1a));
                lte1.current() = le1b;
                checkOrderBook(ltx, {{assets, {le2a}}});
            }
            checkOrderBook(ltx, {{assets, {le1b, le2a}}});
            {
                auto lte2 = ltx.load(LedgerEntryKey(le2a));
                lte2.current() = le2b;
                checkOrderBook(ltx, {{assets, {le1b}}});
            }
            checkOrderBook(ltx, {{assets, {le1b, le2b}}});

            {
                auto lte1 = ltx.load(LedgerEntryKey(le1b));
                auto lte2 = ltx.load(LedgerEntryKey(le2b));
                lte1.current() = le1a;
                lte2.current() = le2a;
                checkOrderBook(ltx, {});
            }
            checkOrderBook(ltx, {{assets, {le1a, le2a}}});

            SECTION("erase one at a time")
            {
                ltx.erase(LedgerEntryKey(le1a));
                checkOrderBook(ltx, {{assets, {le2a}}});
                ltx.erase(LedgerEntryKey(le2a));
                checkOrderBook(ltx, {});
            }
            SECTION("load then erase both")
            {
                auto lte1 = ltx.load(LedgerEntryKey(le1a));
                auto lte2 = ltx.load(LedgerEntryKey(le2a));
                lte1.erase();
                checkOrderBook(ltx, {});
                lte2.erase();
                checkOrderBook(ltx, {});
            }
        }

        SECTION("four offers, two asset pairs")
        {
            LedgerEntry le1a;
            le1a.data.type(OFFER);
            le1a.data.offer() = LedgerTestUtils::generateValidOfferEntry();
            LedgerEntry le1b = generateOfferWithSameKeyAndSwappedAssets(le1a);
            LedgerEntry le2a = generateOfferWithSameAssets(le1a);
            LedgerEntry le2b = generateOfferWithSameKeyAndSwappedAssets(le2a);
            LedgerEntry le3a = generateOfferWithSameAssets(le1a);
            LedgerEntry le3b = generateOfferWithSameKeyAndSwappedAssets(le3a);
            LedgerEntry le4a = generateOfferWithSameAssets(le1a);
            LedgerEntry le4b = generateOfferWithSameKeyAndSwappedAssets(le4a);
            AssetPair assets{le1a.data.offer().buying,
                             le1a.data.offer().selling};
            AssetPair swappedAssets{assets.selling, assets.buying};

            LedgerTxn ltx(app->getLedgerTxnRoot());
            {
                auto lte1 = ltx.create(le1a);
                auto lte2 = ltx.create(le2a);
                auto lte3 = ltx.create(le3a);
                auto lte4 = ltx.create(le4a);
                checkOrderBook(ltx, {});
            }
            checkOrderBook(ltx, {{assets, {le1a, le2a, le3a, le4a}}});

            {
                auto lte1 = ltx.load(LedgerEntryKey(le1a));
                lte1.current() = le1b;
                checkOrderBook(ltx, {{assets, {le2a, le3a, le4a}}});
            }
            checkOrderBook(
                ltx, {{assets, {le2a, le3a, le4a}}, {swappedAssets, {le1b}}});

            {
                auto lte2 = ltx.load(LedgerEntryKey(le2a));
                auto lte3 = ltx.load(LedgerEntryKey(le3a));
                lte2.current() = le2b;
                lte3.current() = le3b;
                checkOrderBook(ltx,
                               {{assets, {le4a}}, {swappedAssets, {le1b}}});
            }
            checkOrderBook(
                ltx, {{assets, {le4a}}, {swappedAssets, {le1b, le2b, le3b}}});

            {
                auto lte4 = ltx.load(LedgerEntryKey(le4a));
                lte4.current() = le4b;
                checkOrderBook(ltx, {{swappedAssets, {le1b, le2b, le3b}}});
            }
            checkOrderBook(ltx, {{swappedAssets, {le1b, le2b, le3b, le4b}}});
        }

        auto withoutLoadingTest = [&](bool isCreate) {
            LedgerEntry le1a;
            le1a.data.type(OFFER);
            le1a.data.offer() = LedgerTestUtils::generateValidOfferEntry();
            LedgerEntry le1b = generateLedgerEntryWithSameKey(le1a);
            AssetPair assetsA{le1a.data.offer().buying,
                              le1a.data.offer().selling};
            AssetPair assetsB{le1b.data.offer().buying,
                              le1b.data.offer().selling};

            LedgerTxn ltx(app->getLedgerTxnRoot());
            isCreate ? ltx.createWithoutLoading(le1a)
                     : ltx.updateWithoutLoading(le1a);
            checkOrderBook(ltx, {{assetsA, {le1a}}});
            isCreate ? ltx.createWithoutLoading(le1b)
                     : ltx.updateWithoutLoading(le1b);
            checkOrderBook(ltx, {{assetsB, {le1b}}});
        };

        SECTION("createWithoutLoading correctly modifies order book")
        {
            withoutLoadingTest(true);
        }

        SECTION("updateWithoutLoading correctly modifies order book")
        {
            withoutLoadingTest(false);
        }

        SECTION("eraseWithoutLoading correctly modifies order book")
        {
            LedgerEntry le1a;
            le1a.data.type(OFFER);
            le1a.data.offer() = LedgerTestUtils::generateValidOfferEntry();
            AssetPair assets{le1a.data.offer().buying,
                             le1a.data.offer().selling};

            {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                ltx.eraseWithoutLoading(LedgerEntryKey(le1a));
                checkOrderBook(ltx, {});
                ltx.create(le1a);
                checkOrderBook(ltx, {{assets, {le1a}}});
                ltx.eraseWithoutLoading(LedgerEntryKey(le1a));
                checkOrderBook(ltx, {});
            }

            {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                {
                    LedgerTxn ltxChild(ltx);
                    ltxChild.eraseWithoutLoading(LedgerEntryKey(le1a));
                    ltxChild.commit();
                }
                checkOrderBook(ltx, {});
            }
        }

        SECTION("deactivating ConstLedgerTxnEntry does not modify order book")
        {
            LedgerEntry le1a;
            le1a.data.type(OFFER);
            le1a.data.offer() = LedgerTestUtils::generateValidOfferEntry();
            AssetPair assets{le1a.data.offer().buying,
                             le1a.data.offer().selling};

            LedgerTxn ltx(app->getLedgerTxnRoot());
            {
                auto lte = ltx.loadWithoutRecord(LedgerEntryKey(le1a));
                checkOrderBook(ltx, {});
            }
            checkOrderBook(ltx, {});

            {
                auto lte = ltx.create(le1a);
                checkOrderBook(ltx, {});
            }
            checkOrderBook(ltx, {{assets, {le1a}}});

            {
                auto lte = ltx.loadWithoutRecord(LedgerEntryKey(le1a));
                checkOrderBook(ltx, {});
            }
            checkOrderBook(ltx, {{assets, {le1a}}});
        }

        SECTION("parent updates correctly on addChild")
        {
            OrderBook orderBook;
            LedgerTxn ltx(app->getLedgerTxnRoot());

            std::vector<LedgerTxnEntry> entries;
            for (size_t i = 0; i < 20; ++i)
            {
                LedgerEntry le;
                le.data.type(OFFER);
                auto& oe = le.data.offer();
                oe = LedgerTestUtils::generateValidOfferEntry();
                entries.emplace_back(ltx.create(le));

                AssetPair assets{oe.buying, oe.selling};
                orderBook[assets].emplace_back(le);
            }
            checkOrderBook(ltx, {});

            {
                LedgerTxn ltxChild(ltx);
                checkOrderBook(ltx, orderBook);
                checkOrderBook(ltxChild, {});

                OrderBook newOrderBook;
                OrderBook combinedOrderBook;

                size_t j = 0;
                for (auto& kv : orderBook)
                {
                    for (auto const& le : kv.second)
                    {
                        if (j % 3 == 0)
                        {
                            auto leNew = generateLedgerEntryWithSameKey(le);
                            ltxChild.load(LedgerEntryKey(le)).current() = leNew;

                            auto const& oe = leNew.data.offer();
                            AssetPair assets{oe.buying, oe.selling};
                            newOrderBook[assets].emplace_back(leNew);
                            combinedOrderBook[assets].emplace_back(leNew);
                        }
                        else if (j % 3 == 1)
                        {
                            auto const& oe = le.data.offer();
                            AssetPair assets{oe.buying, oe.selling};
                            combinedOrderBook[assets].emplace_back(le);
                        }
                        else if (j % 3 == 2)
                        {
                            ltxChild.erase(LedgerEntryKey(le));
                        }
                        ++j;
                    }
                }

                checkOrderBook(ltxChild, newOrderBook);
                checkOrderBook(ltx, orderBook);

                SECTION("parent updates correctly on commit")
                {
                    ltxChild.commit();
                    checkOrderBook(ltx, combinedOrderBook);
                }

                SECTION("parent does not update on rollback")
                {
                    ltxChild.rollback();
                    checkOrderBook(ltx, orderBook);
                }
            }
        }
    };

    SECTION("bucketlist")
    {
        runTest(Config::TESTDB_BUCKET_DB_PERSISTENT);
    }

    // This mode is just used for testing, but we should still make sure it
    // works
    SECTION("in-memory")
    {
        runTest(Config::TESTDB_IN_MEMORY);
    }

#ifdef USE_POSTGRES
    SECTION("postgresql")
    {
        runTest(Config::TESTDB_POSTGRESQL);
    }
#endif
}

TEST_CASE("Access deactivated entry", "[ledgertxn]")
{
    auto runTest = [&](Config::TestDbMode mode) {
        VirtualClock clock;
        auto app = createTestApplication(clock, getTestConfig(0, mode));

        LedgerEntry le1;
        le1.data.type(OFFER);
        le1.data.offer() = LedgerTestUtils::generateValidOfferEntry();

        LedgerKey lk1 = LedgerEntryKey(le1);

        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            ltx.create(le1);
            ltx.commit();
        }

        LedgerKey missingEntryKey = LedgerEntryKey(
            LedgerTestUtils::generateValidLedgerEntryWithExclusions(
                {CONFIG_SETTING}));

        LedgerTxn ltx1(app->getLedgerTxnRoot());

        SECTION("load")
        {
            auto entry = ltx1.load(lk1);
            REQUIRE(entry);

            auto missingEntry = ltx1.load(missingEntryKey);
            REQUIRE(!missingEntry);

            // this will deactivate entry
            LedgerTxn ltx2(ltx1);
            REQUIRE_THROWS_AS(!entry, std::runtime_error);

            REQUIRE(!missingEntry);
        }

        SECTION("loadWithoutRecord")
        {
            auto entry = ltx1.loadWithoutRecord(lk1);
            REQUIRE(entry);

            auto missingEntry = ltx1.loadWithoutRecord(missingEntryKey);
            REQUIRE(!missingEntry);

            // this will deactivate entry
            LedgerTxn ltx2(ltx1);
            REQUIRE_THROWS_AS(!entry, std::runtime_error);

            REQUIRE(!missingEntry);
        }

        SECTION("load and erase")
        {
            auto entry = ltx1.load(lk1);
            REQUIRE(entry);

            entry.erase();

            REQUIRE_THROWS_AS(!entry, std::runtime_error);
        }
        SECTION("load and move assign")
        {
            LedgerTxnEntry entry;
            entry = ltx1.load(lk1);
            REQUIRE(entry);

            LedgerTxnEntry ltxe2;
            ltxe2 = std::move(entry);
            REQUIRE_THROWS_AS(!entry, std::runtime_error);
        }
        SECTION("load and move construct")
        {
            auto entry = ltx1.load(lk1);
            REQUIRE(entry);

            LedgerTxnEntry ltxe2(std::move(entry));
            REQUIRE_THROWS_AS(!entry, std::runtime_error);
        }
        SECTION("loadWithoutRecord and move assign")
        {
            ConstLedgerTxnEntry entry;
            entry = ltx1.loadWithoutRecord(lk1);
            REQUIRE(entry);

            ConstLedgerTxnEntry ltxe2;
            ltxe2 = std::move(entry);
            REQUIRE_THROWS_AS(!entry, std::runtime_error);
        }
        SECTION("loadWithoutRecord and move construct")
        {
            auto entry = ltx1.loadWithoutRecord(lk1);
            REQUIRE(entry);

            ConstLedgerTxnEntry ltxe2(std::move(entry));
            REQUIRE_THROWS_AS(!entry, std::runtime_error);
        }
        SECTION("unassigned entry")
        {
            LedgerTxnEntry ltxe(nullptr);
            REQUIRE(!ltxe);

            ConstLedgerTxnEntry cltxe(nullptr);
            REQUIRE(!cltxe);

            // Move constructor
            LedgerTxnEntry ltxe2(std::move(ltxe));
            REQUIRE(!ltxe2);

            ConstLedgerTxnEntry cltxe2(std::move(cltxe));
            REQUIRE(!cltxe2);

            // Move assignment
            LedgerTxnEntry ltxe3;
            ltxe3 = std::move(ltxe);
            REQUIRE(!ltxe3);

            ConstLedgerTxnEntry cltxe3;
            cltxe3 = std::move(cltxe);
            REQUIRE(!cltxe3);
        }
    };

    SECTION("bucketlist")
    {
        runTest(Config::TESTDB_BUCKET_DB_PERSISTENT);
    }

    SECTION("in-memory")
    {
        runTest(Config::TESTDB_IN_MEMORY);
    }

#ifdef USE_POSTGRES
    SECTION("postgresql")
    {
        runTest(Config::TESTDB_POSTGRESQL);
    }
#endif
}

TEST_CASE("LedgerTxn generalized ledger entries", "[ledgertxn]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());

    InternalLedgerEntry gle(InternalLedgerEntryType::SPONSORSHIP);
    gle.sponsorshipEntry().sponsoredID = autocheck::generator<AccountID>()(5);
    gle.sponsorshipEntry().sponsoringID = autocheck::generator<AccountID>()(5);

    SECTION("create then load")
    {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        REQUIRE(ltx.create(gle));
        REQUIRE(ltx.load(gle.toKey()));
    }

    SECTION("create then commit then load")
    {
        LedgerTxn ltx1(app->getLedgerTxnRoot());
        {
            LedgerTxn ltx2(ltx1);
            REQUIRE(ltx2.create(gle));
            ltx2.commit();
        }
        REQUIRE(ltx1.load(gle.toKey()));
    }

    SECTION("create then commit then load in child")
    {
        LedgerTxn ltx1(app->getLedgerTxnRoot());
        {
            LedgerTxn ltx2(ltx1);
            REQUIRE(ltx2.create(gle));
            ltx2.commit();
        }
        {
            LedgerTxn ltx2(ltx1);
            REQUIRE(ltx2.load(gle.toKey()));
        }
    }
}

TEST_CASE("LedgerTxn best offers cache eviction", "[ledgertxn]")
{
    VirtualClock clock;
    auto cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);
    auto app = createTestApplication(clock, cfg);

    auto buying = autocheck::generator<Asset>()(UINT32_MAX);
    auto selling = autocheck::generator<Asset>()(UINT32_MAX);
    while (buying == selling)
    {
        selling = autocheck::generator<Asset>()(UINT32_MAX);
    }

    // Populate the database with two offers involving the same asset pair
    {
        LedgerTxn ltx(app->getLedgerTxnRoot());

        LedgerEntry le;
        le.data.type(OFFER);
        auto& oe = le.data.offer();
        oe.offerID = 1;
        oe.price = Price{1, 1};
        oe.buying = buying;
        oe.selling = selling;
        ltx.create(le);

        oe.offerID = 2;
        ltx.create(le);

        ltx.commit();
    }

    {
        LedgerTxn ltx1(app->getLedgerTxnRoot());

        // If the implementation involves caching, try to churn the cache
        {
            LedgerTxn ltx2(ltx1);
            auto ltxe1 = ltx2.loadBestOffer(buying, selling);
            REQUIRE(ltxe1.current().data.offer().offerID == 1);
            ltxe1.erase();

            for (size_t i = 0; i < 100; ++i)
            {
                auto a1 = autocheck::generator<Asset>()(UINT32_MAX);
                auto a2 = autocheck::generator<Asset>()(UINT32_MAX);
                if (!(a1 == a2))
                {
                    ltx2.loadBestOffer(a1, a2);
                }
            }

            auto ltxe2 = ltx2.loadBestOffer(buying, selling);
            REQUIRE(ltxe2.current().data.offer().offerID == 2);
        }

        // Access the cache to check if it is valid
        auto ltxe = ltx1.loadBestOffer(buying, selling);
        REQUIRE(ltxe.current().data.offer().offerID == 1);
    }
}

typedef std::map<std::tuple<AccountID, Asset, Asset>, int64_t> PoolShareUpdates;
typedef std::map<std::pair<Asset, Asset>, int64_t> LiquidityPoolUpdates;

static PoolID
getPoolID(Asset const& assetA, Asset const& assetB)
{
    return sha256(xdr::xdr_to_opaque(LIQUIDITY_POOL_CONSTANT_PRODUCT, assetA,
                                     assetB, LIQUIDITY_POOL_FEE_V18));
}

static void
applyLedgerTxnUpdates(AbstractLedgerTxn& ltx, PoolShareUpdates const& updates)
{
    for (auto const& kv : updates)
    {
        LedgerKey key(TRUSTLINE);
        key.trustLine().accountID = std::get<0>(kv.first);
        key.trustLine().asset.type(ASSET_TYPE_POOL_SHARE);
        key.trustLine().asset.liquidityPoolID() =
            getPoolID(std::get<1>(kv.first), std::get<2>(kv.first));

        auto ltxe = ltx.load(key);
        if (ltxe && kv.second > 0)
        {
            ltxe.current().data.trustLine().limit = kv.second;
        }
        else if (ltxe)
        {
            ltxe.erase();
        }
        else
        {
            REQUIRE(kv.second > 0);
            LedgerEntry trust;
            trust.lastModifiedLedgerSeq = ltx.loadHeader().current().ledgerSeq;
            trust.data.type(TRUSTLINE);

            auto& tl = trust.data.trustLine();
            tl = LedgerTestUtils::generateValidTrustLineEntry();
            tl.accountID = key.trustLine().accountID;
            tl.asset = key.trustLine().asset;
            tl.limit = kv.second;

            ltx.create(trust);
        }
    }
}

static void
applyLedgerTxnUpdates(AbstractLedgerTxn& ltx,
                      LiquidityPoolUpdates const& updates)
{
    for (auto const& kv : updates)
    {
        LedgerKey key(LIQUIDITY_POOL);
        key.liquidityPool().liquidityPoolID =
            getPoolID(kv.first.first, kv.first.second);

        auto ltxe = ltx.load(key);
        if (ltxe && kv.second > 0)
        {
            ltxe.current()
                .data.liquidityPool()
                .body.constantProduct()
                .poolSharesTrustLineCount = kv.second;
        }
        else if (ltxe)
        {
            ltxe.erase();
        }
        else
        {
            REQUIRE(kv.second > 0);
            LedgerEntry pool;
            pool.lastModifiedLedgerSeq = ltx.loadHeader().current().ledgerSeq;
            pool.data.type(LIQUIDITY_POOL);

            auto& lp = pool.data.liquidityPool();
            lp = LedgerTestUtils::generateValidLiquidityPoolEntry();
            lp.liquidityPoolID = key.liquidityPool().liquidityPoolID;
            auto& cp = lp.body.constantProduct();
            cp.params.assetA = kv.first.first;
            cp.params.assetB = kv.first.second;
            cp.poolSharesTrustLineCount = kv.second;

            ltx.create(pool);
        }
    }
}

static void
testPoolShareTrustLinesByAccountAndAsset(
    AbstractLedgerTxnParent& ltxParent, AccountID const& accountID,
    Asset const& asset,
    std::vector<std::tuple<Asset, Asset, int64_t>> const& expected,
    std::vector<std::pair<PoolShareUpdates,
                          LiquidityPoolUpdates>>::const_iterator begin,
    std::vector<std::pair<PoolShareUpdates,
                          LiquidityPoolUpdates>>::const_iterator const& end)
{
    REQUIRE(begin != end);
    LedgerTxn ltx(ltxParent);
    applyLedgerTxnUpdates(ltx, begin->first);
    applyLedgerTxnUpdates(ltx, begin->second);

    if (++begin != end)
    {
        testPoolShareTrustLinesByAccountAndAsset(ltx, accountID, asset,
                                                 expected, begin, end);
    }
    else
    {
        auto poolShares =
            ltx.loadPoolShareTrustLinesByAccountAndAsset(accountID, asset);
        REQUIRE(expected.size() == poolShares.size());
        auto expected2 = expected;

        for (auto& cur : poolShares)
        {
            auto const& tl = cur.current().data.trustLine();
            auto it = std::find_if(
                expected2.begin(), expected2.end(), [&](auto const& x) {
                    auto poolID = getPoolID(std::get<0>(x), std::get<1>(x));
                    return tl.accountID == accountID &&
                           tl.asset.liquidityPoolID() == poolID &&
                           tl.limit == std::get<2>(x);
                });
            REQUIRE(it != expected2.end());
            expected2.erase(it);
        }
    }
}

static void
testPoolShareTrustLinesByAccountAndAsset(
    AccountID const& accountID, Asset const& asset,
    std::vector<std::tuple<Asset, Asset, int64_t>> const& expected,
    std::vector<std::pair<PoolShareUpdates, LiquidityPoolUpdates>> updates)
{
    REQUIRE(!updates.empty());

    auto testAtRoot = [&](Application& app) {
        {
            LedgerTxn ltx1(app.getLedgerTxnRoot());
            applyLedgerTxnUpdates(ltx1, updates.cbegin()->first);
            applyLedgerTxnUpdates(ltx1, updates.cbegin()->second);
            ltx1.commit();
        }
        testPoolShareTrustLinesByAccountAndAsset(
            app.getLedgerTxnRoot(), accountID, asset, expected,
            ++updates.cbegin(), updates.cend());
    };

    // first changes are in LedgerTxnRoot with cache
    if (updates.size() > 1)
    {
        VirtualClock clock;
        auto app = createTestApplication(
            clock, getTestConfig(0, Config::TESTDB_IN_MEMORY));

        for_versions_from(18, *app, [&] { testAtRoot(*app); });
    }

    // first changes are in LedgerTxnRoot without cache
    if (updates.size() > 1)
    {
        VirtualClock clock;
        auto cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);
        cfg.ENTRY_CACHE_SIZE = 0;
        auto app = createTestApplication(clock, cfg);

        for_versions_from(18, *app, [&] { testAtRoot(*app); });
    }

    // first changes are in child of LedgerTxnRoot
    {
        VirtualClock clock;
        auto app = createTestApplication(
            clock, getTestConfig(0, Config::TESTDB_IN_MEMORY));

        for_versions_from(18, *app, [&] {
            testPoolShareTrustLinesByAccountAndAsset(
                app->getLedgerTxnRoot(), accountID, asset, expected,
                updates.cbegin(), updates.cend());
        });
    }
}

TEST_CASE_VERSIONS("LedgerTxn loadPoolShareTrustLinesByAccountAndAsset",
                   "[ledgertxn]")
{
    auto a1 = LedgerTestUtils::generateValidAccountEntry().accountID;
    auto a2 = LedgerTestUtils::generateValidAccountEntry().accountID;

    Asset native(ASSET_TYPE_NATIVE);
    Asset cur1 = txtest::makeAsset(txtest::getAccount("issuer"), "CUR1");
    Asset cur2 = txtest::makeAsset(txtest::getAccount("issuer"), "CUR2");
    if (cur2 < cur1)
    {
        std::swap(cur1, cur2);
    }

    SECTION("fails with children")
    {
        VirtualClock clock;
        auto app = createTestApplication(
            clock, getTestConfig(0, Config::TESTDB_IN_MEMORY));

        LedgerTxn ltx1(app->getLedgerTxnRoot());
        LedgerTxn ltx2(ltx1);
        REQUIRE_THROWS_AS(
            ltx1.loadPoolShareTrustLinesByAccountAndAsset(a1, cur1),
            std::runtime_error);
    }

    SECTION("fails if sealed")
    {
        VirtualClock clock;
        auto app = createTestApplication(
            clock, getTestConfig(0, Config::TESTDB_IN_MEMORY));

        LedgerTxn ltx1(app->getLedgerTxnRoot());
        ltx1.getDelta();
        REQUIRE_THROWS_AS(
            ltx1.loadPoolShareTrustLinesByAccountAndAsset(a1, cur1),
            std::runtime_error);
    }

    SECTION("empty parent")
    {
        SECTION("no trust lines")
        {
            testPoolShareTrustLinesByAccountAndAsset(a1, cur1, {}, {{}});
        }

        SECTION("two trust lines - assetA")
        {
            testPoolShareTrustLinesByAccountAndAsset(
                a1, cur1, {{cur1, native, 1}, {cur1, cur2, 2}},
                {{{{{a1, cur1, native}, 1}, {{a1, cur1, cur2}, 2}},
                  {{{cur1, native}, 1}, {{cur1, cur2}, 1}}}});
        }

        SECTION("two trust lines - assetB")
        {
            testPoolShareTrustLinesByAccountAndAsset(
                a1, cur2, {{cur1, cur2, 1}},
                {{{{{a1, cur1, cur2}, 1}}, {{{cur1, cur2}, 1}}}});
        }
    }

    SECTION("one trust line in parent")
    {
        SECTION("liquidity pool modified more recently than trust line")
        {
            testPoolShareTrustLinesByAccountAndAsset(
                a1, cur1, {{cur1, native, 1}},
                {{{{{a1, cur1, native}, 1}}, {{{cur1, native}, 1}}},
                 {{{{a2, cur1, native}, 1}}, {{{cur1, native}, 2}}}});
        }

        SECTION("trust line modified more recently than liquidity pool")
        {
            testPoolShareTrustLinesByAccountAndAsset(
                a1, cur1, {{cur1, native, 2}},
                {{{{{a1, cur1, native}, 1}}, {{{cur1, native}, 1}}},
                 {{{{a1, cur1, native}, 2}}, {}}});
        }

        SECTION("trust line erased")
        {
            testPoolShareTrustLinesByAccountAndAsset(
                a1, cur1, {},
                {{{{{a1, cur1, native}, 1}}, {{{cur1, native}, 1}}},
                 {{{{a1, cur1, native}, 0}, {{a2, cur1, native}, 1}}, {}}});
        }

        SECTION("liquidity pool and trust line erased")
        {
            testPoolShareTrustLinesByAccountAndAsset(
                a1, cur1, {},
                {{{{{a1, cur1, native}, 1}}, {{{cur1, native}, 1}}},
                 {{{{a1, cur1, native}, 0}}, {{{cur1, native}, 0}}}});
        }
    }
}

TEST_CASE("InMemoryLedgerTxn simulate buckets", "[ledgertxn]")
{
    VirtualClock clock;
    Config cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);

    auto app = createTestApplication(clock, cfg);

    auto root = TestAccount::createRoot(*app);
    auto const& lm = app->getLedgerManager();
    auto const minBalance15 = lm.getLastMinBalance(15);

    auto native = txtest::makeNativeAsset();
    auto cur1 = txtest::makeAsset(root, "CUR1");

    auto a1 = root.create("a1", minBalance15);

    LedgerEntry offerEntry;
    offerEntry.data.type(OFFER);

    auto& offer = offerEntry.data.offer();
    offer.sellerID = a1.getPublicKey();
    offer.offerID = 1;
    offer.selling = native;
    offer.buying = cur1;
    offer.amount = 1;
    offer.price = Price{1, 1};

    AbstractLedgerTxn* txnRoot =
        static_cast<AbstractLedgerTxn*>(&app->getLedgerTxnRoot());
    txnRoot->createWithoutLoading(offerEntry);

    auto offers = txnRoot->getOffersByAccountAndAsset(a1, cur1);
    REQUIRE(offers.size() == 1);
    for (auto const& kv : offers)
    {
        REQUIRE(kv.first.offer().sellerID == a1.getPublicKey());
    }

    txnRoot->eraseWithoutLoading(offerKey(a1, offer.offerID));
    REQUIRE(txnRoot->getOffersByAccountAndAsset(a1, cur1).size() == 0);
}

TEST_CASE("InMemoryLedgerTxn getOffersByAccountAndAsset", "[ledgertxn]")
{
    VirtualClock clock;
    Config cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);

    auto app = createTestApplication(clock, cfg);

    auto root = TestAccount::createRoot(*app);
    auto const& lm = app->getLedgerManager();
    auto const minBalance15 = lm.getLastMinBalance(15);

    auto native = txtest::makeNativeAsset();
    auto cur1 = txtest::makeAsset(root, "CUR1");
    auto cur2 = txtest::makeAsset(root, "CUR2");

    auto a1 = root.create("a1", minBalance15);
    auto a2 = root.create("a2", minBalance15);

    a1.changeTrust(cur1, 100);
    a1.changeTrust(cur2, 100);
    a2.changeTrust(cur1, 100);
    a2.changeTrust(cur2, 100);

    root.pay(a1, cur1, 10);
    root.pay(a2, cur1, 10);

    a1.manageOffer(0, native, cur1, Price{2, 1}, 1, MANAGE_OFFER_CREATED);
    a1.manageOffer(0, cur1, native, Price{2, 1}, 1, MANAGE_OFFER_CREATED);
    a1.manageOffer(0, cur1, cur2, Price{2, 1}, 1, MANAGE_OFFER_CREATED);

    a2.manageOffer(0, native, cur1, Price{2, 1}, 1, MANAGE_OFFER_CREATED);
    a2.manageOffer(0, cur1, native, Price{2, 1}, 1, MANAGE_OFFER_CREATED);
    a2.manageOffer(0, cur1, cur2, Price{2, 1}, 1, MANAGE_OFFER_CREATED);

    auto offers = app->getLedgerTxnRoot().getOffersByAccountAndAsset(a1, cur1);

    REQUIRE(offers.size() == 3);
    for (auto const& kv : offers)
    {
        REQUIRE(kv.first.offer().sellerID == a1.getPublicKey());
    }
}

TEST_CASE("InMemoryLedgerTxn getPoolShareTrustLinesByAccountAndAsset",
          "[ledgertxn]")
{
    VirtualClock clock;
    Config cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);

    auto app = createTestApplication(clock, cfg);

    auto root = TestAccount::createRoot(*app);
    auto const& lm = app->getLedgerManager();
    auto const minBalance15 = lm.getLastMinBalance(15);

    auto native = txtest::makeNativeAsset();
    auto cur1 = txtest::makeAsset(root, "CUR1");
    auto cur2 = txtest::makeAsset(root, "CUR2");

    auto share12 = txtest::makeChangeTrustAssetPoolShare(
        cur1, cur2, LIQUIDITY_POOL_FEE_V18);
    auto shareNative1 = txtest::makeChangeTrustAssetPoolShare(
        native, cur1, LIQUIDITY_POOL_FEE_V18);
    auto shareNative2 = txtest::makeChangeTrustAssetPoolShare(
        native, cur2, LIQUIDITY_POOL_FEE_V18);

    auto a1 = root.create("a1", minBalance15);
    auto a2 = root.create("a2", minBalance15);

    a1.changeTrust(cur1, 10);
    a1.changeTrust(cur2, 10);
    a2.changeTrust(cur1, 10);
    a2.changeTrust(cur2, 10);

    a1.changeTrust(share12, 10);
    a1.changeTrust(shareNative1, 10);
    a1.changeTrust(shareNative2, 10);

    a2.changeTrust(share12, 10);
    a2.changeTrust(shareNative1, 10);

    auto poolShareTrustlines =
        app->getLedgerTxnRoot().getPoolShareTrustLinesByAccountAndAsset(a1,
                                                                        cur1);

    REQUIRE(poolShareTrustlines.size() == 2);
    for (auto const& kv : poolShareTrustlines)
    {
        REQUIRE(kv.first.trustLine().accountID == a1.getPublicKey());
    }
}

TEST_CASE_VERSIONS("InMemoryLedgerTxn close multiple ledgers with merges",
                   "[ledgertxn]")
{
    VirtualClock clock;
    Config cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);

    auto app = createTestApplication(clock, cfg);

    auto root = TestAccount::createRoot(*app);
    auto const& lm = app->getLedgerManager();
    auto a1 = root.create("a1", lm.getLastMinBalance(1));
    auto b1 = root.create("b1", lm.getLastMinBalance(1));

    for_versions_from(19, *app, [&] {
        auto tx1 = txtest::transactionFrameFromOps(
            app->getNetworkID(), root, {a1.op(txtest::accountMerge(root))},
            {a1});
        auto tx2 = txtest::transactionFrameFromOps(
            app->getNetworkID(), root, {b1.op(txtest::accountMerge(root))},
            {b1});
        txtest::closeLedger(*app, {tx1});
        txtest::closeLedger(*app, {tx2});
    });
}

TEST_CASE("InMemoryLedgerTxn filtering", "[ledgertxn]")
{
    VirtualClock clock;
    Config cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);

    auto app = createTestApplication(clock, cfg);
    auto root = TestAccount::createRoot(*app);
    auto a1 = root.create("a1", app->getLedgerManager().getLastMinBalance(1));

    InternalLedgerEntry entry(InternalLedgerEntryType::MAX_SEQ_NUM_TO_APPLY);
    entry.maxSeqNumToApplyEntry().sourceAccount = root;
    entry.maxSeqNumToApplyEntry().maxSeqNum = 1;

    InternalLedgerEntry entry2(InternalLedgerEntryType::MAX_SEQ_NUM_TO_APPLY);
    entry2.maxSeqNumToApplyEntry().sourceAccount = a1;
    entry2.maxSeqNumToApplyEntry().maxSeqNum = 1;

    LedgerEntry le = LedgerTestUtils::generateValidLedgerEntryWithExclusions(
        {CONFIG_SETTING});

    LedgerTxn ltx(app->getLedgerTxnRoot());

    SECTION("1 entry")
    {
        ltx.create(entry);
        ltx.commit();

        // MAX_SEQ_NUM_TO_APPLY was filtered out on commit to InMemoryLedgerTxn
        LedgerTxn ltx2(app->getLedgerTxnRoot());
        REQUIRE(!ltx2.load(entry.toKey()));
    }

    SECTION("two entries")
    {
        ltx.create(entry);
        ltx.create(entry2);
        ltx.commit();

        LedgerTxn ltx2(app->getLedgerTxnRoot());
        REQUIRE(!ltx2.load(entry.toKey()));
        REQUIRE(!ltx2.load(entry2.toKey()));
    }

    SECTION("three entries, with one LEDGER_ENTRY")
    {
        ltx.create(entry);
        ltx.create(le);
        ltx.create(entry2);
        ltx.commit();

        LedgerTxn ltx1(app->getLedgerTxnRoot());
        REQUIRE(!ltx1.load(entry.toKey()));
        REQUIRE(!ltx1.load(entry2.toKey()));
        REQUIRE(ltx1.load(LedgerEntryKey(le)));
    }

    SECTION("one LEDGER_ENTRY")
    {
        ltx.create(le);
        ltx.commit();

        LedgerTxn ltx2(app->getLedgerTxnRoot());
        REQUIRE(ltx2.load(LedgerEntryKey(le)));
    }
}
