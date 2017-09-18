// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "catchup/CatchupManager.h"
#include "catchup/CatchupWork.h"

namespace stellar
{

class CatchupTransactionsWork;

class CatchupCompleteWork : public CatchupWork
{
    std::shared_ptr<CatchupTransactionsWork> mCatchupTransactionsWork;
    ProgressHandler mProgressHandler;
    virtual uint32_t firstCheckpointSeq() const override;

  public:
    CatchupCompleteWork(Application& app, WorkParent& parent,
                        uint32_t initLedger, bool manualCatchup,
                        ProgressHandler progressHandler);
    std::string getStatus() const override;
    void onReset() override;
    Work::State onSuccess() override;
    void onFailureRaise() override;
};
}
