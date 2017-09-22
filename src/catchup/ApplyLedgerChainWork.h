// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "herder/TxSetFrame.h"
#include "util/XDRStream.h"
#include "work/Work.h"
#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{

struct LedgerHeaderHistoryEntry;
class TmpDir;

class ApplyLedgerChainWork : public Work
{
    TmpDir const& mDownloadDir;
    uint32_t mFirstSeq;
    uint32_t mCurrSeq;
    uint32_t mLastSeq;
    XDRInputFileStream mHdrIn;
    XDRInputFileStream mTxIn;
    TransactionHistoryEntry mTxHistoryEntry;
    LedgerHeaderHistoryEntry& mLastApplied;

    TxSetFramePtr getCurrentTxSet();
    void openCurrentInputFiles();
    bool applyHistoryOfSingleLedger();

  public:
    ApplyLedgerChainWork(Application& app, WorkParent& parent,
                         TmpDir const& downloadDir, uint32_t first,
                         uint32_t last, LedgerHeaderHistoryEntry& lastApplied);
    std::string getStatus() const override;
    void onReset() override;
    void onStart() override;
    void onRun() override;
    Work::State onSuccess() override;
};
}
