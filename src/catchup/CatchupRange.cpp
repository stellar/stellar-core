// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/CatchupRange.h"
#include "catchup/CatchupConfiguration.h"
#include "history/HistoryManager.h"
#include "ledger/LedgerManager.h"
#include "util/Logging.h"
#include <lib/util/format.h>

namespace
{
using namespace stellar;
void
checkCatchupPreconditions(uint32_t lastClosedLedger,
                          CatchupConfiguration const& configuration)
{
    if (lastClosedLedger == 0)
    {
        throw std::invalid_argument{"lastClosedLedger == 0"};
    }

    if (configuration.toLedger() <= lastClosedLedger)
    {
        throw std::invalid_argument{
            "configuration.toLedger() <= lastClosedLedger"};
    }

    if (configuration.toLedger() <= LedgerManager::GENESIS_LEDGER_SEQ)
    {
        throw std::invalid_argument{
            "configuration.toLedger() <= LedgerManager::GENESIS_LEDGER_SEQ"};
    }

    if (configuration.toLedger() == CatchupConfiguration::CURRENT)
    {
        throw std::invalid_argument{
            "configuration.toLedger() == CatchupConfiguration::CURRENT"};
    }
}

CatchupRange
calculateCatchupRange(uint32_t lcl, CatchupConfiguration const& cfg,
                      HistoryManager const& hm)
{
    checkCatchupPreconditions(lcl, cfg);
    const uint32_t init = LedgerManager::GENESIS_LEDGER_SEQ;

    // Case 1: replay from LCL as we're already past genesis.
    if (lcl > init)
    {
        LedgerRange replay(lcl + 1, cfg.toLedger() - lcl);
        // CLOG(TRACE, "History") << "CatchupRange: case 1 (lcl is past
        // genesis)";
        return CatchupRange(replay);
    }

    // All remaining cases have LCL == genesis.
    assert(lcl == init);
    LedgerRange fullReplay(init + 1, cfg.toLedger() - init);

    // Case 2: full replay because count >= target - init.
    if (cfg.count() >= cfg.toLedger() - init)
    {
        // CLOG(TRACE, "History") << "CatchupRange: case 2 (count passes
        // target)";
        return CatchupRange(fullReplay);
    }

    // Case 3: special case of buckets only, no replay; only
    // possible when targeting the exact end of a checkpoint.
    if (cfg.count() == 0 && hm.isLastLedgerInCheckpoint(cfg.toLedger()))
    {
        uint32_t applyBucketsOnlyAt = cfg.toLedger();
        // CLOG(TRACE, "History") << "CatchupRange: case 3 (checkpoint
        // boundary)";
        return CatchupRange(applyBucketsOnlyAt);
    }

    uint32_t targetStart = cfg.toLedger() - cfg.count();
    uint32_t firstInCheckpoint =
        hm.firstLedgerInCheckpointContaining(targetStart);

    // Case 4: target is inside first checkpoint, just replay.
    if (firstInCheckpoint == init)
    {
        // CLOG(TRACE, "History") << "CatchupRange: case 4 (in first
        // checkpoint)";
        return CatchupRange(fullReplay);
    }

    // Case 5: apply buckets, then replay.
    uint32_t applyBucketsAt =
        hm.lastLedgerBeforeCheckpointContaining(targetStart);
    LedgerRange replay(firstInCheckpoint, cfg.toLedger() - applyBucketsAt);
    // CLOG(TRACE, "History") << "CatchupRange: case 5 (apply buckets and
    // replay)";
    return CatchupRange(applyBucketsAt, replay);
}
}

namespace stellar
{

CatchupRange::CatchupRange(uint32_t applyBucketsAtLedger)
    : mApplyBuckets(true)
    , mApplyBucketsAtLedger(applyBucketsAtLedger)
    , mReplayRange(0, 0)
{
    // CLOG(TRACE, "History") << "CatchupRange: apply buckets only at "
    //                        << mApplyBucketsAtLedger;
    checkInvariants();
}

CatchupRange::CatchupRange(LedgerRange const& replayRange)
    : mApplyBuckets(false), mApplyBucketsAtLedger(0), mReplayRange(replayRange)
{
    // CLOG(TRACE, "History") << "CatchupRange: no buckets"
    //                        << ", replayFirst=" << getReplayFirst()
    //                        << ", replayCount=" << getReplayCount();
    checkInvariants();
}

CatchupRange::CatchupRange(uint32_t applyBucketsAtLedger,
                           LedgerRange const& replayRange)
    : mApplyBuckets(true)
    , mApplyBucketsAtLedger(applyBucketsAtLedger)
    , mReplayRange(replayRange)
{
    // CLOG(TRACE, "History") << "CatchupRange: apply buckets at "
    //                        << mApplyBucketsAtLedger
    //                        << ", replayFirst=" << getReplayFirst()
    //                        << ", replayCount=" << getReplayCount();
    checkInvariants();
}

CatchupRange::CatchupRange(uint32_t lastClosedLedger,
                           CatchupConfiguration const& configuration,
                           HistoryManager const& historyManager)
    : CatchupRange(calculateCatchupRange(lastClosedLedger, configuration,
                                         historyManager))
{
    checkInvariants();
}

void
CatchupRange::checkInvariants()
{
    // Must be applying buckets and/or replaying.
    assert(applyBuckets() || replayLedgers());

    if (!applyBuckets() && replayLedgers())
    {
        // Cases 1, 2 and 4: no buckets, only replay.
        assert(mApplyBucketsAtLedger == 0);
        assert(mReplayRange.mFirst != 0);
    }

    else if (applyBuckets() && replayLedgers())
    {
        // Case 5: buckets and replay.
        assert(mApplyBucketsAtLedger != 0);
        assert(mReplayRange.mFirst != 0);
        assert(mApplyBucketsAtLedger + 1 == mReplayRange.mFirst);
    }

    else
    {
        // Case 3: buckets only, no replay.
        assert(applyBuckets() && !replayLedgers());
        assert(mReplayRange.mFirst == 0);
    }
}
}
