// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "catchup/CatchupManager.h"
#include "catchup/CatchupWork.h"

namespace stellar
{

class CatchupMinimalWork : public CatchupWork
{
  public:
  protected:
    std::shared_ptr<Work> mDownloadLedgersWork;
    std::shared_ptr<Work> mVerifyLedgersWork;
    std::shared_ptr<Work> mDownloadBucketsWork;
    std::shared_ptr<Work> mApplyWork;
    LedgerHeaderHistoryEntry mFirstVerified;
    LedgerHeaderHistoryEntry mLastVerified;
    LedgerHeaderHistoryEntry mLastApplied;
    ProgressHandler mProgressHandler;
    virtual uint32_t firstCheckpointSeq() const override;

  public:
    CatchupMinimalWork(Application& app, WorkParent& parent,
                       uint32_t initLedger, bool manualCatchup,
                       ProgressHandler progressHandler);
    std::string getStatus() const override;
    void onReset() override;
    Work::State onSuccess() override;
    void onFailureRaise() override;
};
}
