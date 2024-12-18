// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/CheckSingleLedgerHeaderWork.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryArchive.h"
#include "history/HistoryManager.h"
#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "main/ErrorMessages.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/TmpDir.h"
#include "util/XDRStream.h"
#include "xdrpp/printer.h"

namespace stellar
{

CheckSingleLedgerHeaderWork::CheckSingleLedgerHeaderWork(
    Application& app, std::shared_ptr<HistoryArchive> archive,
    LedgerHeaderHistoryEntry const& expected)
    : Work(app,
           fmt::format(FMT_STRING("check-single-ledger-header-{:d}"),
                       expected.header.ledgerSeq),
           BasicWork::RETRY_NEVER)
    , mArchive(archive)
    , mExpected(expected)
    , mCheckSuccess(
          app.getMetrics().NewMeter({"history", "check", "success"}, "event"))
    , mCheckFailed(
          app.getMetrics().NewMeter({"history", "check", "failure"}, "event"))
{
}

CheckSingleLedgerHeaderWork::~CheckSingleLedgerHeaderWork()
{
}

void
CheckSingleLedgerHeaderWork::onSuccess()
{
    mCheckSuccess.Mark();
    Work::onSuccess();
}

void
CheckSingleLedgerHeaderWork::onFailureRaise()
{
    mCheckFailed.Mark();
    Work::onFailureRaise();
}

void
CheckSingleLedgerHeaderWork::doReset()
{
    mGetLedgerFileWork.reset();
    mDownloadDir =
        std::make_unique<TmpDir>(mApp.getTmpDirManager().tmpDir(getName()));
    uint32_t checkpoint = HistoryManager::checkpointContainingLedger(
        mExpected.header.ledgerSeq, mApp.getConfig());
    mFt = std::make_unique<FileTransferInfo>(
        *mDownloadDir, FileType::HISTORY_FILE_TYPE_LEDGER, checkpoint);
}

BasicWork::State
CheckSingleLedgerHeaderWork::doWork()
{
    if (mExpected.header.ledgerSeq == 0)
    {
        return State::WORK_SUCCESS;
    }

    if (!mGetLedgerFileWork)
    {
        CLOG_INFO(History, "Downloading ledger checkpoint {} from archive {}",
                  mFt->baseName_gz(), mArchive->getName());
        mGetLedgerFileWork = addWork<GetAndUnzipRemoteFileWork>(
            *mFt, mArchive, BasicWork::RETRY_NEVER);
        return State::WORK_RUNNING;
    }
    else if (!mGetLedgerFileWork->isDone())
    {
        return mGetLedgerFileWork->getState();
    }
    else if (mGetLedgerFileWork->getState() != State::WORK_SUCCESS)
    {
        CLOG_ERROR(
            History,
            "Failed to download ledger checkpoint {} from archive {}: {}",
            mFt->baseName_gz(), mArchive->getName(),
            POSSIBLY_CORRUPTED_HISTORY);
        CLOG_ERROR(
            History,
            "If this occurs often, consider notifying the archive "
            "owner. As long as your configuration has any valid history "
            "archives, this error does NOT mean your node is unhealthy.");
        return mGetLedgerFileWork->getState();
    }

    // Theoretically we could do this scan in multiple steps with state
    // transitions, but a ledger header file is 30kb: reading it synchronously
    // is nearly instant.
    XDRInputFileStream in;
    in.open(mFt->localPath_nogz());
    LedgerHeaderHistoryEntry lhhe;
    size_t headersToRead =
        HistoryManager::getCheckpointFrequency(mApp.getConfig());
    try
    {
        while (in && in.readOne<LedgerHeaderHistoryEntry>(lhhe))
        {
            if (headersToRead == 0)
            {
                CLOG_ERROR(History,
                           "Read too many headers from file {} from archive {}",
                           mFt->localPath_nogz(), mArchive->getName());
                return State::WORK_FAILURE;
            }
            --headersToRead;
            if (lhhe.header.ledgerSeq == mExpected.header.ledgerSeq)
            {
                if (lhhe == mExpected)
                {
                    CLOG_INFO(History,
                              "Local ledger header {} matches archive {}",
                              mExpected.header.ledgerSeq, mArchive->getName());
                    mApp.getMetrics()
                        .NewMeter({"history", "ledger-check", "success"},
                                  "event")
                        .Mark();
                    return State::WORK_SUCCESS;
                }
                else
                {
                    CLOG_ERROR(
                        History,
                        "Local ledger header {} does not match archive {}",
                        mExpected.header.ledgerSeq, mArchive->getName());
                    CLOG_ERROR(History, "Expected: {}",
                               xdr::xdr_to_string(mExpected, "Local Header"));
                    CLOG_ERROR(History, "Found: {}",
                               xdr::xdr_to_string(lhhe, "Archive Header"));
                    mApp.getMetrics()
                        .NewMeter({"history", "ledger-check", "failure"},
                                  "event")
                        .Mark();
                    return State::WORK_FAILURE;
                }
            }
        }
    }
    catch (xdr::xdr_runtime_error& e)
    {
        CLOG_ERROR(History, "XDR error decoding {} from archive {}: {}",
                   mFt->localPath_nogz(), mArchive->getName(), e.what());
        return State::WORK_FAILURE;
    }
    catch (FileSystemException& e)
    {
        CLOG_ERROR(History, "Error opening {} from archive {}: {}",
                   mFt->localPath_nogz(), mArchive->getName(), e.what());
        return State::WORK_FAILURE;
    }
    CLOG_ERROR(History, "Failed to find ledger header {} in archive {}",
               mExpected.header.ledgerSeq, mArchive->getName());
    return State::WORK_FAILURE;
}

}
