// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "ledger/LedgerRange.h"
#include "work/BatchWork.h"
#include <future>
#include <iosfwd>

namespace stellar
{

class VerifyLedgerChainWork;
class HistoryArchive;
class WorkSequence;
class TmpDir;

class WriteVerifiedCheckpointHashesWork : public BatchWork
{
  protected:
    bool hasNext() const override;
    std::shared_ptr<BasicWork> yieldMoreWork() override;
    void resetIter() override;

  public:
    WriteVerifiedCheckpointHashesWork(
        Application& app, LedgerNumHashPair rangeEnd,
        std::string const& outputFile,
        uint32_t nestedBatchSize = NESTED_DOWNLOAD_BATCH_SIZE,
        std::shared_ptr<HistoryArchive> archive = nullptr);
    ~WriteVerifiedCheckpointHashesWork();

    // Helper to load a hash back from a file produced by this class.
    static Hash loadHashFromJsonOutput(uint32_t seq,
                                       std::string const& filename);

  private:
    // This class is a batch work, but it also creates a conditional dependency
    // chain among its batch elements (for trusted ledger propagation): this
    // dependency chain can in turn cause the BatchWork logic to stall, failing
    // to saturate the parallel subprocess-execution system. So to keep the
    // latter busy we introduce an inner level of fully-parallelizable batching
    // of downloads. Empirically this seems to work well at a fixed size.
    static constexpr uint32_t NESTED_DOWNLOAD_BATCH_SIZE = 64;

    // For testing purposes we'd like to be able to change this, however.
    uint32_t const mNestedBatchSize;

    // We make a TmpDir for each inner WorkSequence we run, but delete them on
    // the end of each to free up disk space. Since the inner WorkSequences end
    // in unpredictable order we just keep them in a vector and scan it each
    // time we wake up. It's no bigger than Config::MAX_CONCURRENT_SUBPROCESSES.
    std::vector<
        std::pair<std::shared_ptr<WorkSequence>, std::shared_ptr<TmpDir>>>
        mTmpDirs;

    // Total range to verify is implicitly 1 .. mRangeEnd.first
    LedgerNumHashPair const mRangeEnd;

    // We form a promise and an associated shared_future that hold a copy
    // of mRangeEnd so that we can provide it to the work that we yield
    // for the first entry in the verification chain.
    std::promise<LedgerNumHashPair> mRangeEndPromise;
    std::shared_future<LedgerNumHashPair> mRangeEndFuture;

    // mCurrCheckpoint == LedgerManager::GENESIS_LEDGER_SEQ if we're done, or
    // else some checkpoint-boundary ledger >= 63
    uint32_t mCurrCheckpoint;

    std::shared_ptr<VerifyLedgerChainWork> mPrevVerifyWork;

    std::shared_ptr<HistoryArchive> mArchive;

    void startOutputFile();
    void endOutputFile();
    std::shared_ptr<std::ofstream> mOutputFile;
    std::string mOutputFileName;
};
}
