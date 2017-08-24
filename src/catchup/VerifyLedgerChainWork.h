// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "history/HistoryManager.h"
#include "work/Work.h"

namespace stellar
{

class TmpDir;
struct LedgerHeaderHistoryEntry;

class VerifyLedgerChainWork : public Work
{
    TmpDir const& mDownloadDir;
    uint32_t mFirstSeq;
    uint32_t mCurrSeq;
    uint32_t mLastSeq;
    bool mManualCatchup;
    LedgerHeaderHistoryEntry& mFirstVerified;
    LedgerHeaderHistoryEntry& mLastVerified;

    HistoryManager::VerifyHashStatus verifyHistoryOfSingleCheckpoint();

  public:
    VerifyLedgerChainWork(Application& app, WorkParent& parent,
                          TmpDir const& downloadDir, uint32_t firstSeq,
                          uint32_t lastSeq, bool manualCatchup,
                          LedgerHeaderHistoryEntry& firstVerified,
                          LedgerHeaderHistoryEntry& lastVerified);
    std::string getStatus() const override;
    void onReset() override;
    Work::State onSuccess() override;
};
}
