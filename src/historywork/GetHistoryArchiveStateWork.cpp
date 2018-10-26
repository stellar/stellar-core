// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/GetHistoryArchiveStateWork.h"
#include "history/HistoryArchive.h"
#include "historywork/GetRemoteFileWork.h"
#include "ledger/LedgerManager.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "util/Logging.h"
#include <medida/meter.h>
#include <medida/metrics_registry.h>

namespace stellar
{
GetHistoryArchiveStateWork::GetHistoryArchiveStateWork(
    Application& app, HistoryArchiveState& state, uint32_t seq,
    std::shared_ptr<HistoryArchive> archive, size_t maxRetries)
    : Work(app, "get-archive-state", maxRetries)
    , mState(state)
    , mSeq(seq)
    , mArchive(archive)
    , mLocalFilename(
          archive ? HistoryArchiveState::localName(app, archive->getName())
                  : app.getHistoryManager().localFilename(
                        HistoryArchiveState::baseName()))
    , mGetHistoryArchiveStateStart(app.getMetrics().NewMeter(
          {"history", "download-history-archive-state", "start"}, "event"))
    , mGetHistoryArchiveStateSuccess(app.getMetrics().NewMeter(
          {"history", "download-history-archive-state", "success"}, "event"))
    , mGetHistoryArchiveStateFailure(app.getMetrics().NewMeter(
          {"history", "download-history-archive-state", "failure"}, "event"))
{
}

BasicWork::State
GetHistoryArchiveStateWork::doWork()
{
    if (mGetRemoteFile)
    {
        auto state = mGetRemoteFile->getState();
        if (state == State::WORK_SUCCESS)
        {
            try
            {
                mState.load(mLocalFilename);
            }
            catch (std::runtime_error& e)
            {
                CLOG(ERROR, "History")
                    << "error loading history state: " << e.what();
                return State::WORK_FAILURE;
            }
        }
        return state;
    }
    else
    {
        auto name = mSeq == 0 ? HistoryArchiveState::wellKnownRemoteName()
                              : HistoryArchiveState::remoteName(mSeq);
        mGetRemoteFile = addWork<GetRemoteFileWork>(name, mLocalFilename,
                                                    mArchive, getMaxRetries());
        mGetHistoryArchiveStateStart.Mark();
        return State::WORK_RUNNING;
    }
}

void
GetHistoryArchiveStateWork::doReset()
{
    mGetRemoteFile.reset();
    std::remove(mLocalFilename.c_str());
}

void
GetHistoryArchiveStateWork::onFailureRaise()
{
    mGetHistoryArchiveStateFailure.Mark();
    Work::onFailureRaise();
}

void
GetHistoryArchiveStateWork::onSuccess()
{
    mGetHistoryArchiveStateSuccess.Mark();
    Work::onSuccess();
}
}
