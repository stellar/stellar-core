// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "work/BatchableWork.h"
#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "historywork/VerifyTxResultsWork.h"
#include "main/Application.h"

namespace stellar
{
class VerifyTxResultsSnapWork : public BatchableWork
{
    std::shared_ptr<VerifyTxResultsWork> mDownloadVerifyWork;
    uint32_t mCheckpoint;
    TmpDir const& mDownloadDir;

    bool downloadVerifyTxResults();

  protected:
    void unblockWork(BatchableWorkResultData const& data) override;

  public:
    VerifyTxResultsSnapWork(Application& app, WorkParent& parent,
                            TmpDir const& downloadDir, uint32_t checkpoint);
    ~VerifyTxResultsSnapWork() override;

    std::string getStatus() const override;
    void onReset() override;
    Work::State onSuccess() override;
};
}
