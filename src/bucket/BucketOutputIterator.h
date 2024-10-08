#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketManager.h"
#include "bucket/BucketUtils.h"
#include "bucket/LedgerCmp.h"
#include "util/XDRStream.h"

#include <memory>
#include <string>

namespace stellar
{

class Bucket;
class BucketManager;

// Helper class that writes new elements to a file and returns a bucket
// when finished.
template <typename BucketT> class BucketOutputIterator
{
    BUCKET_TYPE_ASSERT(BucketT);

  protected:
    std::filesystem::path mFilename;
    XDROutputFileStream mOut;
    BucketEntryIdCmp<BucketT> mCmp;
    asio::io_context& mCtx;
    std::unique_ptr<typename BucketT::EntryT> mBuf;
    SHA256 mHasher;
    size_t mBytesPut{0};
    size_t mObjectsPut{0};
    bool mKeepTombstoneEntries{true};
    BucketMetadata mMeta;
    bool mPutMeta{false};
    MergeCounters& mMergeCounters;

  public:
    // BucketOutputIterators must _always_ be constructed with BucketMetadata,
    // regardless of the ledger version the bucket is being written from, even
    // if it's pre-METAENTRY support. The BucketOutputIterator constructor
    // inspects the metadata and determines whether it indicates a ledger
    // version new enough that it should _write_ the metadata to the stream in
    // the form of a METAENTRY; but that's not a thing the caller gets to decide
    // (or forget to do), it's handled automatically.
    BucketOutputIterator(std::string const& tmpDir, bool keepTombstoneEntries,
                         BucketMetadata const& meta, MergeCounters& mc,
                         asio::io_context& ctx, bool doFsync);

    void put(typename BucketT::EntryT const& e);

    std::shared_ptr<BucketT> getBucket(BucketManager& bucketManager,
                                       MergeKey* mergeKey = nullptr);
};
}
