#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"
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

class CLFMaster;
class Database;

class Bucket : public std::enable_shared_from_this<Bucket>
{

    std::string const mFilename;
    uint256 const mHash;

  public:

    class InputIterator;
    class OutputIterator;

    Bucket();
    ~Bucket();

    Bucket(std::string const& filename, uint256 const& hash);

    uint256 const& getHash() const;
    std::string const& getFilename() const;

    bool containsCLFIdentity(CLFEntry const& id) const;
    std::pair<size_t, size_t> countLiveAndDeadEntries() const;

    void apply(Database& db) const;

    static std::shared_ptr<Bucket>
    fresh(CLFMaster& clfMaster,
          std::vector<LedgerEntry> const& liveEntries,
          std::vector<LedgerKey> const& deadEntries);

    static std::shared_ptr<Bucket>
    merge(CLFMaster& clfMaster,
          std::shared_ptr<Bucket> const& oldBucket,
          std::shared_ptr<Bucket> const& newBucket,
          std::vector<std::shared_ptr<Bucket>> const& shadows =
          std::vector<std::shared_ptr<Bucket>>());
};

}
