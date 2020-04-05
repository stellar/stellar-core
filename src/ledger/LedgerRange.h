#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/optional.h"
#include "xdr/Stellar-types.h"
#include <cstdint>
#include <stdexcept>

namespace stellar
{
// Ledger seq num + hash pair, a lightweight substitute of ledger
// history entry, useful for catchup and ledger verification purposes.
using LedgerNumHashPair = std::pair<uint32_t, optional<Hash>>;

// Represents a half-open range of ledgers [first, first+count).
// If count is zero, this represents _no_ ledgers.
struct LedgerRange final
{
  public:
    uint32_t const mFirst;
    uint32_t const mCount;

    LedgerRange(uint32_t first, uint32_t count);
    static LedgerRange
    inclusive(uint32_t first, uint32_t last)
    {
        // LedgerRange is half-open: in exchange for being able to represent
        // empty ranges, it can't represent ranges that include UINT32_MAX.
        assert(last < std::numeric_limits<uint32_t>::max());
        return LedgerRange(first, last - first + 1);
    }
    std::string toString() const;

    // Return first+count, which is the _exclusive_ range limit.
    // Best to use this in a loop header to properly handle count=0.
    uint32_t
    limit() const
    {
        return mFirst + mCount;
    }

    // Return first+count-1 unless count == 0, in which case throw.
    // This is the _inclusive_ range limit, meaningful iff count != 0.
    uint32_t
    last() const
    {
        if (mCount == 0)
        {
            throw std::logic_error("last() cannot be called on "
                                   "LedgerRange when mCount == 0");
        }
        return limit() - 1;
    }

    friend bool operator==(LedgerRange const& x, LedgerRange const& y);
    friend bool operator!=(LedgerRange const& x, LedgerRange const& y);
};
}
