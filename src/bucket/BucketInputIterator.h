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
    BucketEntry const* mEntryPtr;
    XDRInputFileStream mIn;
    BucketEntry mEntry;

    void loadEntry();

  public:
    operator bool() const;

    BucketEntry const& operator*();

    BucketInputIterator(std::shared_ptr<Bucket const> bucket);

    ~BucketInputIterator();

    BucketInputIterator& operator++();
};
}
