// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "bucket/Bucket.h"
#include "bucket/BucketApplicator.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketOutputIterator.h"
#include "bucket/LedgerCmp.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "medida/timer.h"
#include "util/Fs.h"
#include "util/LogSlowExecution.h"
#include "util/Logging.h"
#include "util/TmpDir.h"
#include "util/XDRStream.h"
#include "xdrpp/message.h"
#include <cassert>
#include <future>

namespace stellar
{

Bucket::Bucket(std::string const& filename, Hash const& hash)
    : mFilename(filename), mHash(hash)
{
    assert(filename.empty() || fs::exists(filename));
    if (!filename.empty())
    {
        CLOG(TRACE, "Bucket")
            << "Bucket::Bucket() created, file exists : " << mFilename;
        mSize = fs::size(filename);
    }
}

Bucket::Bucket()
{
}

Hash const&
Bucket::getHash() const
{
    return mHash;
}

std::string const&
Bucket::getFilename() const
{
    return mFilename;
}

size_t
Bucket::getSize() const
{
    return mSize;
}

bool
Bucket::containsBucketIdentity(BucketEntry const& id) const
{
    BucketEntryIdCmp cmp;
    BucketInputIterator iter(shared_from_this());
    while (iter)
    {
        if (!(cmp(*iter, id) || cmp(id, *iter)))
        {
            return true;
        }
        ++iter;
    }
    return false;
}

std::pair<size_t, size_t>
Bucket::countLiveAndDeadEntries() const
{
    size_t live = 0, dead = 0;
    BucketInputIterator iter(shared_from_this());
    while (iter)
    {
        if ((*iter).type() == LIVEENTRY)
        {
            ++live;
        }
        else
        {
            ++dead;
        }
        ++iter;
    }
    return std::make_pair(live, dead);
}

void
Bucket::apply(Application& app) const
{
    BucketApplicator applicator(app, shared_from_this());
    while (applicator)
    {
        applicator.advance();
    }
}

std::vector<BucketEntry>
Bucket::convertToBucketEntry(std::vector<LedgerEntry> const& liveEntries)
{
    std::vector<BucketEntry> live;
    live.reserve(liveEntries.size());
    for (auto const& e : liveEntries)
    {
        BucketEntry ce;
        ce.type(LIVEENTRY);
        ce.liveEntry() = e;
        live.push_back(ce);
    }
    std::sort(live.begin(), live.end(), BucketEntryIdCmp());
    return live;
}

std::vector<BucketEntry>
Bucket::convertToBucketEntry(std::vector<LedgerKey> const& deadEntries)
{
    std::vector<BucketEntry> dead;
    dead.reserve(deadEntries.size());
    for (auto const& e : deadEntries)
    {
        BucketEntry ce;
        ce.type(DEADENTRY);
        ce.deadEntry() = e;
        dead.push_back(ce);
    }
    std::sort(dead.begin(), dead.end(), BucketEntryIdCmp());
    return dead;
}

std::shared_ptr<Bucket>
Bucket::fresh(BucketManager& bucketManager,
              std::vector<LedgerEntry> const& liveEntries,
              std::vector<LedgerKey> const& deadEntries)
{
    auto live = convertToBucketEntry(liveEntries);
    auto dead = convertToBucketEntry(deadEntries);

    BucketOutputIterator liveOut(bucketManager.getTmpDir(), true);
    BucketOutputIterator deadOut(bucketManager.getTmpDir(), true);
    for (auto const& e : live)
    {
        liveOut.put(e);
    }
    for (auto const& e : dead)
    {
        deadOut.put(e);
    }

    auto liveBucket = liveOut.getBucket(bucketManager);
    auto deadBucket = deadOut.getBucket(bucketManager);

    std::shared_ptr<Bucket> bucket;
    {
        auto timer = LogSlowExecution("Bucket merge");
        bucket = Bucket::merge(bucketManager, liveBucket, deadBucket);
    }
    return bucket;
}

inline void
maybePut(BucketOutputIterator& out, BucketEntry const& entry,
         std::vector<BucketInputIterator>& shadowIterators)
{
    BucketEntryIdCmp cmp;
    for (auto& si : shadowIterators)
    {
        // Advance the shadowIterator while it's less than the candidate
        while (si && cmp(*si, entry))
        {
            ++si;
        }
        // We have stepped si forward to the point that either si is exhausted,
        // or else *si >= entry; we now check the opposite direction to see if
        // we have equality.
        if (si && !cmp(entry, *si))
        {
            // If so, then entry is shadowed in at least one level and we will
            // not be doing a 'put'; we return early. There is no need to
            // advance the other iterators, they will advance as and if
            // necessary in future calls to maybePut.
            return;
        }
    }
    // Nothing shadowed.
    out.put(entry);
}

std::shared_ptr<Bucket>
Bucket::merge(BucketManager& bucketManager,
              std::shared_ptr<Bucket> const& oldBucket,
              std::shared_ptr<Bucket> const& newBucket,
              std::vector<std::shared_ptr<Bucket>> const& shadows,
              bool keepDeadEntries)
{
    // This is the key operation in the scheme: merging two (read-only)
    // buckets together into a new 3rd bucket, while calculating its hash,
    // in a single pass.

    assert(oldBucket);
    assert(newBucket);

    BucketInputIterator oi(oldBucket);
    BucketInputIterator ni(newBucket);

    std::vector<BucketInputIterator> shadowIterators(shadows.begin(),
                                                     shadows.end());

    auto timer = bucketManager.getMergeTimer().TimeScope();
    BucketOutputIterator out(bucketManager.getTmpDir(), keepDeadEntries);

    BucketEntryIdCmp cmp;
    while (oi || ni)
    {
        if (!ni)
        {
            // Out of new entries, take old entries.
            maybePut(out, *oi, shadowIterators);
            ++oi;
        }
        else if (!oi)
        {
            // Out of old entries, take new entries.
            maybePut(out, *ni, shadowIterators);
            ++ni;
        }
        else if (cmp(*oi, *ni))
        {
            // Next old-entry has smaller key, take it.
            maybePut(out, *oi, shadowIterators);
            ++oi;
        }
        else if (cmp(*ni, *oi))
        {
            // Next new-entry has smaller key, take it.
            maybePut(out, *ni, shadowIterators);
            ++ni;
        }
        else
        {
            // Old and new are for the same key, take new.
            maybePut(out, *ni, shadowIterators);
            ++oi;
            ++ni;
        }
    }
    return out.getBucket(bucketManager);
}
}
