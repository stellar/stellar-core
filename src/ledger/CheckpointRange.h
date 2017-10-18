#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <cstdint>

namespace stellar
{

class HistoryManager;
class LedgerRange;

class CheckpointRange final
{
  public:
    CheckpointRange(uint32_t first, uint32_t last, uint32_t frequency);
    CheckpointRange(LedgerRange const& ledgerRange,
                    HistoryManager const& historyManager);
    friend bool operator==(CheckpointRange const& x, CheckpointRange const& y);
    friend bool operator!=(CheckpointRange const& x, CheckpointRange const& y);

    uint32_t
    first() const
    {
        return mFirst;
    }
    uint32_t
    last() const
    {
        return mLast;
    }
    uint32_t
    frequency() const
    {
        return mFrequency;
    }
    uint32_t
    count() const
    {
        return (last() - first()) / frequency() + 1;
    }

  private:
    uint32_t mFirst;
    uint32_t mLast;
    uint32_t mFrequency;
};
}
