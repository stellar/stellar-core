// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "history/HistoryManager.h"
#include "ledger/LedgerRange.h"
#include "work/Work.h"

namespace medida
{
class Meter;
}

namespace stellar
{

class TmpDir;
struct LedgerHeaderHistoryEntry;

class VerifyLedgerChainWork : public BasicWork
{
    TmpDir const& mDownloadDir;
    LedgerRange const mRange;
    uint32_t mCurrCheckpoint;
    bool const mManualCatchup;
    LedgerHeaderHistoryEntry& mFirstVerified;
    LedgerHeaderHistoryEntry& mLastVerified;

    medida::Meter& mVerifyLedgerSuccessOld;
    medida::Meter& mVerifyLedgerSuccess;
    medida::Meter& mVerifyLedgerFailureLedgerVersion;
    medida::Meter& mVerifyLedgerFailureOvershot;
    medida::Meter& mVerifyLedgerFailureLink;
    medida::Meter& mVerifyLedgerChainSuccess;
    medida::Meter& mVerifyLedgerChainFailure;
    medida::Meter& mVerifyLedgerChainFailureEnd;

    HistoryManager::LedgerVerificationStatus verifyHistoryOfSingleCheckpoint();

  public:
    VerifyLedgerChainWork(Application& app, TmpDir const& downloadDir,
                          LedgerRange range, bool manualCatchup,
                          LedgerHeaderHistoryEntry& firstVerified,
                          LedgerHeaderHistoryEntry& lastVerified);
    ~VerifyLedgerChainWork() = default;
    std::string getStatus() const override;

  protected:
    void onReset() override;
    BasicWork::State onRun() override;
    bool
    onAbort() override
    {
        return true;
    };
};
}
