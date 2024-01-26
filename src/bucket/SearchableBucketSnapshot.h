#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/NonCopyable.h"
#include "util/UnorderedMap.h"
#include "util/UnorderedSet.h"
#include "util/types.h"

#include <optional>

namespace stellar
{

class Bucket;
class XDRInputFileStream;

// A lightweight wrapper around Bucket for thread safe BucketListDB lookups
class SearchableBucketSnapshot : public NonMovable
{
    std::shared_ptr<Bucket const> mBucket;

    // Lazily-constructed and retained for read path.
    mutable std::unique_ptr<XDRInputFileStream> mStream{};

    // Returns (lazily-constructed) file stream for bucket file. Note
    // this might be in some random position left over from a previous read --
    // must be seek()'ed before use.
    XDRInputFileStream& getStream() const;

    // Loads the bucket entry for LedgerKey k. Starts at file offset pos and
    // reads until key is found or the end of the page.
    std::optional<BucketEntry> getEntryAtOffset(LedgerKey const& k,
                                                std::streamoff pos,
                                                size_t pageSize) const;

  public:
    SearchableBucketSnapshot(std::shared_ptr<Bucket const> const b);
    SearchableBucketSnapshot(SearchableBucketSnapshot const& b);
    SearchableBucketSnapshot&
    operator=(SearchableBucketSnapshot const& b) = delete;

    bool isEmpty() const;
    std::shared_ptr<Bucket const> getRawBucket() const;

    // Loads bucket entry for LedgerKey k.
    std::optional<BucketEntry> getBucketEntry(LedgerKey const& k) const;

    // Loads LedgerEntry's for given keys. When a key is found, the
    // entry is added to result and the key is removed from keys.
    void loadKeys(std::set<LedgerKey, LedgerEntryIdCmp>& keys,
                  std::vector<LedgerEntry>& result) const;

    // Loads all poolshare trustlines for the given account. Trustlines are
    // stored with their corresponding liquidity pool key in
    // liquidityPoolKeyToTrustline. All liquidity pool keys corresponding to
    // loaded trustlines are also reduntantly stored in liquidityPoolKeys.
    // If a trustline key is in deadTrustlines, it is not loaded. Whenever a
    // dead trustline is found, its key is added to deadTrustlines.
    void loadPoolShareTrustLinessByAccount(
        AccountID const& accountID, UnorderedSet<LedgerKey>& deadTrustlines,
        UnorderedMap<LedgerKey, LedgerEntry>& liquidityPoolKeyToTrustline,
        LedgerKeySet& liquidityPoolKeys) const;
};
}