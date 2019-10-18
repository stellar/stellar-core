// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "history/FileTransferInfo.h"
#include "ledger/CheckpointRange.h"
#include "work/BatchWork.h"

namespace medida
{
class Meter;
}

namespace stellar
{

class HistoryArchive;

class BatchDownloadWork : public BatchWork
{
    CheckpointRange const mRange;
    uint32_t mNext;
    std::string const mFileType;
    TmpDir const& mDownloadDir;
    std::shared_ptr<HistoryArchive> mArchive;

  public:
    BatchDownloadWork(Application& app, CheckpointRange range,
                      std::string const& type, TmpDir const& downloadDir,
                      std::shared_ptr<HistoryArchive> archive = nullptr);
    ~BatchDownloadWork() = default;
    std::string getStatus() const override;

  protected:
    bool hasNext() const override;
    std::shared_ptr<BasicWork> yieldMoreWork() override;
    void resetIter() override;
};
}
