// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/ShardedTopBucket.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketUtils.h"
#include "bucket/InMemoryIndex.h"
#include "bucket/LedgerCmp.h"
#include "bucket/LiveBucketIndex.h"
#include "crypto/Hex.h"
#include "ledger/LedgerHashUtils.h"
#include "ledger/LedgerTypeUtils.h"
#include "main/Application.h"
#include "medida/timer.h"
#include "util/Fs.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/MetricsRegistry.h"
#include "util/ProtocolVersion.h"
#include "util/XDRStream.h"
#include <Tracy.hpp>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <queue>
#include <xdrpp/marshal.h>

namespace stellar
{

namespace
{
uint32_t const GROUP_SOROBAN = 0;
uint32_t const GROUP_TTL = 1;
uint32_t const GROUP_CLASSIC = 2;
uint32_t const NUM_GROUPS = 3;

uint64_t
load64(unsigned char const* p)
{
    uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint32_t
groupOf(LedgerEntryType t)
{
    switch (t)
    {
    case CONTRACT_DATA:
    case CONTRACT_CODE:
        return GROUP_SOROBAN;
    case TTL:
        return GROUP_TTL;
    default:
        return GROUP_CLASSIC;
    }
}

// The same as `ShardedTopBucket::shardOf` of the entry's key, without
// building the key for the three types that carry a random 32 bytes.
size_t
shardOfEntry(LedgerEntry const& e, uint32_t n)
{
    switch (e.data.type())
    {
    case ACCOUNT:
        if (e.data.account().accountID.type() == PUBLIC_KEY_TYPE_ED25519)
        {
            return GROUP_CLASSIC * n +
                   load64(e.data.account().accountID.ed25519().data()) % n;
        }
        break;
    case TTL:
        return GROUP_TTL * n + load64(e.data.ttl().keyHash.data()) % n;
    case CONTRACT_CODE:
        return GROUP_SOROBAN * n +
               load64(e.data.contractCode().hash.data()) % n;
    default:
        break;
    }
    return ShardedTopBucket::shardOf(LedgerEntryKey(e), n);
}

// One entry of a ledger's batch, as the cut hands it to its shard.
enum ItemKind : uint8_t
{
    ITEM_INIT,
    ITEM_LIVE,
    ITEM_DEAD
};
struct Item
{
    ItemKind kind;
    void const* ptr;
};

// The 4 bytes that stand in front of every record of an XDR file (see
// XDROutputFileStream::writeOne), then the record.
void
appendRecord(std::string& out, char const* data, size_t size)
{
    releaseAssert(size < 0x80000000u);
    char mark[4];
    mark[0] = static_cast<char>((size >> 24) & 0xFF) | '\x80';
    mark[1] = static_cast<char>((size >> 16) & 0xFF);
    mark[2] = static_cast<char>((size >> 8) & 0xFF);
    mark[3] = static_cast<char>(size & 0xFF);
    out.append(mark, 4);
    out.append(data, size);
}

LtHash16
hashOfRecords(ShardedTopBucket::Shard const& shard)
{
    LtHash16 h;
    for (size_t i = 0; i < shard.entries.size(); ++i)
    {
        h.add(ByteSlice(shard.records.data() + shard.offsets[i],
                        shard.offsets[i + 1] - shard.offsets[i]));
    }
    return h;
}

// Writes the file of the bucket whose entries are those of `shards`: the
// metadata entry, then every entry in the order of its key. No two shards
// hold the same key, so this is the merge of the shards.
void
writeBucketFile(
    std::vector<std::shared_ptr<ShardedTopBucket::Shard const>> const& shards,
    xdr::opaque_vec<> const* metaRecord, std::string const& filename,
    asio::io_context& ctx, bool doFsync)
{
    ZoneScoped;
    XDROutputFileStream out(ctx, doFsync);
    out.open(filename);

    size_t const FLUSH_AT = 1 << 20;
    std::string buf;
    buf.reserve(FLUSH_AT + (64 << 10));
    if (metaRecord)
    {
        appendRecord(buf, reinterpret_cast<char const*>(metaRecord->data()),
                     metaRecord->size());
    }

    struct Cursor
    {
        ShardedTopBucket::Shard const* shard;
        size_t i;
    };
    BucketEntryIdCmp<LiveBucket> idCmp;
    // The smallest key on top.
    auto later = [&idCmp](Cursor const& a, Cursor const& b) {
        return idCmp(b.shard->entries[b.i], a.shard->entries[a.i]);
    };
    std::priority_queue<Cursor, std::vector<Cursor>, decltype(later)> heap(
        later);
    for (auto const& shard : shards)
    {
        if (!shard->entries.empty())
        {
            heap.push(Cursor{shard.get(), 0});
        }
    }
    while (!heap.empty())
    {
        Cursor c = heap.top();
        heap.pop();
        auto const& s = *c.shard;
        appendRecord(buf, s.records.data() + s.offsets[c.i],
                     s.offsets[c.i + 1] - s.offsets[c.i]);
        if (buf.size() >= FLUSH_AT)
        {
            out.writeBytes(buf.data(), buf.size());
            buf.clear();
        }
        if (++c.i < s.entries.size())
        {
            heap.push(c);
        }
    }
    if (!buf.empty())
    {
        out.writeBytes(buf.data(), buf.size());
    }
    out.close();
}
}

void
ShardedTopBucket::Shard::append(BucketEntry&& entry)
{
    auto rec = xdr::xdr_to_opaque(entry);
    records.append(reinterpret_cast<char const*>(rec.data()), rec.size());
    releaseAssert(records.size() <= std::numeric_limits<uint32_t>::max());
    offsets.push_back(static_cast<uint32_t>(records.size()));
    entries.emplace_back(std::move(entry));
}

ShardedTopBucket::ShardedTopBucket(Application& app, uint32_t shardsPerGroup)
    : mShardsPerGroup(shardsPerGroup)
    , mNumShards(static_cast<size_t>(NUM_GROUPS) * shardsPerGroup)
    , mShardDir(app.getBucketManager().getBucketDir() + "/topshards")
    , mCutTimer(app.getMetrics().NewTimer({"bucket", "top-shard", "cut"}))
    , mShardsTimer(
          app.getMetrics().NewTimer({"bucket", "top-shard", "shards"}))
    , mFinishTimer(
          app.getMetrics().NewTimer({"bucket", "top-shard", "finish"}))
    , mFileTimer(app.getMetrics().NewTimer({"bucket", "top-shard", "file"}))
    , mFileWaitTimer(
          app.getMetrics().NewTimer({"bucket", "top-shard", "file-wait"}))
{
    releaseAssert(shardsPerGroup > 0);
    for (size_t i = 0; i < mNumShards; ++i)
    {
        mShards.emplace_back(std::make_shared<Shard const>());
    }
    // Shard files of an earlier run are of no use to this one.
    std::error_code ec;
    std::filesystem::remove_all(mShardDir, ec);
    fs::mkpath(mShardDir);
}

ShardedTopBucket::~ShardedTopBucket()
{
    // Let the file's writer finish (it holds nothing of this object but the
    // timers and the io context, which outlive it).
    if (mFileWriter.valid())
    {
        mFileWriter.wait();
    }
}

size_t
ShardedTopBucket::shardOf(LedgerKey const& key, uint32_t n)
{
    uint64_t h;
    switch (key.type())
    {
    case ACCOUNT:
        if (key.account().accountID.type() == PUBLIC_KEY_TYPE_ED25519)
        {
            h = load64(key.account().accountID.ed25519().data());
        }
        else
        {
            h = std::hash<LedgerKey>()(key);
        }
        break;
    case TTL:
        h = load64(key.ttl().keyHash.data());
        break;
    case CONTRACT_CODE:
        h = load64(key.contractCode().hash.data());
        break;
    default:
        h = std::hash<LedgerKey>()(key);
        break;
    }
    return static_cast<size_t>(groupOf(key.type())) * n + h % n;
}

Hash
ShardedTopBucket::bucketHash(LtHash16 const& h)
{
    return h.isZero() ? Hash() : h.digest();
}

bool
ShardedTopBucket::hasMetaEntry(uint32_t protocolVersion)
{
    return protocolVersionStartsFrom(
        protocolVersion,
        LiveBucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY);
}

BucketEntry
ShardedTopBucket::metaEntry(uint32_t protocolVersion)
{
    BucketMetadata meta;
    meta.ledgerVersion = protocolVersion;
    if (protocolVersionStartsFrom(
            protocolVersion,
            LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
    {
        meta.ext.v(1);
        meta.ext.bucketListType() = BucketListType::LIVE;
    }
    BucketEntry bme;
    bme.type(METAENTRY);
    bme.metaEntry() = meta;
    return bme;
}

LtHash16
ShardedTopBucket::hashOfFile(std::string const& filename)
{
    ZoneScoped;
    LtHash16 h;
    XDRInputFileStream in;
    in.open(filename);
    BucketEntry be;
    while (in && in.readOne(be))
    {
        auto rec = xdr::xdr_to_opaque(be);
        h.add(ByteSlice(rec.data(), rec.size()));
    }
    return h;
}

void
ShardedTopBucket::waitForFile()
{
    if (mFileReady.valid())
    {
        mFileReady.get();
    }
}

void
ShardedTopBucket::rebuildFrom(std::shared_ptr<LiveBucket> const& curr)
{
    ZoneScoped;
    std::vector<Shard> shards(mNumShards);
    LtHash16 total;
    if (!curr->isEmpty())
    {
        LiveBucketInputIterator it(curr);
        if (it.seenMetadata())
        {
            BucketEntry bme;
            bme.type(METAENTRY);
            bme.metaEntry() = it.getMetadata();
            auto rec = xdr::xdr_to_opaque(bme);
            total.add(ByteSlice(rec.data(), rec.size()));
        }
        for (; it; ++it)
        {
            BucketEntry e = *it;
            auto& shard =
                shards.at(shardOf(getBucketLedgerKey(e), mShardsPerGroup));
            shard.append(std::move(e));
        }
        for (auto& shard : shards)
        {
            shard.hash = hashOfRecords(shard);
            total += shard.hash;
        }
    }
    if (bucketHash(total) != curr->getHash())
    {
        throw std::runtime_error(fmt::format(
            FMT_STRING("level 0's bucket {} does not have the commutative "
                       "hash of its records ({}): it was written with "
                       "EXPERIMENTAL_SHARDED_TOP_BUCKET off"),
            binToHex(curr->getHash()), binToHex(bucketHash(total))));
    }
    mShards.clear();
    for (auto& shard : shards)
    {
        mShards.emplace_back(std::make_shared<Shard const>(std::move(shard)));
    }
    mCurrHash = curr->getHash();
    mFileReady = {};
}

std::shared_ptr<LiveBucket>
ShardedTopBucket::addBatch(Application& app, uint32_t ledgerSeq,
                           uint32_t protocolVersion,
                           std::shared_ptr<LiveBucket> const& curr,
                           LedgerEntryRefs initEntries,
                           LedgerEntryRefs liveEntries,
                           LedgerKeyRefs deadEntries, bool countMergeEvents,
                           bool doFsync)
{
    ZoneScoped;
    auto& bm = app.getBucketManager();

    // The file of the bucket made a ledger ago is long written. Being sure of
    // it here means that at most one level-0 file is ever missing on disk.
    {
        auto timer = mFileWaitTimer.TimeScope();
        waitForFile();
    }
    if (curr->getHash() != mCurrHash)
    {
        // A bucket this object did not make: the empty one after a spill, or
        // one that came from disk.
        rebuildFrom(curr);
    }
    auto prevShards = mShards;

    bool const useInit = protocolVersionStartsFrom(
        protocolVersion,
        LiveBucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY);
    uint32_t const n = mShardsPerGroup;
    size_t const total =
        initEntries.size() + liveEntries.size() + deadEntries.size();

    // 1. Cut the batch into the shards. Every task takes a stretch of the
    //    batch and sorts its entries into its own list per shard.
    size_t const cutTasks = std::max<size_t>(
        1, std::min<size_t>(mExecutor.preferredTaskCount(), total / 512));
    std::vector<std::vector<std::vector<Item>>> cut(
        cutTasks, std::vector<std::vector<Item>>(mNumShards));
    {
        auto timer = mCutTimer.TimeScope();
        mExecutor.executeBatchOverRanges(
            total, cutTasks, [&](size_t begin, size_t end, size_t task) {
                auto& mine = cut.at(task);
                size_t const initEnd = initEntries.size();
                size_t const liveEnd = initEnd + liveEntries.size();
                for (size_t i = begin; i < end; ++i)
                {
                    if (i < initEnd)
                    {
                        LedgerEntry const& e = initEntries[i].get();
                        mine[shardOfEntry(e, n)].push_back(
                            Item{ITEM_INIT, &e});
                    }
                    else if (i < liveEnd)
                    {
                        LedgerEntry const& e = liveEntries[i - initEnd].get();
                        mine[shardOfEntry(e, n)].push_back(
                            Item{ITEM_LIVE, &e});
                    }
                    else
                    {
                        LedgerKey const& k = deadEntries[i - liveEnd].get();
                        mine[shardOf(k, n)].push_back(Item{ITEM_DEAD, &k});
                    }
                }
            });
    }

    // 2. Every shard on its own: sort, write and sync the ledger's entries,
    //    merge them with the shard's entries of the previous ledger, hash,
    //    and prepare the shard's part of the index.
    struct Output
    {
        std::shared_ptr<Shard const> shard;
        InMemoryIndexShard index;
        MergeCounters mc;
        std::string file;
    };
    std::vector<Output> outputs(mNumShards);
    auto& ctx = app.getClock().getIOContext();
    {
        auto timer = mShardsTimer.TimeScope();
        size_t const shardTasks = std::max<size_t>(
            1, std::min<size_t>(mExecutor.preferredTaskCount(), mNumShards));
        mExecutor.executeBatchOverRanges(
            mNumShards, shardTasks, [&](size_t begin, size_t end, size_t) {
                BucketEntryIdCmp<LiveBucket> idCmp;
                for (size_t s = begin; s < end; ++s)
                {
                    auto& out = outputs[s];
                    size_t count = 0;
                    for (auto const& task : cut)
                    {
                        count += task[s].size();
                    }
                    std::vector<BucketEntry> fresh;
                    fresh.reserve(count);
                    for (auto const& task : cut)
                    {
                        for (auto const& item : task[s])
                        {
                            auto& be = fresh.emplace_back();
                            if (item.kind == ITEM_DEAD)
                            {
                                be.type(DEADENTRY);
                                be.deadEntry() =
                                    *static_cast<LedgerKey const*>(item.ptr);
                            }
                            else
                            {
                                be.type(item.kind == ITEM_INIT && useInit
                                            ? INITENTRY
                                            : LIVEENTRY);
                                be.liveEntry() =
                                    *static_cast<LedgerEntry const*>(item.ptr);
                            }
                            if (countMergeEvents)
                            {
                                LiveBucket::countNewEntryType(out.mc, be);
                            }
                        }
                    }
                    std::sort(fresh.begin(), fresh.end(), idCmp);
                    releaseAssertOrThrow(
                        std::adjacent_find(fresh.begin(), fresh.end(),
                                           [&idCmp](BucketEntry const& a,
                                                    BucketEntry const& b) {
                                               return !idCmp(a, b);
                                           }) == fresh.end());

                    auto freshShard = std::make_shared<Shard>();
                    freshShard->entries.reserve(fresh.size());
                    freshShard->offsets.reserve(fresh.size() + 1);
                    for (auto& be : fresh)
                    {
                        freshShard->append(std::move(be));
                    }

                    // The ledger's entries of this shard on disk, synced:
                    // from here on the ledger does not depend on the
                    // bucket's file.
                    if (!freshShard->entries.empty())
                    {
                        out.file = fmt::format(FMT_STRING("{}/shard-{}-{}.xdr"),
                                               mShardDir, ledgerSeq, s);
                        std::string fileBytes;
                        fileBytes.reserve(freshShard->records.size() +
                                          4 * freshShard->entries.size());
                        for (size_t i = 0; i < freshShard->entries.size(); ++i)
                        {
                            appendRecord(fileBytes,
                                         freshShard->records.data() +
                                             freshShard->offsets[i],
                                         freshShard->offsets[i + 1] -
                                             freshShard->offsets[i]);
                        }
                        XDROutputFileStream shardOut(ctx, doFsync);
                        shardOut.open(out.file);
                        shardOut.writeBytes(fileBytes.data(), fileBytes.size());
                        shardOut.close();
                    }

                    auto const& prev = *prevShards[s];
                    if (prev.entries.empty())
                    {
                        freshShard->hash = hashOfRecords(*freshShard);
                        out.shard = freshShard;
                    }
                    else
                    {
                        // (Also when the ledger has nothing for the shard:
                        // the merge with nothing is a copy.)
                        auto mergedEntries = LiveBucket::mergeEntriesInMemory(
                            bm, protocolVersion, prev.entries,
                            freshShard->entries, out.mc);
                        auto merged = std::make_shared<Shard>();
                        merged->entries.reserve(mergedEntries.size());
                        merged->offsets.reserve(mergedEntries.size() + 1);
                        for (auto& be : mergedEntries)
                        {
                            merged->append(std::move(be));
                        }
                        merged->hash = hashOfRecords(*merged);
                        out.shard = merged;
                    }

                    auto const& shard = *out.shard;
                    for (size_t i = 0; i < shard.entries.size(); ++i)
                    {
                        out.index.add(shard.entries[i],
                                      shard.offsets[i + 1] - shard.offsets[i]);
                    }
                }
            });
    }

    // 3. Add the shards up: the bucket's hash and its index.
    auto timer = mFinishTimer.TimeScope();
    LtHash16 hashState;
    size_t fileSize = 0;
    size_t entryCount = 0;
    std::vector<std::shared_ptr<Shard const>> shards;
    shards.reserve(mNumShards);
    std::vector<InMemoryIndexShard> indexShards;
    indexShards.reserve(mNumShards);
    std::vector<std::string> shardFiles;
    MergeCounters mc;
    for (auto& out : outputs)
    {
        hashState += out.shard->hash;
        fileSize +=
            out.shard->records.size() + 4 * out.shard->entries.size();
        entryCount += out.shard->entries.size();
        shards.emplace_back(out.shard);
        indexShards.emplace_back(std::move(out.index));
        mc += out.mc;
        if (!out.file.empty())
        {
            shardFiles.emplace_back(std::move(out.file));
        }
    }
    if (countMergeEvents)
    {
        bm.incrMergeCounters<LiveBucket>(mc);
    }

    bool const hasMeta = hasMetaEntry(protocolVersion);
    BucketEntry const bme = metaEntry(protocolVersion);
    auto metaRecord =
        std::make_shared<xdr::opaque_vec<> const>(xdr::xdr_to_opaque(bme));
    if (hasMeta)
    {
        hashState.add(ByteSlice(metaRecord->data(), metaRecord->size()));
        fileSize += metaRecord->size() + 4;
    }

    mShards = shards;

    if (!hasMeta && entryCount == 0)
    {
        // What a bucket without a metadata entry and without entries is:
        // no bucket.
        mCurrHash = Hash();
        mFileReady = {};
        return std::make_shared<LiveBucket>();
    }

    Hash const hash = bucketHash(hashState);
    auto index = std::make_shared<LiveBucketIndex const>(
        bm, std::make_unique<InMemoryIndex const>(
                std::move(indexShards),
                [n](LedgerKey const& k) { return shardOf(k, n); },
                bme.metaEntry(), hasMeta));

    auto promise = std::make_shared<std::promise<void>>();
    std::shared_future<void> ready = promise->get_future().share();
    bool created = false;
    auto bucket = bm.adoptLiveBucketAheadOfItsFile(
        hash, std::move(index), fileSize, ready, protocolVersion, created);
    mCurrHash = hash;

    if (!created)
    {
        // The bucket manager has a bucket with these records already, with
        // its file there or on its way.
        promise->set_value();
        for (auto const& f : shardFiles)
        {
            std::remove(f.c_str());
        }
        mFileReady = {};
        return bucket;
    }

    // 4. The bucket's file, off this thread.
    std::string tmpName = LiveBucket::randomBucketName(bm.getTmpDir());
    std::string canonicalName = bm.bucketFilename(hash);
    auto* fileTimer = &mFileTimer;
    auto* bmPtr = &bm;
    mFileReady = ready;
    mFileWriter = std::async(
        std::launch::async,
        [promise, shards, metaRecord, hasMeta, tmpName, canonicalName,
         shardFiles, fileTimer, bmPtr, &ctx, doFsync]() {
            try
            {
                auto fileScope = fileTimer->TimeScope();
                writeBucketFile(shards, hasMeta ? metaRecord.get() : nullptr,
                                tmpName, ctx, doFsync);
                if (!bmPtr->renameBucketDirFile(tmpName, canonicalName))
                {
                    throw std::runtime_error(
                        fmt::format(FMT_STRING("could not rename {} to {}: {}"),
                                    tmpName, canonicalName, strerror(errno)));
                }
                // The file holds what the ledger's shard files held.
                for (auto const& f : shardFiles)
                {
                    std::remove(f.c_str());
                }
                promise->set_value();
            }
            catch (...)
            {
                promise->set_exception(std::current_exception());
            }
        });
    return bucket;
}
}
