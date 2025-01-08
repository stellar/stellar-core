// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/CheckpointRange.h"
#include "history/HistoryManager.h"
#include "ledger/LedgerRange.h"
#include "util/GlobalChecks.h"

#include <fmt/format.h>

namespace stellar
{

CheckpointRange::CheckpointRange(uint32_t first, uint32_t count,
                                 uint32_t frequency)
    : mFirst{first}, mCount{count}, mFrequency{frequency}
{
    releaseAssert(mFirst > 0);
    releaseAssert((mFirst + 1) % mFrequency == 0);
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
    uint32_t lastCheckpoint =
        HistoryManager::checkpointContainingLedger(r.last(), hm.getConfig());
    return 1 + ((lastCheckpoint - firstCheckpoint) /
                HistoryManager::getCheckpointFrequency(hm.getConfig()));
}
}

CheckpointRange::CheckpointRange(LedgerRange const& ledgerRange,
                                 HistoryManager const& historyManager)
    : mFirst{HistoryManager::checkpointContainingLedger(
          ledgerRange.mFirst, historyManager.getConfig())}
    , mCount{checkpointCount(mFirst, ledgerRange, historyManager)}
    , mFrequency{
          HistoryManager::getCheckpointFrequency(historyManager.getConfig())}
{
    releaseAssert(mFirst > 0);
    releaseAssert((mFirst + 1) % mFrequency == 0);
    releaseAssert(limit() >= mFirst);
    releaseAssert((limit() + 1) % mFrequency == 0);
    if (mCount != 0)
    {
        releaseAssert(last() >= mFirst);
        releaseAssert((last() + 1) % mFrequency == 0);
    }
}

std::string
CheckpointRange::toString() const
{
    return fmt::format(FMT_STRING("[{:d},{:d})"), mFirst, limit());
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
