// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#pragma once

#include "history/HistoryArchive.h"
#include "work/Work.h"

namespace stellar
{

class GetHistoryArchiveStateWork;
class Bucket;
class TmpDir;

namespace txsimulation
{

class TxSimGenerateBucketsWork;
/*
 * TxSimScaleBucketlistWork will retrieve the current bucketlist, transform it
 * by creating new Ledger Entries to mimic existing ones, and apply newly
 * created buckets
 * */
class TxSimScaleBucketlistWork : public Work
{
    Application& mApp;
    std::shared_ptr<GetHistoryArchiveStateWork> mGetState;
    std::shared_ptr<TxSimGenerateBucketsWork> mGenerateBucketsWork;
    std::shared_ptr<BasicWork> mDownloadGenerateBuckets;
    std::shared_ptr<BasicWork> mApplyBuckets;

    std::map<std::string, std::shared_ptr<Bucket>> mCurrentBuckets;
    // Keep references to created buckets to ensure GC doesn't delete them
    // downstream
    std::map<std::string, std::shared_ptr<Bucket>> mGeneratedBuckets;
    // If present, bypass bucket re-generation, and proceed directly to
    // simulated bucket application
    std::shared_ptr<HistoryArchiveState> mHAS;

    TmpDir const& mTmpDir;
    uint32_t const mMultiplier;
    uint32_t const mLedger;

  public:
    TxSimScaleBucketlistWork(
        Application& app, uint32_t multiplier, uint32_t ledger,
        TmpDir const& mTmpDir,
        std::shared_ptr<HistoryArchiveState> has = nullptr);
    virtual ~TxSimScaleBucketlistWork() = default;

  protected:
    BasicWork::State doWork() override;
    void doReset() override;
};
}
}