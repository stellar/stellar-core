// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "ledger/CheckpointRange.h"
#include "ledger/LedgerRange.h"
#include "historywork/BatchWork.h"
#include "util/optional.h"
#include <medida/meter.h>
#include <medida/metrics_registry.h>

namespace stellar
{

class DownloadVerifyLedgersWork : public BatchWork
{
    // Batch commander class that processes
    // ledger chain snapshots (snapshot means sequential download and verify of
    // a particular checkpoint) Because we want to fail fast, chain verification
    // happens from the latest ledger in the range so that it can be verified
    // against SCP hash right away.
    LedgerRange mRange;
    CheckpointRange mCheckpointRange;
    uint32_t mNext;
    TmpDir const& mDownloadDir;

    LedgerHeaderHistoryEntry& mFirstVerified;
    LedgerHeaderHistoryEntry const& mLastClosedLedger;
    optional<Hash> mTrustedHash;

    // Download Metrics
    medida::Meter& mDownloadStart;
    medida::Meter& mDownloadSuccess;
    medida::Meter& mDownloadFailure;

  public:
    DownloadVerifyLedgersWork(Application& app, WorkParent& parent,
                              LedgerRange range, TmpDir const& downloadDir,
                              LedgerHeaderHistoryEntry const& lcl,
                              LedgerHeaderHistoryEntry& firstVerified,
                              optional<Hash> scpHash);
    ~DownloadVerifyLedgersWork() override;
    std::string getStatus() const override;
    void notify(std::string const& child) override;

    bool hasNext() override;
    std::shared_ptr<BatchableWork> yieldMoreWork() override;
    void resetIter() override;
};
}
