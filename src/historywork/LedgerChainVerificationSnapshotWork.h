// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "catchup/VerifyLedgerChainWork.h"
#include "work/BatchableWork.h"
#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "main/Application.h"

namespace stellar
{

class LedgerChainVerificationSnapshotWork : public BatchableWork
{
    // Class that coordinates download and verification logistics.
    // Additionally, it implements notification mechanism and
    // prevents blocked work from running.

    std::shared_ptr<VerifyLedgerChainWork> mDownloadVerifyWork;

    uint32_t mCheckpoint;
    LedgerRange const& mRange;
    LedgerHeaderHistoryEntry& mFirstVerified;
    LedgerHeaderHistoryEntry const& mLastClosedLedger;
    TmpDir const& mDownloadDir;
    optional<Hash> mTrustedHash;
    LedgerHeaderHistoryEntry mVerifiedAhead;

    bool downloadAndVerifyChain();

  protected:
    void unblockWork(BatchableWorkResultData const& data) override;

  public:
    LedgerChainVerificationSnapshotWork(
        Application& app, WorkParent& parent, TmpDir const& downloadDir,
        uint32_t checkpoint, LedgerRange const& range,
        LedgerHeaderHistoryEntry& firstVerified,
        LedgerHeaderHistoryEntry const& lastClosedLedger,
        optional<Hash> scpHash);
    ~LedgerChainVerificationSnapshotWork() override;

    std::string getStatus() const override;
    void onReset() override;
    Work::State onSuccess() override;

    uint32_t
    getCheckpointLedger()
    {
        return mCheckpoint;
    }
};
}
