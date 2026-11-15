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
#include "crypto/Random.h"
#include "history/HistoryArchive.h"
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
#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <queue>
#include <unistd.h>
#include <xdrpp/marshal.h>

namespace stellar
{

namespace
{
uint32_t const GROUP_SOROBAN = 0;
uint32_t const GROUP_TTL = 1;
uint32_t const GROUP_CLASSIC = 2;
uint32_t const NUM_GROUPS = 3;

// The first 8 of 32 random-looking bytes of a key, mixed with a value that
// only this process knows: whoever chooses keys (account ids, contract code)
// cannot aim them at one shard.
uint64_t
load64(unsigned char const* p)
{
    static uint64_t const salt = []() {
        auto bytes = randomBytes(sizeof(uint64_t));
        uint64_t v;
        std::memcpy(&v, bytes.data(), sizeof(v));
        return v;
    }();
    uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    v ^= salt;
    // (the finalizer of splitmix64)
    v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
    v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
    return v ^ (v >> 31);
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

// Makes the names of the files created in `dir` durable.
void
syncDir(std::string const& dir)
{
    ZoneScoped;
    int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd == -1)
    {
        throw std::runtime_error(fmt::format(
            FMT_STRING("could not open {}: {}"), dir, strerror(errno)));
    }
    int rc = ::fsync(fd);
    int err = errno;
    ::close(fd);
    if (rc == -1)
    {
        throw std::runtime_error(fmt::format(
            FMT_STRING("could not sync {}: {}"), dir, strerror(err)));
    }
}

// A ledger's shard file is "shard-<ledger>-<hash of the bucket the ledger was
// added to, in hex>-<instance>-<shard>.xdr".
bool
isShardFile(std::string const& name)
{
    return name.size() > 10 && name.compare(0, 6, "shard-") == 0 &&
           name.compare(name.size() - 4, 4, ".xdr") == 0;
}

struct ShardFileName
{
    uint32_t ledger{0};
    std::string prevHex;
    std::string instance;
};

std::optional<ShardFileName>
parseShardFileName(std::string const& name)
{
    if (!isShardFile(name))
    {
        return std::nullopt;
    }
    std::vector<std::string> parts;
    size_t begin = 0;
    for (size_t i = 0; i <= name.size(); ++i)
    {
        if (i == name.size() || name[i] == '-')
        {
            parts.emplace_back(name.substr(begin, i - begin));
            begin = i + 1;
        }
    }
    if (parts.size() != 5 || parts[2].size() != 64 || parts[1].empty() ||
        parts[1].size() > 10 ||
        parts[1].find_first_not_of("0123456789") != std::string::npos)
    {
        return std::nullopt;
    }
    ShardFileName res;
    res.ledger = static_cast<uint32_t>(std::stoull(parts[1]));
    res.prevHex = parts[2];
    res.instance = parts[3];
    return res;
}

#ifdef BUILD_TESTS
std::atomic<bool> gLoseFileForTesting{false};
#endif
}

#ifdef BUILD_TESTS
void
ShardedTopBucket::loseFileForTesting(bool lose)
{
    gLoseFileForTesting = lose;
}
#endif

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
    , mInstance(binToHex(randomBytes(4)))
    , mExecutor(/*pinWorkers=*/false)
    , mCutTimer(app.getMetrics().NewTimer({"bucket", "top-shard", "cut"}))
    , mShardsTimer(app.getMetrics().NewTimer({"bucket", "top-shard", "shards"}))
    , mFinishTimer(app.getMetrics().NewTimer({"bucket", "top-shard", "finish"}))
    , mFileTimer(app.getMetrics().NewTimer({"bucket", "top-shard", "file"}))
    , mFileWaitTimer(
          app.getMetrics().NewTimer({"bucket", "top-shard", "file-wait"}))
{
    releaseAssert(shardsPerGroup > 0);
    for (size_t i = 0; i < mNumShards; ++i)
    {
        mShards.emplace_back(std::make_shared<Shard const>());
    }
    mExecutor.setPreferredTaskCount(app.getConfig().BATCH_EXECUTOR_TASKS);
    bool const isNew = !fs::exists(mShardDir);
    fs::mkpath(mShardDir);
    if (isNew && !app.getConfig().DISABLE_XDR_FSYNC)
    {
        syncDir(app.getBucketManager().getBucketDir());
    }
}

ShardedTopBucket::~ShardedTopBucket()
{
    // Let the file's writer finish. It uses the clock's io context, which
    // outlives the application, and nothing else of the application, parts
    // of which (the metrics, the bucket manager's config) are gone by now.
    // The bucket manager waits for every such writer before it removes its
    // directories (BucketManager::waitForTopBucketFile); this is the last
    // resort.
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
    // The file must be THE file of its records: the metadata entry first,
    // the entries in the strict order of their keys, every record in its one
    // XDR form. The commutative hash alone does not tell a bucket file from
    // a file with the same records in another order, and whoever reads the
    // file (a merge, the index) relies on the order.
    //
    // The file is read twice at once, record by record (it may be any size:
    // catchup hands every live bucket that is not named by its SHA-256 to
    // this): once decoded, once as bytes to hold the re-encoded record
    // against.
    std::ifstream raw(filename, std::ios::binary);
    if (!raw)
    {
        throw std::runtime_error(
            fmt::format(FMT_STRING("could not open {}"), filename));
    }
    LtHash16 h;
    XDRInputFileStream in;
    in.open(filename);
    BucketEntry be;
    BucketEntry prev;
    bool first = true;
    bool havePrev = false;
    std::string expected;
    std::string found;
    BucketEntryIdCmp<LiveBucket> idCmp;
    while (in && in.readOne(be))
    {
        auto rec = xdr::xdr_to_opaque(be);
        expected.clear();
        appendRecord(expected, reinterpret_cast<char const*>(rec.data()),
                     rec.size());
        found.resize(expected.size());
        raw.read(found.data(), static_cast<std::streamsize>(found.size()));
        if (static_cast<size_t>(raw.gcount()) != found.size() ||
            found != expected)
        {
            throw std::runtime_error(fmt::format(
                FMT_STRING("{}: a record is not in its canonical form"),
                filename));
        }
        if (be.type() == METAENTRY)
        {
            if (!first)
            {
                throw std::runtime_error(fmt::format(
                    FMT_STRING("{}: a metadata entry that is not first"),
                    filename));
            }
        }
        else
        {
            if (havePrev && !idCmp(prev, be))
            {
                throw std::runtime_error(fmt::format(
                    FMT_STRING("{}: entries out of order"), filename));
            }
            prev = be;
            havePrev = true;
        }
        first = false;
        h.add(ByteSlice(rec.data(), rec.size()));
    }
    if (raw.peek() != std::ifstream::traits_type::eof())
    {
        throw std::runtime_error(fmt::format(
            FMT_STRING("{}: bytes after the last record"), filename));
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
    if (mFileWriter.valid())
    {
        // The writer reports its time here and not by itself: it may outlive
        // the application's metrics (see the destructor).
        mFileTimer.Update(mFileWriter.get());
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
    // What goes out of use in this call (the shards of the bucket that
    // spilled, of the bucket that is merged into a new one): tens of thousands
    // of entries, which the file's writer frees, off this thread.
    std::vector<std::vector<std::shared_ptr<Shard const>>> garbage;
    garbage.emplace_back(mShards);
    if (curr->getHash() != mCurrHash)
    {
        // A bucket this object did not make: the empty one after a spill, or
        // one that came from disk.
        rebuildFrom(curr);
    }
    auto prevShards = mShards;
    // What a restart needs to know of this ledger's shard files: the bucket
    // they were added to.
    std::string const prevHex = binToHex(curr->getHash());

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
                        mine[shardOfEntry(e, n)].push_back(Item{ITEM_INIT, &e});
                    }
                    else if (i < liveEnd)
                    {
                        LedgerEntry const& e = liveEntries[i - initEnd].get();
                        mine[shardOfEntry(e, n)].push_back(Item{ITEM_LIVE, &e});
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
        // A task takes the next shard that no task has taken: shards differ
        // in size, and there may be fewer or more of them than tasks.
        std::atomic<size_t> nextShard{0};
        mExecutor.executeBatchOverRanges(
            shardTasks, shardTasks, [&](size_t, size_t, size_t) {
                BucketEntryIdCmp<LiveBucket> idCmp;
                for (size_t s = nextShard.fetch_add(1); s < mNumShards;
                     s = nextShard.fetch_add(1))
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
                        out.file = fmt::format(
                            FMT_STRING("{}/shard-{}-{}-{}-{}.xdr"), mShardDir,
                            ledgerSeq, prevHex, mInstance, s);
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
        fileSize += out.shard->records.size() + 4 * out.shard->entries.size();
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
    if (doFsync && !shardFiles.empty())
    {
        syncDir(mShardDir);
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
        hash, std::move(index), fileSize, ready,
        // (What the bucket's file says: nothing, without a metadata entry.)
        hasMeta ? protocolVersion : 0, created);
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
    // (Not the bucket manager's rename: the writer may run while the
    // application is taken down.)
    std::string const bucketDir = bm.getBucketDir();
    mFileReady = ready;
    garbage.emplace_back(std::move(prevShards));
    mFileWriter = std::async(
        std::launch::async, [promise, shards, metaRecord, hasMeta, tmpName,
                             canonicalName, shardFiles, bucketDir, &ctx,
                             doFsync, garbage = std::move(garbage)]() mutable {
            auto const start = std::chrono::steady_clock::now();
            try
            {
#ifdef BUILD_TESTS
                if (gLoseFileForTesting)
                {
                    // As if the node stopped here: the ledger's shard files
                    // are there, the bucket's file is not.
                    promise->set_value();
                    return std::chrono::nanoseconds(0);
                }
#endif
                writeBucketFile(shards, hasMeta ? metaRecord.get() : nullptr,
                                tmpName, ctx, doFsync);
                bool const renamed =
                    doFsync
                        ? fs::durableRename(tmpName, canonicalName, bucketDir)
                        : std::rename(tmpName.c_str(), canonicalName.c_str()) ==
                              0;
                if (!renamed)
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
            auto const elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start);
            // Here, and not where the next writer is started.
            garbage.clear();
            shards.clear();
            return elapsed;
        });
    if (shardFiles.empty())
    {
        // A new bucket without a new entry (the batch was empty and the
        // metadata entry changed): there are no shard files that a restart
        // could rebuild it from, so its file is written before the ledger
        // goes on.
        waitForFile();
    }
    return bucket;
}

void
ShardedTopBucket::removeShardFiles(std::string const& shardDir)
{
    if (!fs::exists(shardDir))
    {
        return;
    }
    for (auto const& f : fs::findfiles(shardDir, isShardFile))
    {
        std::remove((shardDir + "/" + f).c_str());
    }
}

void
ShardedTopBucket::prepareForRestart(Application& app,
                                    HistoryArchiveState const& has,
                                    uint32_t protocolVersion)
{
    ZoneScoped;
    auto& bm = app.getBucketManager();
    std::string const shardDir = bm.getBucketDir() + "/topshards";
    Hash const hash = hexToBin256(has.currentBuckets.at(0).curr);
    if (auto b = bm.getBucketIfExists<LiveBucket>(hash))
    {
        // Made by this process: its file is there or on its way.
        b->waitForFile();
    }
    if (isZero(hash) || fs::exists(bm.bucketFilename(hash)))
    {
        // A level 0 that was written without the setting is found here and
        // not in the middle of a ledger.
        for (auto const& hex :
             {has.currentBuckets.at(0).curr, has.currentBuckets.at(0).snap})
        {
            Hash const h = hexToBin256(hex);
            if (isZero(h) || !fs::exists(bm.bucketFilename(h)))
            {
                continue;
            }
            if (bucketHash(hashOfFile(bm.bucketFilename(h))) != h)
            {
                throw std::runtime_error(fmt::format(
                    FMT_STRING(
                        "EXPERIMENTAL_SHARDED_TOP_BUCKET is set, but level 0's "
                        "bucket {} does not have the commutative hash of its "
                        "records: this node's state was written without the "
                        "setting"),
                    hex));
            }
        }
        removeShardFiles(shardDir);
        return;
    }

    // The node stopped between the ledger's commit and the end of the
    // bucket's file. The ledger's entries are in its shard files.
    std::map<std::pair<std::string, std::string>, std::vector<std::string>>
        candidates;
    if (fs::exists(shardDir))
    {
        for (auto const& f : fs::findfiles(shardDir, isShardFile))
        {
            auto parsed = parseShardFileName(f);
            if (parsed && parsed->ledger == has.currentLedger)
            {
                candidates[{parsed->prevHex, parsed->instance}].emplace_back(
                    shardDir + "/" + f);
            }
        }
    }
    bool const doFsync = !app.getConfig().DISABLE_XDR_FSYNC;
    bool const hasMeta = hasMetaEntry(protocolVersion);
    BucketEntry const bme = metaEntry(protocolVersion);
    auto const metaRecord = xdr::xdr_to_opaque(bme);
    BucketEntryIdCmp<LiveBucket> idCmp;
    for (auto const& [id, files] : candidates)
    {
        std::vector<BucketEntry> fresh;
        for (auto const& f : files)
        {
            XDRInputFileStream in;
            in.open(f);
            BucketEntry be;
            while (in && in.readOne(be))
            {
                fresh.emplace_back(be);
            }
        }
        std::sort(fresh.begin(), fresh.end(), idCmp);

        std::vector<BucketEntry> entries;
        Hash const prev = hexToBin256(id.first);
        if (isZero(prev))
        {
            entries = std::move(fresh);
        }
        else
        {
            auto prevBucket = bm.getBucketByHash<LiveBucket>(prev);
            if (!prevBucket)
            {
                CLOG_WARNING(Bucket,
                             "Shard files of ledger {} were added to bucket "
                             "{}, which is not there",
                             has.currentLedger, id.first);
                continue;
            }
            std::vector<BucketEntry> old;
            for (LiveBucketInputIterator it(prevBucket); it; ++it)
            {
                old.emplace_back(*it);
            }
            MergeCounters mc;
            entries = LiveBucket::mergeEntriesInMemory(bm, protocolVersion, old,
                                                       fresh, mc);
        }

        auto shard = std::make_shared<Shard>();
        for (auto& be : entries)
        {
            shard->append(std::move(be));
        }
        LtHash16 h = hashOfRecords(*shard);
        if (hasMeta)
        {
            h.add(ByteSlice(metaRecord.data(), metaRecord.size()));
        }
        if (bucketHash(h) != hash)
        {
            CLOG_WARNING(Bucket,
                         "Shard files of ledger {} (instance {}) do not make "
                         "bucket {}",
                         has.currentLedger, id.second, binToHex(hash));
            continue;
        }
        std::string tmpName = LiveBucket::randomBucketName(bm.getTmpDir());
        writeBucketFile({shard}, hasMeta ? &metaRecord : nullptr, tmpName,
                        app.getClock().getIOContext(), doFsync);
        if (!bm.renameBucketDirFile(tmpName, bm.bucketFilename(hash)))
        {
            throw std::runtime_error(
                fmt::format(FMT_STRING("could not rename {} to {}: {}"),
                            tmpName, bm.bucketFilename(hash), strerror(errno)));
        }
        CLOG_INFO(Bucket,
                  "Rebuilt level 0's bucket {} of ledger {} from {} shard "
                  "files",
                  binToHex(hash), has.currentLedger, files.size());
        removeShardFiles(shardDir);
        return;
    }
    // Not rebuilt: the caller's check for missing bucket files reports it.
    CLOG_ERROR(Bucket,
               "Level 0's bucket {} of ledger {} is missing and could not be "
               "rebuilt from shard files",
               binToHex(hash), has.currentLedger);
}
}
