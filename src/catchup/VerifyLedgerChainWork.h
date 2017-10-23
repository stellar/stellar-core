// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "history/HistoryManager.h"
#include "ledger/CheckpointRange.h"
#include "work/Work.h"

namespace medida
{
class Meter;
}

namespace stellar
{

class TmpDir;
struct LedgerHeaderHistoryEntry;

class VerifyLedgerChainWork : public Work
{
    TmpDir const& mDownloadDir;
    CheckpointRange mRange;
    uint32_t mCurrSeq;
    bool mVerifyWithBufferedLedgers;
    bool mHadOk {false};
    LedgerHeaderHistoryEntry& mFirstVerified;
    LedgerHeaderHistoryEntry& mLastVerified;

    medida::Meter& mVerifyLedgerSuccessOld;
    medida::Meter& mVerifyLedgerSuccess;
    medida::Meter& mVerifyLedgerFailureOvershot;
    medida::Meter& mVerifyLedgerFailureLink;
    medida::Meter& mVerifyLedgerChainSuccess;
    medida::Meter& mVerifyLedgerChainFailure;
    medida::Meter& mVerifyLedgerChainFailureEnd;

    HistoryManager::VerifyHashStatus verifyHistoryOfSingleCheckpoint();

  public:
    VerifyLedgerChainWork(Application& app, WorkParent& parent,
                          TmpDir const& downloadDir, CheckpointRange range,
                          bool verifyWithBufferedLedgers,
                          LedgerHeaderHistoryEntry& firstVerified,
                          LedgerHeaderHistoryEntry& lastVerified);
    std::string getStatus() const override;
    void onReset() override;
    Work::State onSuccess() override;
};
}
