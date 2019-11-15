// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#pragma once

#include "ledger/CheckpointRange.h"
#include "util/TmpDir.h"
#include "work/BatchWork.h"

namespace medida
{
class Meter;
}

namespace stellar
{

class HistoryArchive;

class DownloadVerifyTxResultsWork : public BatchWork
{
    TmpDir const& mDownloadDir;
    CheckpointRange const mRange;
    uint32_t mCurrCheckpoint;
    std::shared_ptr<HistoryArchive> mArchive;

  public:
    DownloadVerifyTxResultsWork(
        Application& app, CheckpointRange range, TmpDir const& downloadDir,
        std::shared_ptr<HistoryArchive> archive = nullptr);
    std::string getStatus() const override;

  protected:
    bool hasNext() const override;
    std::shared_ptr<BasicWork> yieldMoreWork() override;
    void resetIter() override;
};
}
