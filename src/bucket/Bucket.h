#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/LedgerCmp.h"
#include "crypto/Hex.h"
#include "overlay/StellarXDR.h"
#include "util/NonCopyable.h"
#include "util/XDRStream.h"
#include <string>

namespace medida
{
class MetricsRegistry;
}

namespace stellar
{

/**
 * Bucket is an immutable container for a sorted set of "Entries" (object ID,
 * hash, xdr-message tuples) which is designed to be held in a shared_ptr<>
 * which is referenced between threads, to minimize copying. It is therefore
 * imperative that it be _really_ immutable, not just faking it.
 *
 * Two buckets can be merged together efficiently (in a single pass): elements
 * from the newer bucket overwrite elements from the older bucket, the rest are
 * merged in sorted order, and all elements are hashed while being added.
 */

class BucketManager;
class BucketList;
class Database;

class Bucket : public std::enable_shared_from_this<Bucket>,
               public NonMovableOrCopyable
{

    std::string const mFilename;
    Hash const mHash;

  public:
    // Create an empty bucket. The empty bucket has hash '000000...' and its
    // filename is the empty string.
    Bucket();

    // Construct a bucket with a given filename and hash. Asserts that the file
    // exists, but does not check that the hash is the bucket's hash. Caller
    // needs to ensure that.
    Bucket(std::string const& filename, Hash const& hash);

    Hash const& getHash() const;
    std::string const& getFilename() const;

    // Returns true if a BucketEntry that is key-wise identical to the given
    // BucketEntry exists in the bucket. For testing.
    bool containsBucketIdentity(BucketEntry const& id) const;

    // Return the count of live and dead BucketEntries in the bucket. For
    // testing.
    std::pair<size_t, size_t> countLiveAndDeadEntries() const;

    // "Applies" the bucket to the database. For each entry in the bucket, if
    // the entry is live, creates or updates the corresponding entry in the
    // database; if the entry is dead (a tombstone), deletes the corresponding
    // entry in the database.
    void apply(Database& db) const;

    // Create a fresh bucket from a given vector of live LedgerEntries and
    // dead LedgerEntryKeys. The bucket will be sorted, hashed, and adopted
    // in the provided BucketManager.
    static std::shared_ptr<Bucket>
    fresh(BucketManager& bucketManager,
          std::vector<LedgerEntry> const& liveEntries,
          std::vector<LedgerKey> const& deadEntries);

    // Merge two buckets together, producing a fresh one. Entries in `oldBucket`
    // are overridden in the fresh bucket by keywise-equal entries in
    // `newBucket`. Entries are inhibited from the fresh bucket by keywise-equal
    // entries in any of the buckets in the provided `shadows` vector.
    static std::shared_ptr<Bucket>
    merge(BucketManager& bucketManager,
          std::shared_ptr<Bucket> const& oldBucket,
          std::shared_ptr<Bucket> const& newBucket,
          std::vector<std::shared_ptr<Bucket>> const& shadows =
              std::vector<std::shared_ptr<Bucket>>(),
          bool keepDeadEntries = true);
};

void checkDBAgainstBuckets(medida::MetricsRegistry& metrics,
                           BucketManager& bucketManager, Database& db,
                           BucketList& bl);
}
