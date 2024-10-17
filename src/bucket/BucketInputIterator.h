#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/XDRStream.h"
#include "xdr/Stellar-ledger.h"

#include <memory>
#include <type_traits>

namespace stellar
{

class Bucket;
class LiveBucket;
class HotArchiveBucket;

// Helper class that reads through the entries in a bucket.
template <typename BucketT> class BucketInputIterator
{
    static_assert(std::is_same_v<BucketT, LiveBucket> ||
                  std::is_same_v<BucketT, HotArchiveBucket>);

    using BucketEntryT = std::conditional_t<std::is_same_v<BucketT, LiveBucket>,
                                            BucketEntry, HotArchiveBucketEntry>;

    std::shared_ptr<BucketT const> mBucket;

    // Validity and current-value of the iterator is funneled into a
    // pointer. If
    // non-null, it points to mEntry.
    BucketEntryT const* mEntryPtr{nullptr};
    XDRInputFileStream mIn;
    BucketEntryT mEntry;
    bool mSeenMetadata{false};
    bool mSeenOtherEntries{false};
    BucketMetadata mMetadata;
    void loadEntry();

  public:
    operator bool() const;

    // In general, BucketInputIterators will read-and-extract the first (and
    // only) METAENTRY in a bucket if it's present, immediately upon opening the
    // input stream. This should be transparent and ignorable in most cases,
    // clients will just retrieve the _default_ BucketMetadata from the iterator
    // in case it was constructed on a pre-METAENTRY bucket. In the unusual case
    // (such as testing) where the client is interested in whether or not there
    // _was_ a METAENTRY or not, the `seenMetadata` method here will indicate.
    bool seenMetadata() const;
    BucketMetadata const& getMetadata() const;

    BucketEntryT const& operator*();

    BucketInputIterator(std::shared_ptr<BucketT const> bucket);

    ~BucketInputIterator();

    BucketInputIterator& operator++();

    std::streamoff pos();
    size_t size() const;
    void seek(std::streamoff offset);
};

typedef BucketInputIterator<LiveBucket> LiveBucketInputIterator;
typedef BucketInputIterator<HotArchiveBucket> HotArchiveBucketInputIterator;
}
