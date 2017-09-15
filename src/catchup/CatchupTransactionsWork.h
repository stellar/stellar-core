// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "ledger/CheckpointRange.h"
#include "work/Work.h"
#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{

class ApplyLedgerChainWork;
class DownloadAndVerifyLedgersWork;
class TmpDir;
class VerifyLedgerChainWork;
struct LedgerHeaderHistoryEntry;

class CatchupTransactionsWork : public Work
{
  public:
    CatchupTransactionsWork(Application& app, WorkParent& parent,
                            TmpDir& downloadDir, CheckpointRange range,
                            bool manualCatchup, std::string catchupTypeName,
                            std::string const& name, size_t maxRetries = 0);
    std::string getStatus() const override;
    void onReset() override;
    Work::State onSuccess() override;

    LedgerHeaderHistoryEntry getFirstVerified() const;
    LedgerHeaderHistoryEntry getLastVerified() const;
    LedgerHeaderHistoryEntry getLastApplied() const;

  private:
    std::shared_ptr<DownloadAndVerifyLedgersWork> mDownloadAndVerifyLedgersWork;
    std::shared_ptr<Work> mDownloadTransactionsWork;
    std::shared_ptr<ApplyLedgerChainWork> mApplyWork;

    TmpDir& mDownloadDir;
    CheckpointRange mRange;
    bool mManualCatchup;
    std::string mCatchupTypeName;
};
}
