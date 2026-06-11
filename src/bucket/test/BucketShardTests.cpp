// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// Tests for the sharded (composite) level-0 curr bucket: shard construction,
// composite hashing, the k-way spill merge that condenses shards into the
// level-1 bucket, and lookups across shards.

#include "bucket/BucketInputIterator.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketUtils.h"
#include "bucket/LiveBucket.h"
#include "bucket/LiveBucketList.h"
#include "bucket/test/BucketTestUtils.h"
#include "crypto/SHA.h"
#include "ledger/ImmutableLedgerView.h"
#include "ledger/LedgerManager.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Fs.h"
#include "util/ProtocolVersion.h"
#include "util/types.h"

#include <filesystem>

using namespace stellar;
using namespace BucketTestUtils;

namespace
{

std::shared_ptr<LiveBucket>
makeShard(Application& app, uint32_t protocolVersion,
          std::vector<LedgerEntry> const& initEntries,
          std::vector<LedgerEntry> const& liveEntries,
          std::vector<LedgerKey> const& deadEntries)
{
    bool useInit = protocolVersionStartsFrom(
        protocolVersion,
        LiveBucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY);
    auto entries = LiveBucket::convertToBucketEntry(useInit, initEntries,
                                                    liveEntries, deadEntries);
    REQUIRE(!entries.empty());
    return LiveBucket::freshShard(app.getBucketManager(), protocolVersion,
                                  std::move(entries),
                                  app.getClock().getIOContext(),
                                  !app.getConfig().DISABLE_XDR_FSYNC);
}

std::vector<LedgerEntry>
generateEntries(size_t n)
{
    return LedgerTestUtils::generateValidUniqueLedgerEntriesWithExclusions(
        {CONFIG_SETTING, CONTRACT_DATA, CONTRACT_CODE, TTL, OFFER}, n);
}

// Require that all levels >= 1 of the two bucket lists are identical. Level 0
// is intentionally excluded: the two lists are fed the same entries but with
// different shard partitions, so their composite level-0 hashes differ while
// the condensed levels below must not.
void
requireLowerLevelsEqual(LiveBucketList const& a, LiveBucketList const& b)
{
    for (uint32_t j = 1; j < LiveBucketList::kNumLevels; ++j)
    {
        CAPTURE(j);
        REQUIRE(a.getLevel(j).getHash() == b.getLevel(j).getHash());
    }
}

} // namespace

TEST_CASE("composite shard set hash and properties", "[bucket][bucketshard]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto vers = getAppLedgerVersion(app);

    auto shard0 = makeShard(*app, vers, {}, generateEntries(4), {});
    auto shard1 = makeShard(*app, vers, {}, generateEntries(5), {});
    auto shard2 = makeShard(*app, vers, {}, generateEntries(6), {});

    auto composite = LiveBucket::makeSharded({shard0, shard1, shard2});
    REQUIRE(composite->isSharded());
    REQUIRE(!composite->isEmpty());
    REQUIRE(composite->getShards().size() == 3);

    // Combined hash is the SHA256 of the concatenated shard hashes, oldest
    // first.
    {
        SHA256 hsh;
        hsh.add(shard0->getHash());
        hsh.add(shard1->getHash());
        hsh.add(shard2->getHash());
        REQUIRE(composite->getHash() == hsh.finish());
    }

    // Order matters.
    auto reordered = LiveBucket::makeSharded({shard1, shard0, shard2});
    REQUIRE(reordered->getHash() != composite->getHash());

    // Size and entry counters aggregate over shards.
    REQUIRE(composite->getSize() ==
            shard0->getSize() + shard1->getSize() + shard2->getSize());
    auto counters = shard0->getBucketEntryCounters();
    counters += shard1->getBucketEntryCounters();
    counters += shard2->getBucketEntryCounters();
    REQUIRE(composite->getBucketEntryCounters() == counters);

    REQUIRE(composite->getBucketVersion() == vers);

    // A single-shard composite hashes the one-element shard list, which is
    // not the shard's own hash.
    auto single = LiveBucket::makeSharded({shard0});
    REQUIRE(single->isSharded());
    REQUIRE(single->getHash() != shard0->getHash());
    {
        SHA256 hsh;
        hsh.add(shard0->getHash());
        REQUIRE(single->getHash() == hsh.finish());
    }
}

TEST_CASE("sharded addBatch equivalent to single-shard addBatch",
          "[bucket][bucketshard]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto vers = getAppLedgerVersion(app);

    LiveBucketList blSingle;
    LiveBucketList blSharded;

    for (uint32_t i = 1; i < 68; ++i)
    {
        app->getClock().crank(false);
        auto live = generateEntries(9);
        auto dead = LedgerTestUtils::generateValidLedgerEntryKeysWithExclusions(
            {CONFIG_SETTING, CONTRACT_DATA, CONTRACT_CODE, TTL, OFFER}, 3);

        blSingle.addBatch(*app, i, vers, {}, live, dead);

        // Split the same batch into three shards, round-robin.
        std::vector<std::vector<LedgerEntry>> liveGroups(3);
        std::vector<std::vector<LedgerKey>> deadGroups(3);
        for (size_t k = 0; k < live.size(); ++k)
        {
            liveGroups[k % 3].push_back(live[k]);
        }
        for (size_t k = 0; k < dead.size(); ++k)
        {
            deadGroups[k % 3].push_back(dead[k]);
        }
        std::vector<std::shared_ptr<LiveBucket>> shards;
        for (size_t g = 0; g < 3; ++g)
        {
            if (!liveGroups[g].empty() || !deadGroups[g].empty())
            {
                shards.push_back(
                    makeShard(*app, vers, {}, liveGroups[g], deadGroups[g]));
            }
        }
        blSharded.addBatchShards(*app, i, vers, std::move(shards));

        requireLowerLevelsEqual(blSingle, blSharded);

        // Level-0 composites on both sides hold the same logical entries.
        auto countCurr = [](LiveBucketList const& bl) {
            return countEntries(bl.getLevel(0).getCurr());
        };
        REQUIRE(countCurr(blSingle) == countCurr(blSharded));
    }
}

TEST_CASE("cross-shard same-key lifecycle folding", "[bucket][bucketshard]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto vers = getAppLedgerVersion(app);

    LiveBucketList blSingle;
    LiveBucketList blSharded;

    // Ledger 1: create the pre-existing entries needed for lifecycle-legal
    // sequences in ledger 2.
    auto preexisting = generateEntries(2);
    auto k1 = preexisting[0]; // deleted and recreated across shards
    auto k4 = preexisting[1]; // updated twice across shards
    blSingle.addBatch(*app, 1, vers, {}, preexisting, {});
    blSharded.addBatchShards(*app, 1, vers,
                             {makeShard(*app, vers, {}, preexisting, {})});

    // Ledger 2: same-key entries across two shards.
    auto fresh = generateEntries(2);
    auto k2 = fresh[0]; // created in shard X, updated in shard Y
    auto k3 = fresh[1]; // created in shard X, deleted in shard Y

    auto bump = [](LedgerEntry e) {
        e.lastModifiedLedgerSeq += 1;
        return e;
    };
    auto k1New = bump(k1);
    auto k2New = bump(k2);
    auto k4New = bump(k4);

    // Shard X (older): INIT k2, INIT k3, DEAD k1, LIVE k4.
    auto shardX = makeShard(*app, vers, {k2, k3}, {k4},
                            {LedgerEntryKey(k1)});
    // Shard Y (newer): LIVE k2', DEAD k3, INIT k1', LIVE k4'.
    auto shardY = makeShard(*app, vers, {k1New}, {k2New, k4New},
                            {LedgerEntryKey(k3)});

    // The single-shard equivalent is what the ltx would have produced for
    // the whole ledger: k2 created (with the newer value), k3 elided
    // entirely, k1 updated, k4 updated.
    blSingle.addBatch(*app, 2, vers, {k2New}, {k1New, k4New}, {});
    blSharded.addBatchShards(*app, 2, vers, {shardX, shardY});

    // Drive empty batches until the ledger-2 batch has been condensed into
    // level 1 (spilled at ledger 4, committed by ledger 6). Stop before
    // ledger 8 so level 1 hasn't snapped the result away yet.
    for (uint32_t i = 3; i <= 7; ++i)
    {
        blSingle.addBatch(*app, i, vers, {}, {}, {});
        blSharded.addBatchShards(*app, i, vers, {});
        requireLowerLevelsEqual(blSingle, blSharded);
    }

    // Sanity-check the condensed level-1 content on the sharded side: k3
    // must have been annihilated, k2 must exist (INIT), k1 and k4 must be
    // live with the newer values.
    bool sawK2 = false, sawK3 = false;
    std::vector<std::shared_ptr<LiveBucket>> lowerBuckets;
    for (uint32_t j = 1; j < LiveBucketList::kNumLevels; ++j)
    {
        lowerBuckets.push_back(blSharded.getLevel(j).getCurr());
        lowerBuckets.push_back(blSharded.getLevel(j).getSnap());
    }
    for (auto const& b : lowerBuckets)
    {
        if (b->isEmpty())
        {
            continue;
        }
        for (LiveBucketInputIterator it(b); it; ++it)
        {
            auto const& e = *it;
            if (e.type() == INITENTRY || e.type() == LIVEENTRY)
            {
                auto key = LedgerEntryKey(e.liveEntry());
                if (key == LedgerEntryKey(k2))
                {
                    sawK2 = true;
                    REQUIRE(e.liveEntry() == k2New);
                }
                if (key == LedgerEntryKey(k3))
                {
                    sawK3 = true;
                }
            }
        }
    }
    REQUIRE(sawK2);
    REQUIRE(!sawK3);
}

TEST_CASE("lookups across sharded level-0 curr", "[bucket][bucketshard]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto vers = getAppLedgerVersion(app);

    auto entries = generateEntries(6);
    std::vector<LedgerEntry> older(entries.begin(), entries.begin() + 3);
    std::vector<LedgerEntry> newer(entries.begin() + 3, entries.end());

    // The newest shard shadows entry 0 from the older shard.
    auto shadowed = older[0];
    shadowed.lastModifiedLedgerSeq += 1;
    newer.push_back(shadowed);

    auto header = app->getLedgerManager().getLastClosedLedgerHeader().header;
    header.ledgerSeq += 1;
    addLiveBatchShardsAndUpdateSnapshot(
        *app, header,
        {makeShard(*app, vers, {}, older, {}),
         makeShard(*app, vers, {}, newer, {})});

    auto ledgerView = app->getLedgerManager().copyImmutableLedgerView();

    // Point loads: the shadowed entry resolves to the newest version.
    auto loaded = ledgerView.loadLiveEntry(LedgerEntryKey(shadowed));
    REQUIRE(loaded);
    REQUIRE(*loaded == shadowed);
    for (size_t i = 1; i < entries.size(); ++i)
    {
        auto l = ledgerView.loadLiveEntry(LedgerEntryKey(entries[i]));
        REQUIRE(l);
        REQUIRE(*l == entries[i]);
    }

    // Bulk load through all shards.
    LedgerKeySet keys;
    for (auto const& e : entries)
    {
        keys.emplace(LedgerEntryKey(e));
    }
    auto bulk = ledgerView.loadLiveKeys(keys, "test");
    REQUIRE(bulk.size() == entries.size());

    // A second ledger's shard shadows across ledgers within curr.
    auto reshadowed = shadowed;
    reshadowed.lastModifiedLedgerSeq += 1;
    header.ledgerSeq += 1;
    addLiveBatchShardsAndUpdateSnapshot(
        *app, header, {makeShard(*app, vers, {}, {reshadowed}, {})});

    auto ledgerView2 = app->getLedgerManager().copyImmutableLedgerView();
    auto reloaded = ledgerView2.loadLiveEntry(LedgerEntryKey(reshadowed));
    REQUIRE(reloaded);
    REQUIRE(*reloaded == reshadowed);

    // Shard files are retained by GC while the composite is referenced.
    auto has = app->getLedgerManager().getLastClosedLedgerHAS();
    app->getBucketManager().forgetUnreferencedBuckets(has);
    auto curr = app->getBucketManager().getLiveBucketList().getLevel(0).getCurr();
    REQUIRE(curr->isSharded());
    for (auto const& shard : curr->getShards())
    {
        REQUIRE(std::filesystem::exists(shard->getFilename()));
    }
}

TEST_CASE("history archive state with sharded level 0",
          "[bucket][bucketshard][history]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto vers = getAppLedgerVersion(app);

    // Drive the app's bucket list with a multi-shard batch.
    auto header = app->getLedgerManager().getLastClosedLedgerHeader().header;
    header.ledgerSeq += 1;
    addLiveBatchShardsAndUpdateSnapshot(
        *app, header,
        {makeShard(*app, vers, {}, generateEntries(4), {}),
         makeShard(*app, vers, {}, generateEntries(5), {})});

    auto& bl = app->getBucketManager().getLiveBucketList();
    auto curr = bl.getLevel(0).getCurr();
    REQUIRE(curr->isSharded());
    auto numShards = curr->getShards().size();

    HistoryArchiveState has(header.ledgerSeq, bl,
                            app->getConfig().NETWORK_PASSPHRASE);

    // The HAS records the composite hash plus the shard list.
    REQUIRE(has.currentBuckets.at(0).curr == binToHex(curr->getHash()));
    REQUIRE(has.currentBuckets.at(0).currShards.size() == numShards);

    // The bucket-list hash recomputed from the HAS matches the in-memory
    // bucket list (the composite hash participates directly).
    REQUIRE(has.getBucketListHash() == bl.getHash());

    // allBuckets enumerates shard files, never the composite hash.
    auto all = has.allBuckets();
    REQUIRE(std::find(all.begin(), all.end(),
                      binToHex(curr->getHash())) == all.end());
    for (auto const& shard : curr->getShards())
    {
        REQUIRE(std::find(all.begin(), all.end(),
                          binToHex(shard->getHash())) != all.end());
    }

    // JSON round trip preserves the shard list and the hash.
    HistoryArchiveState has2;
    has2.fromString(has.toString());
    REQUIRE(has2.currentBuckets.at(0).currShards ==
            has.currentBuckets.at(0).currShards);
    REQUIRE(has2.getBucketListHash() == has.getBucketListHash());
}

TEST_CASE("empty and trivial shard batches", "[bucket][bucketshard]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto vers = getAppLedgerVersion(app);

    LiveBucketList bl;

    // An empty shard list at a META-supporting protocol produces a meta-only
    // shard so curr still carries the protocol version (matching the legacy
    // per-ledger level-0 merge behavior).
    bl.addBatchShards(*app, 1, vers, {});
    {
        auto curr = bl.getLevel(0).getCurr();
        REQUIRE(curr->isSharded());
        REQUIRE(curr->getShards().size() == 1);
        REQUIRE(countEntries(curr) == 0);
        REQUIRE(curr->getBucketVersion() == vers);
    }

    // A non-empty batch after the ledger-2 spill starts a fresh composite.
    bl.addBatchShards(*app, 2, vers,
                      {makeShard(*app, vers, {}, generateEntries(3), {})});
    auto curr = bl.getLevel(0).getCurr();
    REQUIRE(curr->isSharded());
    REQUIRE(curr->getShards().size() == 1);
    REQUIRE(countEntries(curr) == 3);

    // Null shards are skipped, but the ledger still contributes a meta-only
    // shard alongside the retained ledger-2 shard.
    bl.addBatchShards(*app, 3, vers, {nullptr});
    auto after = bl.getLevel(0).getCurr();
    REQUIRE(after->isSharded());
    REQUIRE(after->getShards().size() == 2);
    REQUIRE(after->getShards().front() == curr->getShards().front());
    REQUIRE(countEntries(after) == 3);
}
