// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/CheckpointRange.h"
#include "history/HistoryManager.h"
#include "ledger/LedgerRange.h"

#include <cassert>
#include <fmt/format.h>

namespace stellar
{

CheckpointRange::CheckpointRange(uint32_t first, uint32_t count,
                                 uint32_t frequency)
    : mFirst{first}, mCount{count}, mFrequency{frequency}
{
    assert(mFirst > 0);
    assert((mFirst + 1) % mFrequency == 0);
}

namespace
{
uint32_t
checkpointCount(uint32_t firstCheckpoint, LedgerRange const& r,
                HistoryManager const& hm)
{
    if (r.mCount == 0)
    {
        return 0;
    }
    uint32_t lastCheckpoint = hm.checkpointContainingLedger(r.last());
    return 1 +
           ((lastCheckpoint - firstCheckpoint) / hm.getCheckpointFrequency());
}
}

CheckpointRange::CheckpointRange(LedgerRange const& ledgerRange,
                                 HistoryManager const& historyManager)
    : mFirst{historyManager.checkpointContainingLedger(ledgerRange.mFirst)}
    , mCount{checkpointCount(mFirst, ledgerRange, historyManager)}
    , mFrequency{historyManager.getCheckpointFrequency()}
{
    assert(mFirst > 0);
    assert((mFirst + 1) % mFrequency == 0);
    assert(limit() >= mFirst);
    assert((limit() + 1) % mFrequency == 0);
    if (mCount != 0)
    {
        assert(last() >= mFirst);
        assert((last() + 1) % mFrequency == 0);
    }
}

std::string
CheckpointRange::toString() const
{
    return fmt::format("[{},{})", mFirst, limit());
}

bool
operator==(CheckpointRange const& x, CheckpointRange const& y)
{
    if (x.mFirst != y.mFirst)
    {
        return false;
    }
    if (x.mCount != y.mCount)
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
