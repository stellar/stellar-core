#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/HashID.h"
#include "bucket/LedgerCmp.h"
#include "crypto/Hex.h"
#include "overlay/StellarXDR.h"
#include "util/NonCopyable.h"
#include "util/ProtocolVersion.h"
#include "util/XDRStream.h"
#include <optional>
#include <string>

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

class Application;
class BucketManager;
class BucketList;
class Database;

class Bucket : public std::enable_shared_from_this<Bucket>,
               public NonMovableOrCopyable
{
    std::optional<std::filesystem::path const> mSortByTypeFilename;
    std::optional<std::filesystem::path const> mSortByAccountFilename;
    std::optional<Hash const> mSortByTypeHash;
    std::optional<Hash const> mSortByAccountHash;
    size_t mSize{0};

  public:
    // Create an empty bucket. The empty bucket has hash '000000...' and its
    // filename is the empty string.
    Bucket();

    // Construct a bucket with a single filename and hash. Asserts that the file
    // exists, but does not check that the hash is the bucket's hash. Caller
    // needs to ensure that.
    Bucket(std::filesystem::path const& filename, Hash const& hash,
           BucketSortOrder type);

    // Associates a file with the given sort order and hash to the bucket.
    // Asserts that the file exists, but does not check that the hash is the
    // bucket's hash. Caller needs to ensure that. Bucket must not already be
    // associated with a file of the given type when this funciton is called.
    void addFile(std::filesystem::path const& filename, Hash const& hash,
                 BucketSortOrder type);

    // Drop reference to file of the given sort order, if it exists
    // Note: This does not delete the file, BucketManager should handle that
    void dropFile(BucketSortOrder type);

    // Returns a valid sort order for the bucket
    BucketSortOrder getValidType() const;

    std::optional<Hash const> const& getHash(BucketSortOrder type) const;
    std::optional<std::filesystem::path const> const&
    getFilename(BucketSortOrder type) const;

    // Returns a hash to identify this bucket internally. Can be any hash type.
    HashID const getHashID() const;

    // Returns the bucket hash in whichever sort order is required by the given
    // protocol version
    Hash const getHashByProtocol(uint32_t protocolVersion) const;

    // Returns true if the bucket does not have any hashes or files assocaited
    // with it, false otherwise
    bool isEmpty() const;

    size_t getSize() const;
    bool hasFileWithSortOrder(BucketSortOrder type) const;

    // Returns true if a BucketEntry that is key-wise identical to the given
    // BucketEntry exists in the bucket. For testing.
    bool containsBucketIdentity(BucketEntry const& id) const;

    // At version 11, we added support for INITENTRY and METAENTRY. Before this
    // we were only supporting LIVEENTRY and DEADENTRY.
    static constexpr ProtocolVersion
        FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY =
            ProtocolVersion::V_11;
    static constexpr ProtocolVersion FIRST_PROTOCOL_SHADOWS_REMOVED =
        ProtocolVersion::V_12;

    // Buckets with protocol version 20 and higher are sorted by account instead
    // of by type
    static constexpr ProtocolVersion FIRST_PROTOCOL_SORTED_BY_ACCOUNT =
        ProtocolVersion::V_20;

    static void checkProtocolLegality(BucketEntry const& entry,
                                      uint32_t protocolVersion);

    static std::vector<BucketEntry> convertToBucketEntry(
        bool useInit, std::vector<LedgerEntry> const& initEntries,
        std::vector<LedgerEntry> const& liveEntries,
        std::vector<LedgerKey> const& deadEntries, uint32_t protocolVersion);

    // Returns sort order of given file
    static BucketSortOrder getFileType(std::filesystem::path filename);

    // Returns sort order that should be used to calculate the LedgerHeader hash
    // for the given protocol version
    static BucketSortOrder protocolSortOrder(uint32_t protocolVersion);
#ifdef BUILD_TESTS
    // "Applies" the bucket to the database. For each entry in the bucket,
    // if the entry is init or live, creates or updates the corresponding
    // entry in the database (respectively; if the entry is dead (a
    // tombstone), deletes the corresponding entry in the database.
    void apply(Application& app) const;
#endif // BUILD_TESTS

    // Create a fresh bucket from given vectors of init (created) and live
    // (updated) LedgerEntries, and dead LedgerEntryKeys. The bucket will
    // be sorted, hashed, and adopted in the provided BucketManager.
    static std::shared_ptr<Bucket>
    fresh(BucketManager& bucketManager, uint32_t protocolVersion,
          std::vector<LedgerEntry> const& initEntries,
          std::vector<LedgerEntry> const& liveEntries,
          std::vector<LedgerKey> const& deadEntries, bool countMergeEvents,
          asio::io_context& ctx, bool doFsync);

    // Merge two buckets together, producing a fresh one. Entries in `oldBucket`
    // are overridden in the fresh bucket by keywise-equal entries in
    // `newBucket`. Entries are inhibited from the fresh bucket by keywise-equal
    // entries in any of the buckets in the provided `shadows` vector.
    //
    // Each bucket is self-describing in terms of the ledger protocol version it
    // was constructed under, and the merge algorithm adjusts to the maximum of
    // the versions attached to each input or shadow bucket. The provided
    // `maxProtocolVersion` bounds this (for error checking) and should usually
    // be the protocol of the ledger header at which the merge is starting. An
    // exception will be thrown if any provided bucket versions exceed it.
    static std::shared_ptr<Bucket>
    merge(BucketManager& bucketManager, uint32_t maxProtocolVersion,
          std::shared_ptr<Bucket> const& oldBucket,
          std::shared_ptr<Bucket> const& newBucket,
          std::vector<std::shared_ptr<Bucket>> const& shadows,
          bool keepDeadEntries, bool countMergeEvents, asio::io_context& ctx,
          bool doFsync);

    // Returns oldest protocol number associated with bucket
    static uint32_t getBucketVersion(std::shared_ptr<Bucket const> bucket);
};
}
