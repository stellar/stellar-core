// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "ledger/CheckpointRange.h"
#include "work/Work.h"

namespace stellar
{

class ApplyLedgerChainWork;
class LedgerHeaderHistoryEntry;
class TmpDir;

class DownloadAndApplyTransactionsWork : public Work
{
  public:
    DownloadAndApplyTransactionsWork(Application& app, WorkParent& parent,
                                     CheckpointRange range,
                                     TmpDir const& downloadDir);
    std::string getStatus() const override;
    void onReset() override;
    Work::State onSuccess() override;

    LedgerHeaderHistoryEntry getLastApplied() const;

  protected:
    TmpDir const& mDownloadDir;
    std::shared_ptr<Work> mDownloadTransactionsWork;
    std::shared_ptr<ApplyLedgerChainWork> mApplyWork;
    CheckpointRange mRange;
};
}
