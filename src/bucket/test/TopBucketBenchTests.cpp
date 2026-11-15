// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// A benchmark of what the bucket list does with one ledger's entries at
// level 0, with batches shaped like the native-asset payment load of the
// perfnet runs: per payment one changed account, one new contract data entry
// (a balance) and one new TTL entry. Hidden: run it by name or tag.

#include "bucket/BucketManager.h"
#include "bucket/BucketOutputIterator.h"
#include "bucket/LiveBucket.h"
#include "bucket/LiveBucketIndex.h"
#include "bucket/LiveBucketList.h"
#include "bucket/test/BucketTestUtils.h"
#include "crypto/SHA.h"
#include "ledger/InMemorySorobanState.h"
#include "ledger/LedgerEntryRefs.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTypeUtils.h"
#include "ledger/NetworkConfig.h"
#include "main/Application.h"
#include "main/Config.h"
#include "medida/timer.h"
#include "test/Catch2.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/MetricsRegistry.h"
#include "util/Timer.h"
#include <chrono>
#include <fmt/format.h>
#include <thread>

using namespace stellar;

namespace TopBucketBench
{
using Clock = std::chrono::steady_clock;

double
msSince(Clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// The entries of `payments` payments of ledger `ledger`: an account that
// exists (LIVE), and a balance entry and its TTL that do not (INIT).
void
makeBatch(uint32_t ledger, uint32_t payments, std::vector<LedgerEntry>& init,
          std::vector<LedgerEntry>& live)
{
    init.clear();
    live.clear();
    init.reserve(2 * payments);
    live.reserve(payments);
    Hash contract = sha256("native asset contract");
    for (uint32_t i = 0; i < payments; ++i)
    {
        auto seed = sha256(fmt::format("payment {} {}", ledger, i));

        LedgerEntry account;
        account.lastModifiedLedgerSeq = ledger;
        account.data.type(ACCOUNT);
        auto& ae = account.data.account();
        // Accounts repeat from ledger to ledger (the load's source accounts).
        ae.accountID.ed25519() = sha256(fmt::format("account {}", i));
        ae.balance = 1'000'000'000 + ledger;
        ae.seqNum = (static_cast<int64_t>(ledger) << 32) + i;
        ae.thresholds[0] = 1;
        ae.ext.v(1);
        ae.ext.v1().ext.v(2);
        ae.ext.v1().ext.v2().ext.v(3);
        ae.ext.v1().ext.v2().ext.v3().seqLedger = ledger;
        ae.ext.v1().ext.v2().ext.v3().seqTime = 1'700'000'000 + ledger;
        live.emplace_back(std::move(account));

        LedgerEntry balance;
        balance.lastModifiedLedgerSeq = ledger;
        balance.data.type(CONTRACT_DATA);
        auto& cd = balance.data.contractData();
        cd.contract.type(SC_ADDRESS_TYPE_CONTRACT);
        cd.contract.contractId() = contract;
        cd.durability = PERSISTENT;
        // Key: ["Balance", address]
        cd.key.type(SCV_VEC);
        cd.key.vec().activate();
        auto& keyVec = *cd.key.vec();
        keyVec.resize(2);
        keyVec[0].type(SCV_SYMBOL);
        keyVec[0].sym() = "Balance";
        keyVec[1].type(SCV_ADDRESS);
        keyVec[1].address().type(SC_ADDRESS_TYPE_CONTRACT);
        keyVec[1].address().contractId() = seed;
        // Value: {amount, authorized, clawback}
        cd.val.type(SCV_MAP);
        cd.val.map().activate();
        auto& m = *cd.val.map();
        m.resize(3);
        m[0].key.type(SCV_SYMBOL);
        m[0].key.sym() = "amount";
        m[0].val.type(SCV_I128);
        m[0].val.i128().lo = 1 + i;
        m[1].key.type(SCV_SYMBOL);
        m[1].key.sym() = "authorized";
        m[1].val.type(SCV_BOOL);
        m[1].val.b() = true;
        m[2].key.type(SCV_SYMBOL);
        m[2].key.sym() = "clawback";
        m[2].val.type(SCV_BOOL);
        m[2].val.b() = false;

        LedgerEntry ttl;
        ttl.lastModifiedLedgerSeq = ledger;
        ttl.data.type(TTL);
        ttl.data.ttl().keyHash = getTTLKey(balance).ttl().keyHash;
        ttl.data.ttl().liveUntilLedgerSeq = ledger + 500'000;

        init.emplace_back(std::move(balance));
        init.emplace_back(std::move(ttl));
    }
}
}

TEST_CASE("top bucket add by step with payment-shaped batches",
          "[.][bucketbench]")
{
    using namespace TopBucketBench;
    VirtualClock clock;
    Config cfg(getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT));
    // As a validator: sync the files.
    cfg.DISABLE_XDR_FSYNC = false;
    auto app = createTestApplication(clock, cfg);
    auto& bm = app->getBucketManager();
    auto version = app->getConfig().LEDGER_PROTOCOL_VERSION;

    for (uint32_t payments : {6400u, 10000u})
    {
        std::vector<LedgerEntry> init1, live1, init2, live2;
        makeBatch(2, payments, init1, live1);
        makeBatch(3, payments, init2, live2);
        std::vector<LedgerKey> dead;

        size_t xdrBytes = 0;
        for (auto const* v : {&init1, &live1})
        {
            for (auto const& e : *v)
            {
                xdrBytes += xdr::xdr_size(e);
            }
        }

        // The steps apart, on the second ledger's shape (a bucket of two
        // ledgers is written at every other ledger).
        auto t = Clock::now();
        auto entries1 =
            LiveBucket::convertToBucketEntry(true, init1, live1, dead);
        double convert = msSince(t);

        BucketMetadata meta;
        meta.ledgerVersion = version;
        meta.ext.v(1);
        meta.ext.bucketListType() = BucketListType::LIVE;

        t = Clock::now();
        auto index =
            std::make_shared<LiveBucketIndex const>(bm, entries1, meta);
        double indexMs = msSince(t);

        t = Clock::now();
        MergeCounters mc;
        LiveBucketOutputIterator out(bm.getTmpDir(), true, meta, mc,
                                     clock.getIOContext(), true);
        for (auto const& e : entries1)
        {
            out.put(e);
        }
        double put = msSince(t);
        t = Clock::now();
        auto b1 = out.getBucket(bm, nullptr, nullptr, index);
        double close = msSince(t);

        // Serializing and hashing alone, without a file.
        t = Clock::now();
        SHA256 hasher;
        size_t bytes = 0;
        for (auto const& e : entries1)
        {
            auto buf = xdr::xdr_to_opaque(e);
            hasher.add(buf);
            bytes += buf.size();
        }
        auto h = hasher.finish();
        double serialize = msSince(t);
        (void)h;

        t = Clock::now();
        std::vector<BucketEntry> copy(entries1);
        double copyMs = msSince(t);

        // The whole add, as the bucket list does it, over six ledgers.
        LiveBucketList bl;
        std::vector<double> adds;
        for (uint32_t ledger = 2; ledger < 8; ++ledger)
        {
            std::vector<LedgerEntry> init, live;
            makeBatch(ledger, payments, init, live);
            t = Clock::now();
            bl.addBatch(*app, ledger, version, init, live, dead);
            adds.push_back(msSince(t));
        }

        LOG_INFO(DEFAULT_LOG,
                 "TOPBUCKET {} payments: {} entries, {:.2f} MB of XDR | "
                 "convert and sort {:.1f} ms | copy of the entries {:.1f} ms "
                 "| in-memory index {:.1f} ms | output: put (serialize, hash, "
                 "write) {:.1f} ms, close (sync, adopt) {:.1f} ms | serialize "
                 "and SHA-256 alone {:.1f} ms ({} bytes) | addBatch at "
                 "ledgers 2..7: {:.1f} {:.1f} {:.1f} {:.1f} {:.1f} {:.1f} ms",
                 payments, entries1.size(), xdrBytes / 1e6, convert, copyMs,
                 indexMs, put, close, serialize, bytes, adds[0], adds[1],
                 adds[2], adds[3], adds[4], adds[5]);
        REQUIRE(!b1->isEmpty());
    }
}

TEST_CASE("top bucket add as shards with payment-shaped batches",
          "[.][bucketbench]")
{
    using namespace TopBucketBench;
    for (uint32_t shardsPerGroup : {0u, 4u, 8u, 16u, 32u})
    {
        VirtualClock clock;
        Config cfg(getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT));
        // TOPBUCKET_NO_FSYNC=1 shows what the syncs cost.
        cfg.DISABLE_XDR_FSYNC = getenv("TOPBUCKET_NO_FSYNC") != nullptr;
        cfg.EXPERIMENTAL_SHARDED_TOP_BUCKET = shardsPerGroup;
        auto app = createTestApplication(clock, cfg);
        auto version = app->getConfig().LEDGER_PROTOCOL_VERSION;
        for (uint32_t payments : {6400u, 10000u})
        {
            LiveBucketList bl;
            std::vector<LedgerKey> dead;
            std::vector<double> adds;
            // Ledgers 2 to 21. A level-0 spill at every even ledger; the
            // first ledgers warm the threads up and are left out.
            for (uint32_t ledger = 2; ledger < 22; ++ledger)
            {
                std::vector<LedgerEntry> init, live;
                makeBatch(ledger, payments, init, live);
                auto t = Clock::now();
                bl.addBatch(*app, ledger, version, init, live, dead);
                adds.push_back(msSince(t));
                // As a validator between two closes: let the background
                // work (the bucket's file, the level-1 merge) get on.
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }
            double odd = 0, even = 0;
            size_t nOdd = 0, nEven = 0;
            for (size_t i = 4; i < adds.size(); ++i)
            {
                // adds[i] is ledger i + 2: an even ledger adds to an empty
                // level 0 (it spilled), an odd one to one ledger.
                if ((i + 2) % 2 == 0)
                {
                    even += adds[i];
                    ++nEven;
                }
                else
                {
                    odd += adds[i];
                    ++nOdd;
                }
            }
            auto timer = [&](char const* name) {
                auto& t =
                    app->getMetrics().NewTimer({"bucket", "top-shard", name});
                return t.count() ? t.sum() / t.count() : 0.0;
            };
            LOG_INFO(DEFAULT_LOG,
                     "TOPSHARD {} shards per group, {} payments ({} "
                     "entries): addBatch mean {:.1f} ms after a spill, {:.1f} "
                     "ms onto one ledger | cut {:.1f} shards {:.1f} finish "
                     "{:.1f} file wait {:.2f} ms; the file (background) "
                     "{:.1f} ms",
                     shardsPerGroup, payments, 3 * payments, even / nEven,
                     odd / nOdd, timer("cut"), timer("shards"), timer("finish"),
                     timer("file-wait"), timer("file"));
        }
    }
}

// What the ledger commit runs beside the bucket list add (and waits for): the
// update of the in-memory Soroban state with the same batch. Its cost grows
// with the state: the maps it inserts into are large.
TEST_CASE("in-memory soroban state update with payment-shaped batches",
          "[.][bucketbench]")
{
    using namespace TopBucketBench;
    VirtualClock clock;
    Config cfg(getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT));
    auto app = createTestApplication(clock, cfg);
    auto& lm = app->getLedgerManager();
    std::optional<SorobanNetworkConfig const> sorobanConfig(
        lm.getLastClosedSorobanNetworkConfig());
    auto& metrics = lm.getSorobanMetrics();

    InMemorySorobanState state;
    LedgerHeader lh;
    lh.ledgerVersion = app->getConfig().LEDGER_PROTOCOL_VERSION;
    std::vector<LedgerKey> dead;
    uint32_t const payments = 10000;
    uint32_t const ledgers = 260;
    std::vector<double> times;
    for (uint32_t ledger = 1; ledger <= ledgers; ++ledger)
    {
        std::vector<LedgerEntry> init, live;
        makeBatch(ledger, payments, init, live);
        lh.ledgerSeq = ledger;
        auto initRefs = toRefs(init);
        auto liveRefs = toRefs(live);
        auto deadRefs = toRefs(dead);
        auto t = Clock::now();
        state.updateState(initRefs, liveRefs, deadRefs, lh, sorobanConfig,
                          metrics);
        times.push_back(msSince(t));
    }
    auto mean = [&](size_t from, size_t to) {
        double sum = 0;
        for (size_t i = from; i < to; ++i)
        {
            sum += times[i];
        }
        return sum / static_cast<double>(to - from);
    };
    LOG_INFO(DEFAULT_LOG,
             "SOROBANSTATE {} payments per ledger (as many new contract data "
             "entries with their TTL): updateState mean {:.1f} ms at ledgers "
             "1..20, {:.1f} ms at 121..140 (1.2 million entries), {:.1f} ms "
             "at 241..260 (2.4 million entries)",
             payments, mean(0, 20), mean(120, 140), mean(240, 260));
}
