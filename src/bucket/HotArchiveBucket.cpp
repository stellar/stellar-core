// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/HotArchiveBucket.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketMergeAdapter.h"
#include "bucket/BucketOutputIterator.h"
#include "bucket/BucketUtils.h"
#include "ledger/LedgerTypeUtils.h"
#include "util/GlobalChecks.h"

namespace stellar
{

std::shared_ptr<HotArchiveBucket>
HotArchiveBucket::fresh(BucketManager& bucketManager, uint32_t protocolVersion,
                        std::vector<LedgerEntry> const& archivedEntries,
                        std::vector<LedgerKey> const& restoredEntries,
                        bool countMergeEvents, asio::io_context& ctx,
                        bool doFsync)
{
    ZoneScoped;
    BucketMetadata meta;
    meta.ledgerVersion = protocolVersion;
    meta.ext.v(1);
    meta.ext.bucketListType() = BucketListType::HOT_ARCHIVE;
    auto entries = convertToBucketEntry(archivedEntries, restoredEntries);

    MergeCounters mc;
    HotArchiveBucketOutputIterator out(bucketManager.getTmpDir(), true, meta,
                                       mc, ctx, doFsync);
    for (auto const& e : entries)
    {
        out.put(e);
    }

    if (countMergeEvents)
    {
        bucketManager.incrMergeCounters<HotArchiveBucket>(mc);
    }

    return out.getBucket(bucketManager);
}

std::vector<HotArchiveBucketEntry>
HotArchiveBucket::convertToBucketEntry(
    std::vector<LedgerEntry> const& archivedEntries,
    std::vector<LedgerKey> const& restoredEntries)
{
    std::vector<HotArchiveBucketEntry> bucket;
    for (auto const& e : archivedEntries)
    {
        HotArchiveBucketEntry be;
        be.type(HOT_ARCHIVE_ARCHIVED);
        be.archivedEntry() = e;
        releaseAssertOrThrow(isPersistentEntry(e.data));
        bucket.push_back(be);
    }
    for (auto const& k : restoredEntries)
    {
        HotArchiveBucketEntry be;
        be.type(HOT_ARCHIVE_LIVE);
        be.key() = k;
        releaseAssertOrThrow(isPersistentEntry(k));
        bucket.push_back(be);
    }

    BucketEntryIdCmp<HotArchiveBucket> cmp;
    std::sort(bucket.begin(), bucket.end(), cmp);
    releaseAssert(std::adjacent_find(bucket.begin(), bucket.end(),
                                     [&cmp](HotArchiveBucketEntry const& lhs,
                                            HotArchiveBucketEntry const& rhs) {
                                         return !cmp(lhs, rhs);
                                     }) == bucket.end());
    return bucket;
}

void
HotArchiveBucket::maybePut(
    std::function<void(HotArchiveBucketEntry const&)> putFunc,
    HotArchiveBucketEntry const& entry, MergeCounters& mc)
{
    putFunc(entry);
}

template <typename InputSource>
void
HotArchiveBucket::mergeCasesWithEqualKeys(
    MergeCounters& mc, InputSource& inputSource,
    std::function<void(HotArchiveBucketEntry const&)> putFunc,
    uint32_t protocolVersion)
{
    // Always take the newer entry.
    putFunc(inputSource.getNewEntry());
    inputSource.advanceNew();
    inputSource.advanceOld();
}

uint32_t
HotArchiveBucket::getBucketVersion() const
{
    HotArchiveBucketInputIterator it(shared_from_this());
    return it.getMetadata().ledgerVersion;
}

HotArchiveBucket::HotArchiveBucket(
    std::string const& filename, Hash const& hash,
    std::unique_ptr<HotArchiveBucket::IndexT const>&& index)
    : BucketBase(filename, hash, std::move(index))
{
}

HotArchiveBucket::HotArchiveBucket() : BucketBase()
{
}

bool
HotArchiveBucket::isTombstoneEntry(HotArchiveBucketEntry const& e)
{
    return e.type() == HOT_ARCHIVE_LIVE;
}

std::shared_ptr<HotArchiveBucket::LoadT const>
HotArchiveBucket::bucketEntryToLoadResult(
    std::shared_ptr<EntryT const> const& be)
{
    return isTombstoneEntry(*be) ? nullptr : be;
}

template void
HotArchiveBucket::mergeCasesWithEqualKeys<FileMergeInput<HotArchiveBucket>>(
    MergeCounters& mc, FileMergeInput<HotArchiveBucket>& inputSource,
    std::function<void(HotArchiveBucketEntry const&)> putFunc,
    uint32_t protocolVersion);

template void
HotArchiveBucket::mergeCasesWithEqualKeys<MemoryMergeInput<HotArchiveBucket>>(
    MergeCounters& mc, MemoryMergeInput<HotArchiveBucket>& inputSource,
    std::function<void(HotArchiveBucketEntry const&)> putFunc,
    uint32_t protocolVersion);
}
