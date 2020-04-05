#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerRange.h"
#include <cstdint>
#include <stdexcept>
#include <string>

namespace stellar
{

class HistoryManager;

// Represents a half-open range of checkpoints [first, first+count)
// where count is a count of checkpoints, not ledgers.
struct CheckpointRange final
{
    uint32_t const mFirst;
    uint32_t const mCount;
    uint32_t const mFrequency;

    CheckpointRange(uint32_t first, uint32_t count, uint32_t frequency);
    static CheckpointRange
    inclusive(uint32_t first, uint32_t last, uint32_t frequency)
    {
        // CheckpointRange is half-open: in exchange for being able to represent
        // empty ranges, it can't represent ranges that include UINT32_MAX.
        assert(last < std::numeric_limits<uint32_t>::max());

        // First and last must both be ledgers identifying checkpoints (i.e. one
        // less than multiples of frequency), and last must be >= first. The
        // resulting count will always be 1 or more since this is an inclusive
        // range.
        assert(last >= first);
        assert((first + 1) % frequency == 0);
        assert((last + 1) % frequency == 0);
        uint32_t count = 1 + ((last - first) / frequency);
        return CheckpointRange(first, count, frequency);
    }
    CheckpointRange(LedgerRange const& ledgerRange,
                    HistoryManager const& historyManager);
    friend bool operator==(CheckpointRange const& x, CheckpointRange const& y);
    friend bool operator!=(CheckpointRange const& x, CheckpointRange const& y);

    LedgerRange
    getLedgerRange() const
    {
        return LedgerRange{mFirst, getLedgerCount()};
    }

    uint32_t
    getLedgerCount() const
    {
        return mCount * mFrequency;
    }

    uint32_t
    limit() const
    {
        return mFirst + getLedgerCount();
    }

    uint32_t
    last() const
    {
        if (mCount == 0)
        {
            throw std::logic_error("last() cannot be called on "
                                   "CheckpointRange when mCount == 0");
        }
        return mFirst + ((mCount - 1) * mFrequency);
    }

    std::string toString() const;
};
}
