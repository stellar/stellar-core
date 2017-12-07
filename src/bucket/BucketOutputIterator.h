#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/LedgerCmp.h"
#include "util/XDRStream.h"
#include "xdr/Stellar-ledger.h"

#include <memory>
#include <string>

namespace stellar
{

class Bucket;
class BucketManager;

// Helper class that writes new elements to a file and returns a bucket
// when finished.
class BucketOutputIterator
{
    std::string mFilename;
    XDROutputFileStream mOut;
    BucketEntryIdCmp mCmp;
    std::unique_ptr<BucketEntry> mBuf;
    std::unique_ptr<SHA256> mHasher;
    size_t mBytesPut{0};
    size_t mObjectsPut{0};
    bool mKeepDeadEntries{true};

  public:
    BucketOutputIterator(std::string const& tmpDir, bool keepDeadEntries);

    void put(BucketEntry const& e);

    std::shared_ptr<Bucket> getBucket(BucketManager& bucketManager);
};
}
