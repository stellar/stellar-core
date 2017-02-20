// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "work/Work.h"
#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{

class TmpDir;
struct LedgerHeaderHistoryEntry;

class CatchupTransactionsWork : public Work
{
  public:
    CatchupTransactionsWork(Application& app, WorkParent& parent,
                            TmpDir& downloadDir, uint32_t firstSeq,
                            uint32_t lastSeq, bool manualCatchup,
                            std::string catchupTypeName,
                            std::string const& name, size_t maxRetries = 0);
    std::string getStatus() const override;
    void onReset() override;
    Work::State onSuccess() override;

    const LedgerHeaderHistoryEntry& getFirstVerified() const;
    const LedgerHeaderHistoryEntry& getLastVerified() const;
    const LedgerHeaderHistoryEntry& getLastApplied() const;

  private:
    std::shared_ptr<Work> mDownloadLedgersWork;
    std::shared_ptr<Work> mDownloadTransactionsWork;
    std::shared_ptr<Work> mVerifyWork;
    std::shared_ptr<Work> mApplyWork;

    TmpDir& mDownloadDir;
    uint32_t mFirstSeq;
    uint32_t mLastSeq;
    bool mManualCatchup;
    LedgerHeaderHistoryEntry mFirstVerified;
    LedgerHeaderHistoryEntry mLastVerified;
    LedgerHeaderHistoryEntry mLastApplied;
    std::string mCatchupTypeName;
};
}
