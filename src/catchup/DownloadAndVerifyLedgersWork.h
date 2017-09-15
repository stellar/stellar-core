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

class LedgerHeaderHistoryEntry;
class TmpDir;
class VerifyLedgerChainWork;

class DownloadAndVerifyLedgersWork : public Work
{
  public:
    DownloadAndVerifyLedgersWork(Application& app, WorkParent& parent,
                                 CheckpointRange range, bool manualCatchup,
                                 TmpDir const& downloadDir);
    std::string getStatus() const override;
    void onReset() override;
    Work::State onSuccess() override;

    LedgerHeaderHistoryEntry getFirstVerified() const;
    LedgerHeaderHistoryEntry getLastVerified() const;

  protected:
    TmpDir const& mDownloadDir;
    std::shared_ptr<Work> mDownloadLedgersWork;
    std::shared_ptr<VerifyLedgerChainWork> mVerifyLedgersWork;
    CheckpointRange mRange;
    bool mManualCatchup;
};
}
