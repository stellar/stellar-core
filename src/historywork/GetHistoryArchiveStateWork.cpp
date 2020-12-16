// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/GetHistoryArchiveStateWork.h"
#include "history/HistoryArchive.h"
#include "historywork/GetRemoteFileWork.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "main/ErrorMessages.h"
#include "util/Logging.h"
#include <Tracy.hpp>
#include <fmt/format.h>
#include <medida/meter.h>
#include <medida/metrics_registry.h>

namespace stellar
{
GetHistoryArchiveStateWork::GetHistoryArchiveStateWork(
    Application& app, uint32_t seq, std::shared_ptr<HistoryArchive> archive,
    std::string mode, size_t maxRetries)
    : Work(app, "get-archive-state", maxRetries)
    , mSeq(seq)
    , mArchive(archive)
    , mRetries(maxRetries)
    , mLocalFilename(
          archive ? HistoryArchiveState::localName(app, archive->getName())
                  : app.getHistoryManager().localFilename(
                        HistoryArchiveState::baseName()))
    , mGetHistoryArchiveStateSuccess(app.getMetrics().NewMeter(
          {"history", "download-history-archive-state" + std::move(mode),
           "success"},
          "event"))
{
}

static bool
isWellKnown(uint32_t seq)
{
    return seq == 0;
}

BasicWork::State
GetHistoryArchiveStateWork::doWork()
{
    ZoneScoped;
    if (mGetRemoteFile)
    {
        auto state = mGetRemoteFile->getState();
        auto archive = mGetRemoteFile->getCurrentArchive();
        if (state == State::WORK_SUCCESS)
        {
            try
            {
                mState.load(mLocalFilename);
            }
            catch (std::runtime_error& e)
            {
                CLOG_ERROR(History, "Error loading history state: {}",
                           e.what());
                CLOG_ERROR(History, "{}", POSSIBLY_CORRUPTED_LOCAL_FS);
                CLOG_ERROR(History, "OR");
                CLOG_ERROR(History, "{}", POSSIBLY_CORRUPTED_HISTORY);
                CLOG_ERROR(History, "OR");
                CLOG_ERROR(History, "{}", UPGRADE_STELLAR_CORE);
                return State::WORK_FAILURE;
            }
        }
        else if (state == State::WORK_FAILURE && archive)
        {
            if (isWellKnown(mSeq))
            {
                // Archive is corrupt if it's missing a well-known file
                CLOG_ERROR(History,
                           "Could not download {} file: corrupt archive {}",
                           HistoryArchiveState::wellKnownRemoteName(),
                           archive->getName());
            }
            else
            {
                CLOG_ERROR(History,
                           "Missing HAS for ledger {}: maybe stale archive {}",
                           std::to_string(mSeq), archive->getName());
            }
        }
        return state;
    }

    else
    {
        auto name = getRemoteName();
        CLOG_INFO(History, "Downloading history archive state: {}", name);
        mGetRemoteFile = addWork<GetRemoteFileWork>(name, mLocalFilename,
                                                    mArchive, mRetries);
        return State::WORK_RUNNING;
    }
}

void
GetHistoryArchiveStateWork::doReset()
{
    mGetRemoteFile.reset();
    std::remove(mLocalFilename.c_str());
    mState = {};
}

void
GetHistoryArchiveStateWork::onSuccess()
{
    mGetHistoryArchiveStateSuccess.Mark();
    Work::onSuccess();
}

std::string
GetHistoryArchiveStateWork::getRemoteName() const
{
    return isWellKnown(mSeq) ? HistoryArchiveState::wellKnownRemoteName()
                             : HistoryArchiveState::remoteName(mSeq);
}

std::string
GetHistoryArchiveStateWork::getStatus() const
{
    std::string ledgerString = mSeq == 0 ? "current" : std::to_string(mSeq);
    return fmt::format("Downloading state file {} for ledger {}",
                       getRemoteName(), ledgerString);
}
}
