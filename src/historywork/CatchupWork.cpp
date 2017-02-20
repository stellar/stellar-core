// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/CatchupWork.h"
#include "history/HistoryManager.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "lib/util/format.h"
#include "main/Application.h"

namespace stellar
{

CatchupWork::CatchupWork(Application& app, WorkParent& parent,
                         uint32_t initLedger, std::string const& mode,
                         bool manualCatchup)
    : BucketDownloadWork(
          app, parent, fmt::format("catchup-{:s}-{:08x}", mode, initLedger),
          app.getHistoryManager().getLastClosedHistoryArchiveState())
    , mInitLedger(initLedger)
    , mManualCatchup(manualCatchup)
{
}

uint32_t
CatchupWork::nextLedger() const
{
    return mManualCatchup
               ? mInitLedger
               : mApp.getHistoryManager().nextCheckpointLedger(mInitLedger);
}

uint32_t
CatchupWork::archiveStateSeq() const
{
    return firstCheckpointSeq();
}

uint32_t
CatchupWork::lastCheckpointSeq() const
{
    return nextLedger() - 1;
}

void
CatchupWork::onReset()
{
    BucketDownloadWork::onReset();
    uint64_t sleepSeconds =
        mManualCatchup ? 0
                       : mApp.getHistoryManager().nextCheckpointCatchupProbe(
                             lastCheckpointSeq());
    mGetHistoryArchiveStateWork = addWork<GetHistoryArchiveStateWork>(
        mRemoteState, archiveStateSeq(), std::chrono::seconds(sleepSeconds));
}
}
