#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/GlobalChecks.h"
#include "util/XDROperators.h" // IWYU pragma: keep
#include "xdr/Stellar-ledger-entries.h"
#include <filesystem>
#include <map>
#include <optional>
#include <variant>
#include <vector>

#include "util/XDRCereal.h"
#include <cereal/archives/binary.hpp>
#include <cereal/types/utility.hpp>
#include <cereal/types/vector.hpp>

namespace asio
{
class io_context;
}

namespace stellar
{

class BucketManager;
class Config;

using AssetPoolIDMap = std::map<Asset, std::vector<PoolID>>;
using IndexPtrT = std::shared_ptr<BucketEntry const>;

// Querying a BucketIndex can return one of three states:
// 1. CACHE_HIT: The entry is in the cache. Can either be a live or dead entry.
// 2. FILE_OFFSET: The entry is not in the cache, but the entry potentially
//    exists at the given offset.
// 3. NOT_FOUND: The entry does not exist in the bucket.
enum IndexReturnState
{
    CACHE_HIT,
    FILE_OFFSET,
    NOT_FOUND
};

class IndexReturnT
{
  private:
    // Payload maps to the possible return states:
    // CACHE_HIT: IndexPtrT
    // FILE_OFFSET: std::streamoff
    // NOT_FOUND: std::monostate
    using PayloadT = std::variant<IndexPtrT, std::streamoff, std::monostate>;
    PayloadT mPayload;
    IndexReturnState mState;

  public:
    IndexReturnT(IndexPtrT entry)
        : mPayload(entry), mState(IndexReturnState::CACHE_HIT)
    {
        releaseAssertOrThrow(entry);
    }
    IndexReturnT(std::streamoff offset)
        : mPayload(offset), mState(IndexReturnState::FILE_OFFSET)
    {
    }
    IndexReturnT()
        : mPayload(std::monostate{}), mState(IndexReturnState::NOT_FOUND)
    {
    }

    IndexReturnState
    getState() const
    {
        return mState;
    }
    IndexPtrT
    cacheHit() const
    {
        releaseAssertOrThrow(mState == IndexReturnState::CACHE_HIT);
        releaseAssertOrThrow(std::holds_alternative<IndexPtrT>(mPayload));
        return std::get<IndexPtrT>(mPayload);
    }
    std::streamoff
    fileOffset() const
    {
        releaseAssertOrThrow(mState == IndexReturnState::FILE_OFFSET);
        releaseAssertOrThrow(std::holds_alternative<std::streamoff>(mPayload));
        return std::get<std::streamoff>(mPayload);
    }
};

// Returns pagesize, in bytes, from BUCKETLIST_DB_INDEX_CUTOFF config param
std::streamoff getPageSizeFromConfig(Config const& cfg);

// Builds index for given bucketfile. This is expensive (> 20 seconds
// for the largest buckets) and should only be called once. Constructs a
// DiskIndex or InMemoryIndex depending on config and Bucket size.
template <class BucketT>
std::unique_ptr<typename BucketT::IndexT const>
createIndex(BucketManager& bm, std::filesystem::path const& filename,
            Hash const& hash, asio::io_context& ctx);

// Loads index from given file. If file does not exist or if saved
// index does not have expected version or pageSize, return null
template <class BucketT>
std::unique_ptr<typename BucketT::IndexT const>
loadIndex(BucketManager const& bm, std::filesystem::path const& filename,
          std::size_t fileSize);
}
