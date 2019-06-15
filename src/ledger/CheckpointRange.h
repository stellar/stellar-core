#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <cstdint>
#include <string>

namespace stellar
{

class HistoryManager;
struct LedgerRange;

struct CheckpointRange final
{
    uint32_t const mFirst;
    uint32_t const mLast;
    uint32_t const mFrequency;

    CheckpointRange(uint32_t first, uint32_t last, uint32_t frequency);
    CheckpointRange(LedgerRange const& ledgerRange,
                    HistoryManager const& historyManager);
    friend bool operator==(CheckpointRange const& x, CheckpointRange const& y);
    friend bool operator!=(CheckpointRange const& x, CheckpointRange const& y);

    uint32_t
    count() const
    {
        return (mLast - mFirst) / mFrequency + 1;
    }

    std::string toString() const;
};
}
