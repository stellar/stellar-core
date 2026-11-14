// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// Tests of level 0 of the live bucket list written as shards under the
// commutative hash (EXPERIMENTAL_SHARDED_TOP_BUCKET,
// bucket/ShardedTopBucket.h).

#include "bucket/BucketInputIterator.h"
#include "bucket/BucketManager.h"
#include "bucket/LiveBucket.h"
#include "bucket/LiveBucketIndex.h"
#include "bucket/LiveBucketList.h"
#include "bucket/ShardedTopBucket.h"
#include "bucket/test/BucketTestUtils.h"
#include "crypto/Hex.h"
#include "crypto/LtHash.h"
#include "crypto/SHA.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTypeUtils.h"
#include "ledger/test/LedgerTestUtils.h"
#include "main/Application.h"
#include "main/Config.h"
#include "medida/counter.h"
#include "medida/meter.h"
#include "medida/timer.h"
#include "simulation/LoadGenerator.h"
#include "simulation/Simulation.h"
#include "simulation/Topologies.h"
#include "test/Catch2.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Fs.h"
#include "util/Math.h"
#include "util/MetricsRegistry.h"
#include "util/Timer.h"
#include <fmt/format.h>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_set>

using namespace stellar;

namespace
{
std::string
fileBytes(std::filesystem::path const& name)
{
    std::ifstream in(name, std::ios::binary);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Batches that follow the rules a ledger follows: an entry is created (INIT)
// only where no live entry is, changed (LIVE) or deleted (DEAD) only where
// one is; a key occurs once per ledger. Keys come back after a deletion, and
// are deleted or changed in the ledger after their creation, so that every
// case of a merge of two ledgers at level 0 occurs.
struct BatchMaker
{
    std::map<LedgerKey, LedgerEntry> mLive;
    std::vector<LedgerEntry> mDeleted;
    std::vector<LedgerKey> mCreatedLastLedger;
    // The types of the entries that are created, changed and deleted freely.
    std::unordered_set<LedgerEntryType> mTypes{
        ACCOUNT, TRUSTLINE, OFFER, DATA, CONTRACT_DATA, CONTRACT_CODE, TTL};
    // For a ledger manager, which keeps contract data with its TTL: so many
    // new contract data entries per ledger, each with its TTL entry (while
    // mTypes holds the classic types only).
    size_t mContractDataWithTTL{0};

    void
    make(uint32_t ledger, size_t size, std::vector<LedgerEntry>& init,
         std::vector<LedgerEntry>& live, std::vector<LedgerKey>& dead)
    {
        init.clear();
        live.clear();
        dead.clear();
        std::set<LedgerKey> touched;
        auto& rng = getGlobalRandomEngine();

        // Delete or change half of what the previous ledger created.
        for (size_t i = 0; i < mCreatedLastLedger.size(); i += 2)
        {
            auto const& key = mCreatedLastLedger[i];
            auto it = mLive.find(key);
            if (it == mLive.end() || !touched.insert(key).second)
            {
                continue;
            }
            if (i % 4 == 0)
            {
                dead.emplace_back(key);
                mDeleted.emplace_back(it->second);
                mLive.erase(it);
            }
            else
            {
                it->second.lastModifiedLedgerSeq = ledger;
                live.emplace_back(it->second);
            }
        }
        mCreatedLastLedger.clear();

        // Bring back some of what was deleted (also in the ledger before).
        size_t back = std::min<size_t>(mDeleted.size(), size / 8 + 1);
        for (size_t i = 0; i < back; ++i)
        {
            auto e = mDeleted.back();
            mDeleted.pop_back();
            auto key = LedgerEntryKey(e);
            if (mLive.count(key) || !touched.insert(key).second)
            {
                continue;
            }
            e.lastModifiedLedgerSeq = ledger;
            mLive[key] = e;
            init.emplace_back(e);
            mCreatedLastLedger.emplace_back(key);
        }

        // New entries of every group.
        for (size_t i = 0; i < size; ++i)
        {
            auto e = LedgerTestUtils::generateValidLedgerEntryWithTypes(mTypes);
            auto key = LedgerEntryKey(e);
            if (mLive.count(key) || !touched.insert(key).second)
            {
                continue;
            }
            e.lastModifiedLedgerSeq = ledger;
            mLive[key] = e;
            init.emplace_back(e);
            mCreatedLastLedger.emplace_back(key);
        }
        for (size_t i = 0; i < mContractDataWithTTL; ++i)
        {
            auto e = LedgerTestUtils::generateValidLedgerEntryWithTypes(
                {CONTRACT_DATA});
            if (!touched.insert(LedgerEntryKey(e)).second)
            {
                continue;
            }
            e.lastModifiedLedgerSeq = ledger;
            LedgerEntry ttl;
            ttl.lastModifiedLedgerSeq = ledger;
            ttl.data.type(TTL);
            ttl.data.ttl().keyHash = getTTLKey(e).ttl().keyHash;
            ttl.data.ttl().liveUntilLedgerSeq = ledger + 10'000;
            init.emplace_back(e);
            init.emplace_back(ttl);
        }

        // Change and delete some older entries.
        std::vector<LedgerKey> keys;
        for (auto const& [key, _] : mLive)
        {
            if (!touched.count(key))
            {
                keys.emplace_back(key);
            }
        }
        stellar::shuffle(keys.begin(), keys.end(), rng);
        for (size_t i = 0; i < keys.size() && i < size / 2; ++i)
        {
            auto it = mLive.find(keys[i]);
            touched.insert(keys[i]);
            if (i % 3 == 0)
            {
                dead.emplace_back(keys[i]);
                mDeleted.emplace_back(it->second);
                mLive.erase(it);
            }
            else
            {
                it->second.lastModifiedLedgerSeq = ledger;
                live.emplace_back(it->second);
            }
        }
    }
};

Application::pointer
makeApp(VirtualClock& clock, int instance, uint32_t shardsPerGroup)
{
    Config cfg(getTestConfig(instance, Config::TESTDB_BUCKET_DB_PERSISTENT));
    cfg.EXPERIMENTAL_SHARDED_TOP_BUCKET = shardsPerGroup;
    return createTestApplication(clock, cfg);
}
}

TEST_CASE("sharded level 0 is the same buckets under another hash",
          "[bucket][bucketlist][sharded-top-bucket]")
{
    VirtualClock clock;
    // The first application has the setting off: its bucket list is what
    // every other one must agree with, apart from the hash of level 0.
    std::vector<uint32_t> const shardCounts{0, 1, 2, 5, 16};
    std::vector<Application::pointer> apps;
    std::vector<std::unique_ptr<LiveBucketList>> lists;
    for (size_t i = 0; i < shardCounts.size(); ++i)
    {
        apps.emplace_back(makeApp(clock, static_cast<int>(i), shardCounts[i]));
        lists.emplace_back(std::make_unique<LiveBucketList>());
    }
    auto version = apps[0]->getConfig().LEDGER_PROTOCOL_VERSION;

    BatchMaker maker;
    std::vector<LedgerEntry> init, live;
    std::vector<LedgerKey> dead;
    size_t sawTwoLedgerBuckets = 0;
    for (uint32_t ledger = 1; ledger <= 70; ++ledger)
    {
        // Now and then a ledger without entries.
        maker.make(ledger, ledger % 11 == 0 ? 0 : 40, init, live, dead);
        if (ledger % 23 == 0)
        {
            init.clear();
            live.clear();
            dead.clear();
        }
        for (size_t i = 0; i < apps.size(); ++i)
        {
            lists[i]->addBatch(*apps[i], ledger, version, init, live, dead);
        }
        for (auto& list : lists)
        {
            list->resolveAllFutures();
        }

        auto const& off = *lists[0];
        auto offCurr = off.getLevel(0).getCurr();
        auto offSnap = off.getLevel(0).getSnap();
        // What the hash of a level-0 bucket is with the setting: the LtHash
        // of the records of the file the bucket has without it.
        auto expectHash = [](std::shared_ptr<LiveBucket> const& b) {
            return b->isEmpty() ? Hash()
                                : ShardedTopBucket::bucketHash(
                                      ShardedTopBucket::hashOfFile(
                                          b->getFilename().string()));
        };
        Hash const currHash = expectHash(offCurr);
        Hash const snapHash = expectHash(offSnap);
        if (!offCurr->isEmpty())
        {
            REQUIRE(currHash != offCurr->getHash());
        }

        for (size_t i = 1; i < lists.size(); ++i)
        {
            INFO("ledger " << ledger << ", " << shardCounts[i]
                           << " shards per group");
            auto const& on = *lists[i];
            auto onCurr = on.getLevel(0).getCurr();
            auto onSnap = on.getLevel(0).getSnap();
            // Level 0: the commutative hash, whatever the shard count, and
            // the same file, byte for byte.
            REQUIRE(onCurr->getHash() == currHash);
            REQUIRE(onSnap->getHash() == snapHash);
            REQUIRE(onCurr->isEmpty() == offCurr->isEmpty());
            if (!onCurr->isEmpty())
            {
                REQUIRE(fileBytes(onCurr->getFilename()) ==
                        fileBytes(offCurr->getFilename()));
                REQUIRE(onCurr->getSize() == offCurr->getSize());
                REQUIRE(onCurr->getBucketVersion() ==
                        offCurr->getBucketVersion());
                REQUIRE(onCurr->getBucketEntryCounters() ==
                        offCurr->getBucketEntryCounters());
            }
            if (!onSnap->isEmpty())
            {
                REQUIRE(fileBytes(onSnap->getFilename()) ==
                        fileBytes(offSnap->getFilename()));
            }
            // Levels 1 and deeper: the same buckets under the same SHA-256.
            for (uint32_t level = 1; level < LiveBucketList::kNumLevels;
                 ++level)
            {
                REQUIRE(on.getLevel(level).getCurr()->getHash() ==
                        off.getLevel(level).getCurr()->getHash());
                REQUIRE(on.getLevel(level).getSnap()->getHash() ==
                        off.getLevel(level).getSnap()->getHash());
                REQUIRE(on.getLevel(level).getHash() ==
                        off.getLevel(level).getHash());
            }
            // The bucket list hash: the same for every shard count, and not
            // the one without the setting (level 0 is hashed differently).
            REQUIRE(on.getHash() == lists[1]->getHash());
            if (!offCurr->isEmpty() || !offSnap->isEmpty())
            {
                REQUIRE(on.getHash() != off.getHash());
            }

            // Every entry of the ledger is found through the bucket's index,
            // as the bucket without the setting has it.
            if (!onCurr->isEmpty())
            {
                auto const& index = onCurr->getIndexForTesting();
                auto const& offIndex = offCurr->getIndexForTesting();
                auto check = [&](LedgerKey const& key) {
                    auto got = index.lookup(key);
                    auto want = offIndex.lookup(key);
                    REQUIRE(got.getState() == want.getState());
                    if (want.getState() == IndexReturnState::CACHE_HIT)
                    {
                        REQUIRE(*got.cacheHit() == *want.cacheHit());
                    }
                };
                for (auto const& e : init)
                {
                    check(LedgerEntryKey(e));
                }
                for (auto const& e : live)
                {
                    check(LedgerEntryKey(e));
                }
                for (auto const& k : dead)
                {
                    check(k);
                }
                // And a key that is in no bucket.
                check(LedgerEntryKey(
                    LedgerTestUtils::generateValidLedgerEntryWithTypes(
                        {ACCOUNT, CONTRACT_DATA, TTL})));
                for (auto type : {ACCOUNT, TRUSTLINE, OFFER, DATA,
                                  CONTRACT_DATA, CONTRACT_CODE, TTL})
                {
                    REQUIRE(index.getRangeForType(type) ==
                            offIndex.getRangeForType(type));
                }
            }
        }
        if (ledger % 2 == 1 && ledger > 1 && !offCurr->isEmpty())
        {
            ++sawTwoLedgerBuckets;
        }
    }
    REQUIRE(sawTwoLedgerBuckets > 20);
}

TEST_CASE("level 0 is rebuilt from the shard files after a stop",
          "[bucket][bucketlist][sharded-top-bucket]")
{
    using BucketTestUtils::BucketTestApplication;
    // The ledger after which the node stops: one whose batch went into an
    // empty level 0 (it spilled), and one whose batch was merged with the
    // ledger before.
    for (uint32_t stopAt : {12u, 13u})
    {
        INFO("the node stops after ledger " << stopAt);
        Config cfgA(getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT));
        cfgA.EXPERIMENTAL_SHARDED_TOP_BUCKET = 3;
        // A node that stops there has not dropped the bucket of the ledger
        // before: that happens once the file is written.
        cfgA.DISABLE_BUCKET_GC = true;
        // The control: the same ledgers without a stop.
        Config cfgB(getTestConfig(1, Config::TESTDB_BUCKET_DB_PERSISTENT));
        cfgB.EXPERIMENTAL_SHARDED_TOP_BUCKET = 5;

        auto clockA = std::make_unique<VirtualClock>();
        VirtualClock clockB;
        auto appA = createTestApplication<BucketTestApplication>(*clockA, cfgA);
        auto appB = createTestApplication<BucketTestApplication>(clockB, cfgB);

        BatchMaker maker;
        // (No offers: those the ledger manager also keeps in SQL, where
        // entries put straight into the bucket list are not.)
        maker.mTypes = {ACCOUNT, TRUSTLINE, DATA};
        maker.mContractDataWithTTL = 10;
        std::vector<LedgerEntry> init, live;
        std::vector<LedgerKey> dead;
        auto close = [&](BucketTestApplication& app) {
            app.getLedgerManager().setNextLedgerEntryBatchForBucketTesting(
                init, live, dead);
            BucketTestUtils::closeLedger(app);
        };
        auto bucketListHash = [](Application& app) {
            return app.getLedgerManager()
                .getLastClosedLedgerHeader()
                .header.bucketListHash;
        };
        auto shardFiles = [](Application& app) {
            auto dir = app.getBucketManager().getBucketDir() + "/topshards";
            return fs::findfiles(
                dir, [](std::string const& name) { return name[0] == 's'; });
        };

        uint32_t ledger = appA->getLedgerManager().getLastClosedLedgerNum();
        REQUIRE(ledger == appB->getLedgerManager().getLastClosedLedgerNum());
        while (++ledger < stopAt)
        {
            maker.make(ledger, 40, init, live, dead);
            close(*appA);
            close(*appB);
            REQUIRE(bucketListHash(*appA) == bucketListHash(*appB));
        }

        maker.make(stopAt, 40, init, live, dead);
        ShardedTopBucket::loseFileForTesting(true);
        close(*appA);
        ShardedTopBucket::loseFileForTesting(false);
        close(*appB);
        REQUIRE(appA->getLedgerManager().getLastClosedLedgerNum() == stopAt);
        REQUIRE(bucketListHash(*appA) == bucketListHash(*appB));

        Hash const lost = appA->getBucketManager()
                              .getLiveBucketList()
                              .getLevel(0)
                              .getCurr()
                              ->getHash();
        std::string const lostFile = fmt::format(
            "{}/bucket-{}.xdr", appA->getBucketManager().getBucketDir(),
            binToHex(lost));
        REQUIRE(!isZero(lost));
        REQUIRE(!fs::exists(lostFile));
        REQUIRE(!shardFiles(*appA).empty());

        // The restart.
        appA.reset();
        clockA = std::make_unique<VirtualClock>();
        appA =
            createTestApplication<BucketTestApplication>(*clockA, cfgA, false);
        REQUIRE(appA->getLedgerManager().getLastClosedLedgerNum() == stopAt);
        REQUIRE(fs::exists(lostFile));
        REQUIRE(shardFiles(*appA).empty());
        REQUIRE(ShardedTopBucket::bucketHash(
                    ShardedTopBucket::hashOfFile(lostFile)) == lost);
        auto currB =
            appB->getBucketManager().getLiveBucketList().getLevel(0).getCurr();
        REQUIRE(currB->getHash() == lost);
        REQUIRE(fileBytes(lostFile) == fileBytes(currB->getFilename()));
        REQUIRE(bucketListHash(*appA) == bucketListHash(*appB));

        // And on from there, through the next spills.
        for (uint32_t i = 0; i < 9; ++i)
        {
            maker.make(++ledger, 40, init, live, dead);
            close(*appA);
            close(*appB);
            REQUIRE(bucketListHash(*appA) == bucketListHash(*appB));
        }
        REQUIRE(shardFiles(*appB).size() <= 15);
    }
}

TEST_CASE("nodes with different shard counts close the same ledgers",
          "[bucket][sharded-top-bucket][simulation]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    // How many shards a node uses is its own business.
    std::vector<uint32_t> const shardCounts{1, 2, 4, 7};
    // Threshold 1: every node votes, so every node computed the hash.
    auto simulation = Topologies::core(4, 1, networkID, [&](int i) {
        auto cfg = getTestConfig(i, Config::TESTDB_BUCKET_DB_PERSISTENT);
        cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 100;
        cfg.GENESIS_TEST_ACCOUNT_COUNT = 100;
        cfg.EXPERIMENTAL_SHARDED_TOP_BUCKET =
            shardCounts.at(static_cast<size_t>(i) % shardCounts.size());
        return cfg;
    });
    simulation->startAllNodes();
    auto nodes = simulation->getNodes();
    std::set<uint32_t> used;
    for (auto const& n : nodes)
    {
        used.insert(n->getConfig().EXPERIMENTAL_SHARDED_TOP_BUCKET);
    }
    REQUIRE(used.size() == shardCounts.size());

    uint32_t const desiredTxRate = 1;
    uint32_t const ledgerWideLimit = static_cast<uint32>(
        desiredTxRate * simulation->getExpectedLedgerCloseTime().count() * 2);
    upgradeSorobanNetworkConfig(
        [&](SorobanNetworkConfig& cfg) {
            setSorobanNetworkConfigForTest(cfg);
            cfg.mLedgerMaxTxCount = ledgerWideLimit;
        },
        simulation);

    // Classic entries from one node, contract code and TTL entries from
    // another: all three groups of shards get entries.
    auto& loadGen = nodes[0]->getLoadGenerator();
    auto& loadGenDone =
        nodes[0]->getMetrics().NewMeter({"loadgen", "run", "complete"}, "run");
    auto& secondLoadGen = nodes[1]->getLoadGenerator();
    auto& secondLoadGenDone =
        nodes[1]->getMetrics().NewMeter({"loadgen", "run", "complete"}, "run");
    auto count = loadGenDone.count();
    auto secondCount = secondLoadGenDone.count();
    uint32_t const txCount = 30;
    loadGen.generateLoad(GeneratedLoadConfig::txLoad(
        LoadGenMode::SOROBAN_UPLOAD, 50, txCount, desiredTxRate, 0));
    secondLoadGen.generateLoad(GeneratedLoadConfig::txLoad(
        LoadGenMode::PAY, 50, txCount, desiredTxRate, 50));
    simulation->crankUntil(
        [&]() {
            return loadGenDone.count() > count &&
                   secondLoadGenDone.count() > secondCount;
        },
        200 * simulation->getExpectedLedgerCloseTime(), false);
    REQUIRE(nodes[0]
                ->getMetrics()
                .NewMeter({"loadgen", "run", "failed"}, "run")
                .count() == 0);
    REQUIRE(nodes[1]
                ->getMetrics()
                .NewMeter({"loadgen", "run", "failed"}, "run")
                .count() == 0);

    uint32_t const target =
        nodes[0]->getLedgerManager().getLastClosedLedgerNum() + 2;
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(target, 1); },
        50 * simulation->getExpectedLedgerCloseTime(), false);

    // The same ledger at every node: the header holds the bucket list hash.
    // And the ledgers were not empty.
    uint32_t const seq =
        std::min({nodes[0]->getLedgerManager().getLastClosedLedgerNum(),
                  nodes[1]->getLedgerManager().getLastClosedLedgerNum(),
                  nodes[2]->getLedgerManager().getLastClosedLedgerNum(),
                  nodes[3]->getLedgerManager().getLastClosedLedgerNum()});
    REQUIRE(seq >= target - 1);
    for (auto const& n : nodes)
    {
        auto& shardTimer =
            n->getMetrics().NewTimer({"bucket", "top-shard", "shards"});
        REQUIRE(shardTimer.count() >= seq - 1);
        auto& applied =
            n->getMetrics().NewCounter({"ledger", "apply", "success"});
        REQUIRE(applied.count() >= 2 * txCount);
    }
    // (haveAllExternalized compares the ledgers' hashes: a node that
    // computed another bucket list hash would have stopped at its first
    // non-empty ledger.)
}
