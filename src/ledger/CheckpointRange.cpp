// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/CheckpointRange.h"
#include "history/HistoryManager.h"
#include "ledger/LedgerRange.h"
#include <cassert>

namespace stellar
{

CheckpointRange::CheckpointRange(uint32_t first, uint32_t last,
                                 uint32_t frequency)
    : mFirst{first}, mLast{last}, mFrequency{frequency}
{
    assert(mFirst > 0);
    assert(mLast >= mFirst);
    assert((mFirst + 1) % mFrequency == 0);
    assert((mLast + 1) % mFrequency == 0);
}

CheckpointRange::CheckpointRange(LedgerRange const& ledgerRange,
                                 HistoryManager const& historyManager)
    : mFirst{historyManager.checkpointContainingLedger(ledgerRange.first())}
    , mLast{historyManager.checkpointContainingLedger(ledgerRange.last())}
    , mFrequency{historyManager.getCheckpointFrequency()}
{
    assert(mFirst > 0);
    assert(mLast >= mFirst);
    assert((mFirst + 1) % mFrequency == 0);
    assert((mLast + 1) % mFrequency == 0);
}

bool
operator==(CheckpointRange const& x, CheckpointRange const& y)
{
    if (x.mFirst != y.mFirst)
    {
        return false;
    }
    if (x.mLast != y.mLast)
    {
        return false;
    }
    if (x.mFrequency != y.mFrequency)
    {
        return false;
    }
    return true;
}

bool
operator!=(CheckpointRange const& x, CheckpointRange const& y)
{
    return !(x == y);
}
}
