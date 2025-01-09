// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/CatchupWork.h"
#include "bucket/BucketManager.h"
#include "catchup/ApplyBucketsWork.h"
#include "catchup/ApplyBufferedLedgersWork.h"
#include "catchup/ApplyCheckpointWork.h"
#include "catchup/CatchupConfiguration.h"
#include "catchup/CatchupRange.h"
#include "catchup/DownloadApplyTxsWork.h"
#include "catchup/VerifyLedgerChainWork.h"
#include "herder/Herder.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryManager.h"
#include "historywork/BatchDownloadWork.h"
#include "historywork/DownloadBucketsWork.h"
#include "historywork/DownloadVerifyTxResultsWork.h"
#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "main/PersistentState.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "work/WorkWithCallback.h"
#include <Tracy.hpp>
#include <fmt/format.h>

namespace stellar
{

uint32_t const CatchupWork::PUBLISH_QUEUE_UNBLOCK_APPLICATION = 8;
uint32_t const CatchupWork::PUBLISH_QUEUE_MAX_SIZE = 16;

static std::shared_ptr<LedgerHeaderHistoryEntry>
getHistoryEntryForLedger(uint32_t ledgerSeq, FileTransferInfo const& ft)
{
    XDRInputFileStream in;
    in.open(ft.localPath_nogz());

    auto lhhe = std::make_shared<LedgerHeaderHistoryEntry>();

    while (in && in.readOne<LedgerHeaderHistoryEntry>(*lhhe))
    {
        if (lhhe->header.ledgerSeq == ledgerSeq)
        {
            return lhhe;
        }
    }

    return nullptr;
}

static bool
setHerderStateTo(FileTransferInfo const& ft, uint32_t ledger, Application& app)
{
    auto entry = getHistoryEntryForLedger(ledger, ft);
    if (!entry)
    {
        CLOG_ERROR(History,
                   "Malformed catchup file does not contain "
                   "LedgerHeaderHistoryEntry for ledger {}",
                   ledger);
        return false;
    }

    app.getHerder().setTrackingSCPState(ledger, entry->header.scpValue,
                                        /* isTrackingNetwork */ false);
    CLOG_INFO(History, "Herder state is set! tracking={}, closeTime={}", ledger,
              entry->header.scpValue.closeTime);
    return true;
}

CatchupWork::CatchupWork(Application& app,
                         CatchupConfiguration catchupConfiguration,
                         std::set<std::shared_ptr<LiveBucket>> bucketsToRetain,
                         std::shared_ptr<HistoryArchive> archive)
    : Work(app, "catchup", BasicWork::RETRY_NEVER)
    , mLocalState{app.getLedgerManager().getLastClosedLedgerHAS()}
    , mDownloadDir{std::make_unique<TmpDir>(
          mApp.getTmpDirManager().tmpDir(getName()))}
    , mCatchupConfiguration{catchupConfiguration}
    , mArchive{archive}
    , mRetainedBuckets{bucketsToRetain}
{
    if (mArchive)
    {
        CLOG_INFO(History, "CatchupWork: selected archive {}",
                  mArchive->getName());
    }
}

CatchupWork::~CatchupWork()
{
}

std::string
CatchupWork::getStatus() const
{
    std::string toLedger;
    if (mCatchupConfiguration.toLedger() == CatchupConfiguration::CURRENT)
    {
        toLedger = mGetHistoryArchiveStateWork &&
                           mGetHistoryArchiveStateWork->getState() ==
                               State::WORK_SUCCESS
                       ? std::to_string(mGetHistoryArchiveStateWork
                                            ->getHistoryArchiveState()
                                            .currentLedger)
                       : "CURRENT";
    }
    else
    {
        toLedger = std::to_string(mCatchupConfiguration.toLedger());
    }

    return fmt::format(FMT_STRING("Catching up to ledger {}: {}"), toLedger,
                       mCurrentWork ? mCurrentWork->getStatus()
                                    : Work::getStatus());
}

void
CatchupWork::doReset()
{
    ZoneScoped;
    mBucketsAppliedEmitted = false;
    mTransactionsVerifyEmitted = false;
    mBuckets.clear();
    mDownloadVerifyLedgersSeq.reset();
    mBucketVerifyApplySeq.reset();
    mTransactionsVerifyApplySeq.reset();
    mGetHistoryArchiveStateWork.reset();
    mApplyBufferedLedgersWork.reset();
    auto const& lcl = mApp.getLedgerManager().getLastClosedLedgerHeader();
    mLastClosedLedgerHashPair = LedgerNumHashPair(
        lcl.header.ledgerSeq, std::make_optional<Hash>(lcl.hash));
    mCatchupSeq.reset();
    mGetBucketStateWork.reset();
    mVerifyTxResults.reset();
    mVerifyLedgers.reset();
    mLastApplied = mApp.getLedgerManager().getLastClosedLedgerHeader();
    mCurrentWork.reset();
    mHAS.reset();
    mBucketHAS.reset();
    mRetainedBuckets.clear();
}

void
CatchupWork::downloadVerifyLedgerChain(CatchupRange const& catchupRange,
                                       LedgerNumHashPair rangeEnd)
{
    releaseAssert(!mCatchupConfiguration.localBucketsOnly());

    ZoneScoped;
    auto verifyRange = catchupRange.getFullRangeIncludingBucketApply();
    releaseAssert(verifyRange.mCount != 0);
    auto checkpointRange =
        CheckpointRange{verifyRange, mApp.getHistoryManager()};
    // Batch download has default retries ("a few") to ensure we rotate through
    // archives
    auto getLedgers = std::make_shared<BatchDownloadWork>(
        mApp, checkpointRange, FileType::HISTORY_FILE_TYPE_LEDGER,
        *mDownloadDir, mArchive);
    mRangeEndPromise = std::promise<LedgerNumHashPair>();
    mRangeEndFuture = mRangeEndPromise.get_future().share();
    mRangeEndPromise.set_value(rangeEnd);

    auto fatalFailurePromise = std::promise<bool>();
    mFatalFailureFuture = fatalFailurePromise.get_future().share();

    mVerifyLedgers = std::make_shared<VerifyLedgerChainWork>(
        mApp, *mDownloadDir, verifyRange, mLastClosedLedgerHashPair,
        std::nullopt, mRangeEndFuture, std::move(fatalFailurePromise));

    // Never retry the sequence: downloads already have retries, and there's no
    // point retrying verification
    std::vector<std::shared_ptr<BasicWork>> seq{getLedgers, mVerifyLedgers};
    mDownloadVerifyLedgersSeq = addWork<WorkSequence>(
        "download-verify-ledgers-seq", seq, BasicWork::RETRY_NEVER);
    mCurrentWork = mDownloadVerifyLedgersSeq;
}

void
CatchupWork::downloadVerifyTxResults(CatchupRange const& catchupRange)
{
    ZoneScoped;
    auto range = catchupRange.getReplayRange();
    auto checkpointRange = CheckpointRange{range, mApp.getHistoryManager()};
    mVerifyTxResults = std::make_shared<DownloadVerifyTxResultsWork>(
        mApp, checkpointRange, *mDownloadDir);
}

bool
CatchupWork::alreadyHaveBucketsHistoryArchiveState(uint32_t atCheckpoint) const
{
    return mCatchupConfiguration.localBucketsOnly() ||
           atCheckpoint == mGetHistoryArchiveStateWork->getHistoryArchiveState()
                               .currentLedger;
}

WorkSeqPtr
CatchupWork::downloadApplyBuckets()
{
    ZoneScoped;

    // If stellar-core aborts while applying buckets, it can leave state in
    // the database. This guarantees that we clear that state the next time
    // the application starts.
    auto& ps = mApp.getPersistentState();
    ps.setRebuildForOfferTable();

    std::vector<std::shared_ptr<BasicWork>> seq;
    auto version = mApp.getConfig().LEDGER_PROTOCOL_VERSION;

    // Download buckets, or skip if catchup is local
    if (!mCatchupConfiguration.localBucketsOnly())
    {
        std::vector<std::string> hashes =
            mBucketHAS->differingBuckets(mLocalState);
        auto getBuckets = std::make_shared<DownloadBucketsWork>(
            mApp, mBuckets, hashes, *mDownloadDir, mArchive);
        seq.push_back(getBuckets);

        auto verifyHASCallback = [has = *mBucketHAS](Application& app) {
            if (!has.containsValidBuckets(app))
            {
                CLOG_ERROR(History, "Malformed HAS: invalid buckets");
                return false;
            }
            return true;
        };
        auto verifyHAS = std::make_shared<WorkWithCallback>(mApp, "verify-has",
                                                            verifyHASCallback);
        seq.push_back(verifyHAS);
        version = mVerifiedLedgerRangeStart.header.ledgerVersion;
    }

    auto applyBuckets = std::make_shared<ApplyBucketsWork>(
        mApp, mBuckets, *mBucketHAS, version);
    seq.push_back(applyBuckets);
    return std::make_shared<WorkSequence>(mApp, "download-verify-apply-buckets",
                                          seq, RETRY_NEVER);
}

void
CatchupWork::assertBucketState()
{
    releaseAssert(mBucketHAS);
    releaseAssert(mBucketHAS->currentLedger >
                  LedgerManager::GENESIS_LEDGER_SEQ);

    // Consistency check: remote state and mVerifiedLedgerRangeStart should
    // point to the same ledger and the same BucketList.
    if (mBucketHAS->currentLedger != mVerifiedLedgerRangeStart.header.ledgerSeq)
    {
        CLOG_ERROR(History, "Caught up to wrong ledger: wanted {}, got {}",
                   mBucketHAS->currentLedger,
                   mVerifiedLedgerRangeStart.header.ledgerSeq);
    }
    releaseAssert(mBucketHAS->currentLedger ==
                  mVerifiedLedgerRangeStart.header.ledgerSeq);
    releaseAssert(mBucketHAS->getBucketListHash() ==
                  mVerifiedLedgerRangeStart.header.bucketListHash);

    // Consistency check: LCL should be in the _past_ from
    // firstVerified, since we're about to clobber a bunch of DB
    // state with new buckets held in firstVerified's state.
    auto lcl = mApp.getLedgerManager().getLastClosedLedgerHeader();
    if (mVerifiedLedgerRangeStart.header.ledgerSeq < lcl.header.ledgerSeq)
    {
        throw std::runtime_error(fmt::format(
            FMT_STRING("Catchup MINIMAL applying ledger earlier than local "
                       "LCL: {:s} < {:s}"),
            LedgerManager::ledgerAbbrev(mVerifiedLedgerRangeStart),
            LedgerManager::ledgerAbbrev(lcl)));
    }
}

void
CatchupWork::downloadApplyTransactions(CatchupRange const& catchupRange)
{
    ZoneScoped;
    auto waitForPublish = mCatchupConfiguration.offline();
    auto range = catchupRange.getReplayRange();
    mTransactionsVerifyApplySeq = std::make_shared<DownloadApplyTxsWork>(
        mApp, *mDownloadDir, range, mLastApplied, waitForPublish, mArchive);
}

BasicWork::State
CatchupWork::getAndMaybeSetHistoryArchiveState()
{
    // First, retrieve the HAS

    // If we're just doing local catchup, set HAS right away
    if (mCatchupConfiguration.localBucketsOnly())
    {
        mHAS = getCatchupConfiguration().getHAS();
        releaseAssert(mHAS.has_value());
    }
    else
    {
        // Otherwise, continue with the normal catchup flow: download and verify
        // HAS from history archive
        if (!mGetHistoryArchiveStateWork)
        {
            auto toLedger =
                mCatchupConfiguration.toLedger() == 0
                    ? "CURRENT"
                    : std::to_string(mCatchupConfiguration.toLedger());
            CLOG_INFO(
                History,
                "Starting catchup with configuration:\n  lastClosedLedger: "
                "{}\n  toLedger: {}\n  count: {}",
                mApp.getLedgerManager().getLastClosedLedgerNum(), toLedger,
                mCatchupConfiguration.count());

            auto toCheckpoint =
                mCatchupConfiguration.toLedger() ==
                        CatchupConfiguration::CURRENT
                    ? CatchupConfiguration::CURRENT
                    : HistoryManager::checkpointContainingLedger(
                          mCatchupConfiguration.toLedger(), mApp.getConfig());
            // Set retries to 10 to ensure we retry enough in case current
            // checkpoint isn't published yet
            mGetHistoryArchiveStateWork = addWork<GetHistoryArchiveStateWork>(
                toCheckpoint, mArchive, true, 10);
            mCurrentWork = mGetHistoryArchiveStateWork;
            return State::WORK_RUNNING;
        }
        else if (mGetHistoryArchiveStateWork->getState() != State::WORK_SUCCESS)
        {
            return mGetHistoryArchiveStateWork->getState();
        }
        else
        {
            mHAS = std::make_optional<HistoryArchiveState>(
                mGetHistoryArchiveStateWork->getHistoryArchiveState());
        }
    }

    // Second, perform some validation

    // If the HAS is a newer version and contains networkPassphrase,
    // we should make sure that it matches the config's networkPassphrase.
    if (!mHAS->networkPassphrase.empty() &&
        mHAS->networkPassphrase != mApp.getConfig().NETWORK_PASSPHRASE)
    {
        CLOG_ERROR(History, "The network passphrase of the "
                            "application does not match that of the "
                            "history archive state");
        return State::WORK_FAILURE;
    }

    if (mLastClosedLedgerHashPair.first >= mHAS->currentLedger)
    {
        CLOG_INFO(History, "*");
        CLOG_INFO(
            History,
            "* Target ledger {} is not newer than last closed ledger {} - "
            "nothing to do",
            mHAS->currentLedger, mLastClosedLedgerHashPair.first);

        if (mCatchupConfiguration.toLedger() == CatchupConfiguration::CURRENT)
        {
            CLOG_INFO(History, "* Wait until next checkpoint before retrying");
        }
        else
        {
            CLOG_INFO(
                History,
                "* If you really want to catchup to {} run stellar-core new-db",
                mCatchupConfiguration.toLedger());
        }

        CLOG_INFO(History, "*");

        CLOG_ERROR(History, "Nothing to catchup to ");

        return State::WORK_FAILURE;
    }

    return State::WORK_SUCCESS;
}

BasicWork::State
CatchupWork::getAndMaybeSetBucketHistoryArchiveState(uint32_t applyBucketsAt)
{
    if (!alreadyHaveBucketsHistoryArchiveState(applyBucketsAt))
    {
        if (!mGetBucketStateWork)
        {
            mGetBucketStateWork = addWork<GetHistoryArchiveStateWork>(
                applyBucketsAt, mArchive, true);
            mCurrentWork = mGetBucketStateWork;
        }
        if (mGetBucketStateWork->getState() != State::WORK_SUCCESS)
        {
            return mGetBucketStateWork->getState();
        }
        else
        {
            mBucketHAS = mGetBucketStateWork->getHistoryArchiveState();
        }
    }
    else
    {
        mBucketHAS = mHAS;
    }

    return State::WORK_SUCCESS;
}

BasicWork::State
CatchupWork::runCatchupStep()
{
    ZoneScoped;

    // Step 1: Get and validate history archive state
    auto res = getAndMaybeSetHistoryArchiveState();
    if (res != State::WORK_SUCCESS)
    {
        return res;
    }

    // HAS is fetched and validated at this point
    releaseAssert(mHAS->currentLedger > LedgerManager::GENESIS_LEDGER_SEQ);

    auto resolvedConfiguration =
        mCatchupConfiguration.resolve(mHAS->currentLedger);
    auto catchupRange =
        CatchupRange{mLastClosedLedgerHashPair.first, resolvedConfiguration,
                     mApp.getHistoryManager()};

    // Step 3: If needed, download archive state for buckets
    if (catchupRange.applyBuckets())
    {
        res = getAndMaybeSetBucketHistoryArchiveState(
            catchupRange.getBucketApplyLedger());
        if (res != State::WORK_SUCCESS)
        {
            return res;
        }
    }

    // Step 4: Download, verify and apply ledgers, buckets and transactions

    // Bucket and transaction processing has started
    if (mCatchupSeq)
    {
        releaseAssert(mDownloadVerifyLedgersSeq ||
                      mCatchupConfiguration.localBucketsOnly());
        releaseAssert(mTransactionsVerifyApplySeq ||
                      !catchupRange.replayLedgers());

        if (mCatchupSeq->getState() == State::WORK_SUCCESS)
        {
            // Step 4.4: Apply buffered ledgers
            if (mApplyBufferedLedgersWork)
            {
                // ApplyBufferedLedgersWork will try to apply
                // as many ledgers in mSyncingLedgers as possible.
                // Note that it's not always possible to apply
                // _all_ the ledgers in mSyncingLedgers due to gaps.
                if (mApplyBufferedLedgersWork->getState() ==
                    State::WORK_SUCCESS)
                {
                    mApplyBufferedLedgersWork.reset();
                }
                else
                {
                    // waiting for mApplyBufferedLedgersWork to complete/error
                    // out
                    return mApplyBufferedLedgersWork->getState();
                }
            }
            // see if we need to apply buffered ledgers
            if (mApp.getLedgerApplyManager()
                    .maybeGetNextBufferedLedgerToApply())
            {
                mApplyBufferedLedgersWork = addWork<ApplyBufferedLedgersWork>();
                mCurrentWork = mApplyBufferedLedgersWork;
                return State::WORK_RUNNING;
            }

            return State::WORK_SUCCESS;
        }
        else if (mBucketVerifyApplySeq)
        {
            if (mBucketVerifyApplySeq->getState() == State::WORK_SUCCESS &&
                !mBucketsAppliedEmitted)
            {
                // If we crash before this call to setLastClosedLedger, then
                // the node will have to catch up again and it will clear the
                // ledger because clearRebuildForType has not been called yet.
                mApp.getLedgerManager().setLastClosedLedger(
                    mVerifiedLedgerRangeStart,
                    !mCatchupConfiguration.localBucketsOnly());
                mBucketsAppliedEmitted = true;
                mBuckets.clear();
                mLastApplied =
                    mApp.getLedgerManager().getLastClosedLedgerHeader();

                // We've applied buckets successfully, so we don't need to
                // rebuild on startup.
                //
                // If we crash after the call to setLastClosedLedger but before
                // clearRebuildForType, then the new HAS will have already been
                // written in the call to setLastClosedLedger. In this case, we
                // will unnecessarily rebuild the ledger but the buckets are
                // persistently available locally so it will return us to the
                // correct state.
                auto& ps = mApp.getPersistentState();
                ps.clearRebuildForOfferTable();
            }
        }
        else if (mTransactionsVerifyApplySeq)
        {
            if (mTransactionsVerifyApplySeq->getState() ==
                    State::WORK_SUCCESS &&
                !mTransactionsVerifyEmitted)
            {
                mTransactionsVerifyEmitted = true;

                // In this case we should actually have been caught-up during
                // the replay process and, if judged successful, our LCL should
                // be the one provided as well.
                auto& lastClosed =
                    mApp.getLedgerManager().getLastClosedLedgerHeader();
                releaseAssert(mLastApplied.hash == lastClosed.hash);
                releaseAssert(mLastApplied.header == lastClosed.header);
            }
        }
        return mCatchupSeq->getState();
    }

    // If we're just doing local catchup, setup bucket application and exit
    if (mCatchupConfiguration.localBucketsOnly())
    {
        releaseAssert(catchupRange.applyBuckets());
        auto lhhe = mCatchupConfiguration.getHistoryEntry();
        releaseAssert(lhhe);
        mVerifiedLedgerRangeStart = *lhhe;

        mBucketVerifyApplySeq = downloadApplyBuckets();
        std::vector<std::shared_ptr<BasicWork>> seq{mBucketVerifyApplySeq};

        mCatchupSeq = addWork<WorkSequence>("catchup-seq", seq, RETRY_NEVER);
        mCurrentWork = mCatchupSeq;
        return State::WORK_RUNNING;
    }

    // Otherwise, proceed with normal flow. Still waiting for ledger headers
    if (mDownloadVerifyLedgersSeq)
    {
        if (mDownloadVerifyLedgersSeq->getState() == State::WORK_SUCCESS)
        {
            mVerifiedLedgerRangeStart =
                mVerifyLedgers->getMaxVerifiedLedgerOfMinCheckpoint();
            if (catchupRange.applyBuckets() && !mBucketsAppliedEmitted)
            {
                assertBucketState();
            }

            std::vector<std::shared_ptr<BasicWork>> seq;
            // Before replaying ledgers, ensure Herder state is consistent with
            // LCL
            auto cb = [ledgerSeq = catchupRange.last(),
                       &dir = *mDownloadDir](Application& app) {
                if (app.getHerder().trackingConsensusLedgerIndex() >= ledgerSeq)
                {
                    return true;
                }

                auto checkpoint = HistoryManager::checkpointContainingLedger(
                    ledgerSeq, app.getConfig());
                auto ft = FileTransferInfo(
                    dir, FileType::HISTORY_FILE_TYPE_LEDGER, checkpoint);

                return setHerderStateTo(ft, ledgerSeq, app);
            };

            auto consistencyWork = std::make_shared<WorkWithCallback>(
                mApp, "herder-state-consistency-work", cb);
            seq.push_back(consistencyWork);

            if (mCatchupConfiguration.mode() ==
                CatchupConfiguration::Mode::OFFLINE_COMPLETE)
            {
                downloadVerifyTxResults(catchupRange);
                seq.push_back(mVerifyTxResults);
            }

            if (catchupRange.applyBuckets())
            {
                // Step 4.2: Download, verify and apply buckets
                mBucketVerifyApplySeq = downloadApplyBuckets();
                seq.push_back(mBucketVerifyApplySeq);
            }

            if (catchupRange.replayLedgers())
            {
                // Step 4.3: Download and apply ledger chain
                downloadApplyTransactions(catchupRange);
                seq.push_back(mTransactionsVerifyApplySeq);
            }

            mCatchupSeq =
                addWork<WorkSequence>("catchup-seq", seq, RETRY_NEVER);
            mCurrentWork = mCatchupSeq;
            return State::WORK_RUNNING;
        }
        else if (mDownloadVerifyLedgersSeq->getState() == State::WORK_FAILURE)
        {
            if (mVerifyLedgers && mVerifyLedgers->isDone())
            {
                releaseAssert(futureIsReady(mFatalFailureFuture));
            }
        }
        return mDownloadVerifyLedgersSeq->getState();
    }

    // Step 4.1: Download and verify ledger chain
    downloadVerifyLedgerChain(
        catchupRange,
        LedgerNumHashPair(catchupRange.last(), mCatchupConfiguration.hash()));

    return State::WORK_RUNNING;
}

BasicWork::State
CatchupWork::doWork()
{
    ZoneScoped;
    auto nextState = runCatchupStep();
    auto& lam = mApp.getLedgerApplyManager();

    if (nextState == BasicWork::State::WORK_SUCCESS)
    {
        releaseAssert(!lam.maybeGetNextBufferedLedgerToApply());
    }

    lam.logAndUpdateCatchupStatus(true);
    return nextState;
}

void
CatchupWork::onFailureRaise()
{
    CLOG_WARNING(History, "Catchup failed");
    Work::onFailureRaise();
    if (mCatchupConfiguration.localBucketsOnly())
    {
        throw std::runtime_error("Unable to rebuild local state");
    }
}

void
CatchupWork::onSuccess()
{
    CLOG_INFO(History, "Catchup finished");
    Work::onSuccess();
}
}
