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

class VerifyLedgerChainWork : public Work
{
    TmpDir const& mDownloadDir;
    LedgerRange mRange;
    uint32_t mCurrCheckpoint;
    LedgerHeaderHistoryEntry& mFirstVerified;
    LedgerHeaderHistoryEntry const& mLastClosedLedger;
    optional<Hash> mTrustedHash;

    // First ledger of last verified checkpoint. Needed for a checkpoint that
    // is being verified: last ledger in current checkpoint must agree with
    // mVerifiedAhead
    LedgerHeaderHistoryEntry mVerifiedAhead{};

    medida::Meter& mVerifyLedgerSuccessOld;
    medida::Meter& mVerifyLedgerSuccess;
    medida::Meter& mVerifyLedgerFailureLedgerVersion;
    medida::Meter& mVerifyLedgerFailureOvershot;
    medida::Meter& mVerifyLedgerFailureUndershot;
    medida::Meter& mVerifyLedgerFailureLink;
    medida::Meter& mVerifyLedgerChainSuccess;
    medida::Meter& mVerifyLedgerChainFailure;
    medida::Meter& mVerifyLedgerChainFailureEnd;

    HistoryManager::LedgerVerificationStatus verifyHistoryOfSingleCheckpoint();
    HistoryManager::LedgerVerificationStatus
    verifyAgainstTrustedHash(LedgerHeaderHistoryEntry& ledger);

  public:
    VerifyLedgerChainWork(Application& app, WorkParent& parent,
                          TmpDir const& downloadDir, LedgerRange range,
                          LedgerHeaderHistoryEntry& firstVerified,
                          LedgerHeaderHistoryEntry const& lastClosedLedger,
                          optional<Hash> scpHash);
    ~VerifyLedgerChainWork() override;
    std::string getStatus() const override;
    void onReset() override;
    Work::State onSuccess() override;
};
}
