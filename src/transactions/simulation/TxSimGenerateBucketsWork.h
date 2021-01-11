// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "history/HistoryArchive.h"
#include "work/BasicWork.h"
#include <list>

namespace stellar
{

class Bucket;
class TmpDir;

namespace txsimulation
{

/*
 * TxSimGenerateBucketsWork transforms the existing bucketlist by creating new
 * ledger entries that mimic the existing ones. This is useful for simulation
 * and test purposes, as production buckets can be used to create a more or less
 * plausible set of LedgerEntries and simulate a much bigger ledger. This work
 * assumes presence of the bucket files in BUCKET_DIR_PATH. Each bucket is
 * transformed into a new, bigger bucket that mimics lifecycles of given ledger
 * entries, but with artificially generated account keys.
 * */

class TxSimGenerateBucketsWork : public BasicWork
{
    Application& mApp;
    std::map<std::string, std::shared_ptr<Bucket>>& mBuckets;
    HistoryArchiveState const mApplyState;

    // New HAS is populated incrementally
    HistoryArchiveState mGeneratedApplyState;
    uint32_t const mMultiplier;
    uint32_t mLevel;

    std::shared_ptr<Bucket> mPrevSnap;

    std::list<std::shared_ptr<Bucket>> mIntermediateBuckets;
    std::vector<FutureBucket> mMergesInProgress;
    bool mIsCurr;

    void setFutureBucket(std::shared_ptr<Bucket> const& curr);
    void startBucketGeneration(std::shared_ptr<Bucket> const& oldBucket);
    bool checkOrStartMerges();
    void processGeneratedBucket();

  public:
    TxSimGenerateBucketsWork(
        Application& app,
        std::map<std::string, std::shared_ptr<Bucket>>& buckets,
        HistoryArchiveState const& applyState, uint32_t multiplier);
    virtual ~TxSimGenerateBucketsWork() = default;

    HistoryArchiveState const& getGeneratedHAS();

  protected:
    void onReset() override;
    BasicWork::State onRun() override;

    bool
    onAbort() override
    {
        return true;
    }
};
}
}