// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TxSimScaleBucketlistWork.h"
#include "catchup/ApplyBucketsWork.h"
#include "historywork/DownloadBucketsWork.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "src/transactions/simulation/TxSimGenerateBucketsWork.h"

namespace stellar
{
namespace txsimulation
{

TxSimScaleBucketlistWork::TxSimScaleBucketlistWork(
    Application& app, uint32_t multiplier, uint32_t ledger,
    TmpDir const& tmpDir, std::shared_ptr<HistoryArchiveState> has)
    : Work(app, "simulate-buckets", BasicWork::RETRY_NEVER)
    , mApp(app)
    , mHAS(has)
    , mTmpDir(tmpDir)
    , mMultiplier(multiplier)
    , mLedger(ledger)
{
}

BasicWork::State
TxSimScaleBucketlistWork::doWork()
{
    if (mApplyBuckets)
    {
        return mApplyBuckets->getState();
    }

    auto done = mDownloadGenerateBuckets &&
                mDownloadGenerateBuckets->getState() == State::WORK_SUCCESS;
    if (done || mHAS)
    {
        // Stop referencing old bucketlist, as it's not needed anymore
        mCurrentBuckets.clear();

        // Step 3: Apply artificially created bucketlist
        auto const& has =
            mHAS ? *mHAS : mGenerateBucketsWork->getGeneratedHAS();
        mApplyBuckets = addWork<ApplyBucketsWork>(
            mGeneratedBuckets, has, Config::CURRENT_LEDGER_PROTOCOL_VERSION);
        return State::WORK_RUNNING;
    }
    else if (mDownloadGenerateBuckets)
    {
        return mDownloadGenerateBuckets->getState();
    }

    if (mGetState)
    {
        if (mGetState->getState() == State::WORK_SUCCESS)
        {
            auto has = mGetState->getHistoryArchiveState();
            auto bucketHashes = has.differingBuckets(
                mApp.getLedgerManager().getLastClosedLedgerHAS());

            std::vector<std::shared_ptr<BasicWork>> seq;
            // No need to download anything if HAS is present
            if (!mHAS)
            {
                seq.emplace_back(std::make_shared<DownloadBucketsWork>(
                    mApp, mCurrentBuckets, bucketHashes, mTmpDir));
            }
            mGenerateBucketsWork = std::make_shared<TxSimGenerateBucketsWork>(
                mApp, mGeneratedBuckets, has, mMultiplier);

            seq.emplace_back(mGenerateBucketsWork);

            // Step 2: download current buckets, and generate the new bucketlist
            mDownloadGenerateBuckets =
                addWork<WorkSequence>("download-simulate-buckets", seq);
        }
        else
        {
            return mGetState->getState();
        }
    }

    // Step 1: retrieve current HAS
    mGetState = addWork<GetHistoryArchiveStateWork>(mLedger);
    return State::WORK_RUNNING;
}

void
TxSimScaleBucketlistWork::doReset()
{
    mGetState.reset();
    mGenerateBucketsWork.reset();
    mDownloadGenerateBuckets.reset();
    mApplyBuckets.reset();
    mCurrentBuckets.clear();
    mGeneratedBuckets.clear();
}
}
}
