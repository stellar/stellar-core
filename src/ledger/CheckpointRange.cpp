// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/CheckpointRange.h"
#include "history/HistoryManager.h"
#include "util/GlobalChecks.h"

#include <fmt/format.h>
#include <limits>
#include <stdexcept>

namespace stellar
{

CheckpointRange::CheckpointRange(uint32_t first, uint32_t count,
                                 uint32_t frequency)
    : mFirst{first}, mCount{count}, mFrequency{frequency}
{
    releaseAssert(mFirst > 0);
    releaseAssert((mFirst + 1) % mFrequency == 0);
}

CheckpointRange
CheckpointRange::inclusive(uint32_t first, uint32_t last, uint32_t frequency)
{
    // CheckpointRange is half-open: in exchange for being able to represent
    // empty ranges, it can't represent ranges that include UINT32_MAX.
    releaseAssert(last < std::numeric_limits<uint32_t>::max());

    // First and last must both be ledgers identifying checkpoints (i.e. one
    // less than multiples of frequency), and last must be >= first. The
    // resulting count will always be 1 or more since this is an inclusive
    // range.
    releaseAssert(last >= first);
    releaseAssert((first + 1) % frequency == 0);
    releaseAssert((last + 1) % frequency == 0);
    uint32_t count = 1 + ((last - first) / frequency);
    return CheckpointRange(first, count, frequency);
}

uint32_t
CheckpointRange::last() const
{
    if (mCount == 0)
    {
        throw std::logic_error("last() cannot be called on "
                               "CheckpointRange when mCount == 0");
    }
    return mFirst + ((mCount - 1) * mFrequency);
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
