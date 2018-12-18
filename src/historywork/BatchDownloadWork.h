// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "history/FileTransferInfo.h"
#include "historywork/BatchWork.h"
#include "ledger/CheckpointRange.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "work/Work.h"

namespace medida
{
class Meter;
}

namespace stellar
{

class BatchDownloadWork : public BatchWork
{
    // Specialized class for downloading _lots_ of files (thousands to
    // millions). Sets up N (small number) of parallel download-decompress
    // worker chains to nibble away at a set of files-to-download, stored
    // as an integer deque. N is the subprocess-concurrency limit by default
    // (though it's still enforced globally at the ProcessManager level,
    // so you don't have to worry about making a few extra BatchDownloadWork
    // classes -- they won't override the global limit, just schedule a small
    // backlog in the ProcessManager).
    CheckpointRange mRange;
    uint32_t mNext;
    std::string mFileType;
    TmpDir const& mDownloadDir;

    medida::Meter& mDownloadSuccess;
    medida::Meter& mDownloadFailure;

  public:
    BatchDownloadWork(Application& app, WorkParent& parent,
                      CheckpointRange range, std::string const& type,
                      TmpDir const& downloadDir);
    ~BatchDownloadWork() override;
    std::string getStatus() const override;
    void notify(std::string const& child) override;

  protected:
    bool hasNext() override;
    std::string yieldMoreWork() override;
    void resetIter() override;
};
}
