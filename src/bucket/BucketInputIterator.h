#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/LedgerCmp.h"
#include "util/XDRStream.h"
#include "xdr/Stellar-ledger.h"

#include <memory>

namespace stellar
{

class Bucket;

// Helper class that reads through the entries in a bucket.
class BucketInputIterator
{
    std::shared_ptr<Bucket const> mBucket;

    // Validity and current-value of the iterator is funneled into a
    // pointer. If
    // non-null, it points to mEntry.
    BucketEntry const* mEntryPtr{nullptr};
    XDRInputFileStream mIn;
    BucketEntry mEntry;
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

    BucketEntry const& operator*();

    BucketInputIterator(std::shared_ptr<Bucket const> bucket);

    ~BucketInputIterator();

    BucketInputIterator& operator++();

    size_t pos();
    size_t size() const;
};
}
