// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/HotArchiveBucket.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketOutputIterator.h"
#include "bucket/BucketUtils.h"

namespace stellar
{

std::shared_ptr<HotArchiveBucket>
HotArchiveBucket::fresh(BucketManager& bucketManager, uint32_t protocolVersion,
                        std::vector<LedgerEntry> const& archivedEntries,
                        std::vector<LedgerKey> const& restoredEntries,
                        std::vector<LedgerKey> const& deletedEntries,
                        bool countMergeEvents, asio::io_context& ctx,
                        bool doFsync)
{
    ZoneScoped;
    BucketMetadata meta;
    meta.ledgerVersion = protocolVersion;
    meta.ext.v(1);
    meta.ext.bucketListType() = BucketListType::HOT_ARCHIVE;
    auto entries =
        convertToBucketEntry(archivedEntries, restoredEntries, deletedEntries);

    MergeCounters mc;
    HotArchiveBucketOutputIterator out(bucketManager.getTmpDir(), true, meta,
                                       mc, ctx, doFsync);
    for (auto const& e : entries)
    {
        out.put(e);
    }

    if (countMergeEvents)
    {
        bucketManager.incrMergeCounters(mc);
    }

    return out.getBucket(bucketManager);
}

std::vector<HotArchiveBucketEntry>
HotArchiveBucket::convertToBucketEntry(
    std::vector<LedgerEntry> const& archivedEntries,
    std::vector<LedgerKey> const& restoredEntries,
    std::vector<LedgerKey> const& deletedEntries)
{
    std::vector<HotArchiveBucketEntry> bucket;
    for (auto const& e : archivedEntries)
    {
        HotArchiveBucketEntry be;
        be.type(HOT_ARCHIVE_ARCHIVED);
        be.archivedEntry() = e;
        bucket.push_back(be);
    }
    for (auto const& k : restoredEntries)
    {
        HotArchiveBucketEntry be;
        be.type(HOT_ARCHIVE_LIVE);
        be.key() = k;
        bucket.push_back(be);
    }
    for (auto const& k : deletedEntries)
    {
        HotArchiveBucketEntry be;
        be.type(HOT_ARCHIVE_DELETED);
        be.key() = k;
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
    HotArchiveBucketOutputIterator& out, HotArchiveBucketEntry const& entry,
    std::vector<HotArchiveBucketInputIterator>& shadowIterators,
    bool keepShadowedLifecycleEntries, MergeCounters& mc)
{
    // Archived BucketList is only present after protocol 21, so shadows are
    // never supported
    out.put(entry);
}

void
HotArchiveBucket::mergeCasesWithEqualKeys(
    MergeCounters& mc, HotArchiveBucketInputIterator& oi,
    HotArchiveBucketInputIterator& ni, HotArchiveBucketOutputIterator& out,
    std::vector<HotArchiveBucketInputIterator>& shadowIterators,
    uint32_t protocolVersion, bool keepShadowedLifecycleEntries)
{
    // If two identical keys have the same type, throw an error. Otherwise,
    // take the newer key.
    HotArchiveBucketEntry const& oldEntry = *oi;
    HotArchiveBucketEntry const& newEntry = *ni;
    if (oldEntry.type() == newEntry.type())
    {
        throw std::runtime_error(
            "Malformed Hot Archive bucket: two identical keys with "
            "the same type.");
    }

    out.put(newEntry);
    ++ni;
    ++oi;
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

}