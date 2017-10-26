// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "ledger/CheckpointRange.h"
#include "work/Work.h"

namespace medida
{
class Meter;
}

namespace stellar
{

class TmpDir;

class BatchDownloadWork : public Work
{
    // Specialized class for downloading _lots_ of files (thousands to
    // millions). Sets up N (small number) of parallel download-decompress
    // worker chains to nibble away at a set of files-to-download, stored
    // as an integer deque. N is the subprocess-concurrency limit by default
    // (though it's still enforced globally at the ProcessManager level,
    // so you don't have to worry about making a few extra BatchDownloadWork
    // classes -- they won't override the global limit, just schedule a small
    // backlog in the ProcessManager).
    std::deque<uint32_t> mFinished;
    std::map<std::string, uint32_t> mRunning;
    CheckpointRange mRange;
    uint32_t mNext;
    std::string mFileType;
    TmpDir const& mDownloadDir;

    medida::Meter& mDownloadCached;
    medida::Meter& mDownloadStart;
    medida::Meter& mDownloadSuccess;
    medida::Meter& mDownloadFailure;

    void addNextDownloadWorker();

  public:
    BatchDownloadWork(Application& app, WorkParent& parent,
                      CheckpointRange range, std::string const& type,
                      TmpDir const& downloadDir);
    ~BatchDownloadWork();
    std::string getStatus() const override;
    void onReset() override;
    void notify(std::string const& child) override;
};
}
