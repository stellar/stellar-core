// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/ReplayDebugMetaWork.h"
#include "catchup/CatchupWork.h"
#include "work/WorkScheduler.h"
#include "work/WorkWithCallback.h"

#include "catchup/ApplyLedgerWork.h"
#include "herder/LedgerCloseData.h"
#include "historywork/GunzipFileWork.h"
#include "ledger/LedgerManager.h"
#include "util/DebugMetaUtils.h"
#include "util/GlobalChecks.h"
#include "util/XDRStream.h"
#include <regex>

namespace stellar
{

// Helper class to apply ledgers from a single debug meta file
class ApplyLedgersFromMetaWork : public Work
{
    XDRInputFileStream mMetaIn;
    std::filesystem::path const mFilename;
    bool mFileOpen{false};
    std::shared_ptr<ApplyLedgerWork> mApplyLedgerWork;
    uint32_t const mTargetLedger;

  public:
    ApplyLedgersFromMetaWork(Application& app,
                             std::filesystem::path const& unzippedMetaFile,
                             uint32_t targetLedger)
        : Work(app, fmt::format("apply-ledgers-from-{}", unzippedMetaFile),
               BasicWork::RETRY_NEVER)
        , mFilename(unzippedMetaFile)
        , mTargetLedger(targetLedger)
    {
    }

    virtual ~ApplyLedgersFromMetaWork() = default;

  protected:
    void
    doReset() override
    {
        mMetaIn.close();
        mFileOpen = false;
        mApplyLedgerWork.reset();
    }

    State
    doWork() override
    {
        if (!mFileOpen)
        {
            mMetaIn.open(mFilename.string());
            mFileOpen = true;
        }

        if (mApplyLedgerWork)
        {
            if (mApplyLedgerWork->getState() == BasicWork::State::WORK_SUCCESS)
            {
                mApplyLedgerWork.reset();
            }
            else
            {
                return mApplyLedgerWork->getState();
            }
        }

        if (mTargetLedger != 0 &&
            mApp.getLedgerManager().getLastClosedLedgerNum() >= mTargetLedger)
        {
            CLOG_INFO(Work, "LCL is at or past the target ledger {}",
                      mTargetLedger);
            return BasicWork::State::WORK_SUCCESS;
        }

        // Invariant: ledger close meta can't have gaps, so here reading should
        // always yield the next ledger
        LedgerCloseMeta lcm;
        if (!mMetaIn.readOne(lcm))
        {
            // Reached the end of the stream, success
            return BasicWork::State::WORK_SUCCESS;
        }

        auto lh = lcm.v() == 0 ? lcm.v0().ledgerHeader : lcm.v1().ledgerHeader;

        auto ledgerSeqToApply = lh.header.ledgerSeq;
        auto lcl = mApp.getLedgerManager().getLastClosedLedgerNum();
        if (ledgerSeqToApply <= lcl)
        {
            // Old ledger, skipping
            CLOG_INFO(Work, "LCL {} is more recent than ledger {}", lcl,
                      ledgerSeqToApply);
            return BasicWork::State::WORK_RUNNING;
        }
        else if (ledgerSeqToApply > lcl + 1)
        {
            CLOG_FATAL(Work,
                       "Ledger {} is too far (lcl={}), can't apply. Please run "
                       "catchup {}/0` first.",
                       ledgerSeqToApply, lcl, ledgerSeqToApply - 1);
            return BasicWork::State::WORK_FAILURE;
        }

        TxSetXDRFrameConstPtr txSet;
        if (lcm.v() == 0)
        {
            txSet = TxSetXDRFrame::makeFromWire(lcm.v0().txSet);
        }
        else
        {
            txSet = TxSetXDRFrame::makeFromWire(lcm.v1().txSet);
        }

        LedgerCloseData ledgerCloseData(ledgerSeqToApply, txSet,
                                        lh.header.scpValue);

        releaseAssert(!mApplyLedgerWork);
        mApplyLedgerWork = addWork<ApplyLedgerWork>(ledgerCloseData);
        return BasicWork::State::WORK_RUNNING;
    }

    void
    onSuccess() override
    {
        // Close the stream just in case Work is kept alive for some time before
        // it's garbage-collected
        mMetaIn.close();
    }
};

ReplayDebugMetaWork::ReplayDebugMetaWork(Application& app,
                                         uint32_t targetLedger,
                                         std::filesystem::path metaDir)
    : Work(app, "replay-debug-meta", BasicWork::RETRY_NEVER)
    , mTargetLedger(targetLedger)
    , mFiles(metautils::listMetaDebugFiles(metaDir))
    , mMetaDir(metaDir)
{
    mNextToApply = mFiles.cbegin();
}

BasicWork::State
ReplayDebugMetaWork::applyLastLedger()
{
    StoredDebugTransactionSet debugTxSet;
    {
        XDRInputFileStream in;
        in.open(metautils::getLatestTxSetFilePath(mMetaDir).string());
        if (!in.readOne(debugTxSet))
        {
            CLOG_ERROR(Work, "Debug tx set missing");
            return BasicWork::State::WORK_FAILURE;
        }
    }

    auto lcl = mApp.getLedgerManager().getLastClosedLedgerNum();
    if (lcl + 1 == debugTxSet.ledgerSeq)
    {
        mApp.getLedgerManager().closeLedger(
            LedgerCloseData::toLedgerCloseData(debugTxSet),
            /* externalize */ false);
    }
    else
    {
        CLOG_WARNING(Work,
                     "LCL={} does not match expected next ledger to apply {}, "
                     "nothing to do",
                     lcl, debugTxSet.ledgerSeq);
    }

    return BasicWork::State::WORK_SUCCESS;
}

BasicWork::State
ReplayDebugMetaWork::doWork()
{
    if (mCurrentWorkSequence)
    {
        if (mCurrentWorkSequence->getState() == BasicWork::State::WORK_SUCCESS)
        {
            mCurrentWorkSequence.reset();
            ++mNextToApply;
        }
        else
        {
            return mCurrentWorkSequence->getState();
        }
    }

    releaseAssert(!mCurrentWorkSequence);
    if (mNextToApply == mFiles.end())
    {
        if (mTargetLedger == 0 ||
            mApp.getLedgerManager().getLastClosedLedgerNum() < mTargetLedger)
        {
            // Finished applying meta from all debug files, see if we can apply
            // the latest saved tx set on top
            return applyLastLedger();
        }
        else
        {
            return BasicWork::State::WORK_SUCCESS;
        }
    }

    auto filename = *mNextToApply;
    CLOG_INFO(Work, "Process next debug meta file: {}", filename.string());
    auto isZipped = std::regex_match(filename.string(),
                                     metautils::META_DEBUG_ZIP_FILE_REGEX);

    auto zipped = metautils::getMetaDebugDirPath(mMetaDir) / filename;

    std::filesystem::path unzipped = zipped;
    std::vector<std::shared_ptr<BasicWork>> seq;

    if (isZipped)
    {
        seq.emplace_back(std::make_shared<GunzipFileWork>(
            mApp, zipped.string(), /* keepExisting */ true));
        // If zipped, remove `gz` extension
        unzipped.replace_extension();
    }

    seq.emplace_back(std::make_shared<ApplyLedgersFromMetaWork>(mApp, unzipped,
                                                                mTargetLedger));
    auto removeUnzipped = [isZipped, unzipped](Application& app) {
        if (isZipped)
        {
            std::filesystem::remove(unzipped);
        }
        return true;
    };
    seq.emplace_back(std::make_shared<WorkWithCallback>(mApp, "remove-unzipped",
                                                        removeUnzipped));

    mCurrentWorkSequence = addWork<WorkSequence>("unzip-and-apply-from-meta",
                                                 seq, BasicWork::RETRY_NEVER);
    return BasicWork::State::WORK_RUNNING;
}

void
ReplayDebugMetaWork::doReset()
{
    mNextToApply = mFiles.cbegin();
    mCurrentWorkSequence.reset();
}

}
