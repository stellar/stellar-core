#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketIndex.h"
#include "crypto/Hex.h"
#include "overlay/StellarXDR.h"
#include "util/NonCopyable.h"
#include "util/ProtocolVersion.h"
#include "util/UnorderedMap.h"
#include "util/UnorderedSet.h"
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

class Bucket : public std::enable_shared_from_this<Bucket>,
               public NonMovableOrCopyable
{
    std::filesystem::path const mFilename;
    Hash const mHash;
    size_t mSize{0};
    bool mLazyIndex;

    std::unique_ptr<BucketIndex const> mIndex{};

    // Lazily-constructed and retained for read path.
    std::unique_ptr<XDRInputFileStream> mStream;

    // Returns index, throws if index not yet initialized
    BucketIndex const& getIndex() const;

    // Returns (lazily-constructed) file stream for bucket file. Note
    // this might be in some random position left over from a previous read --
    // must be seek()'ed before use.
    XDRInputFileStream& getStream();

    // Loads the bucket entry for LedgerKey k. Starts at file offset pos and
    // reads until key is found or the end of the page.
    std::optional<BucketEntry>
    getEntryAtOffset(LedgerKey const& k, std::streamoff pos, size_t pageSize);

  public:
    // Create an empty bucket. The empty bucket has hash '000000...' and its
    // filename is the empty string.
    Bucket();

    // Construct a bucket with a given filename and hash. Asserts that the file
    // exists, but does not check that the hash is the bucket's hash. Caller
    // needs to ensure that.
    Bucket(std::string const& filename, Hash const& hash,
           std::unique_ptr<BucketIndex const>&& index);

    Hash const& getHash() const;
    std::filesystem::path const& getFilename() const;
    size_t getSize() const;

    // Returns true if a BucketEntry that is key-wise identical to the given
    // BucketEntry exists in the bucket. For testing.
    bool containsBucketIdentity(BucketEntry const& id) const;

    bool isEmpty() const;

    // Delete index and close file stream
    void freeIndex();

    // Returns true if bucket is indexed, false otherwise
    bool isIndexed() const;

    // Sets index, throws if index is already set
    void setIndex(std::unique_ptr<BucketIndex const>&& index);

    // Loads bucket entry for LedgerKey k.
    std::optional<BucketEntry> getBucketEntry(LedgerKey const& k);

    // Loads LedgerEntry's for given keys. When a key is found, the
    // entry is added to result and the key is removed from keys.
    void loadKeys(std::set<LedgerKey, LedgerEntryIdCmp>& keys,
                  std::vector<LedgerEntry>& result);

    // Loads all poolshare trustlines for the given account. Trustlines are
    // stored with their corresponding liquidity pool key in
    // liquidityPoolKeyToTrustline. All liquidity pool keys corresponding to
    // loaded trustlines are also reduntantly stored in liquidityPoolKeys.
    // If a trustline key is in deadTrustlines, it is not loaded. Whenever a
    // dead trustline is found, its key is added to deadTrustlines.
    void loadPoolShareTrustLinessByAccount(
        AccountID const& accountID, UnorderedSet<LedgerKey>& deadTrustlines,
        UnorderedMap<LedgerKey, LedgerEntry>& liquidityPoolKeyToTrustline,
        LedgerKeySet& liquidityPoolKeys);

    // At version 11, we added support for INITENTRY and METAENTRY. Before this
    // we were only supporting LIVEENTRY and DEADENTRY.
    static constexpr ProtocolVersion
        FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY =
            ProtocolVersion::V_11;
    static constexpr ProtocolVersion FIRST_PROTOCOL_SHADOWS_REMOVED =
        ProtocolVersion::V_12;

    static void checkProtocolLegality(BucketEntry const& entry,
                                      uint32_t protocolVersion);

    static std::vector<BucketEntry>
    convertToBucketEntry(bool useInit,
                         std::vector<LedgerEntry> const& initEntries,
                         std::vector<LedgerEntry> const& liveEntries,
                         std::vector<LedgerKey> const& deadEntries);

    static LedgerKey getBucketLedgerKey(BucketEntry const& be);

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

    static uint32_t getBucketVersion(std::shared_ptr<Bucket> const& bucket);
    static uint32_t
    getBucketVersion(std::shared_ptr<Bucket const> const& bucket);
};
}
