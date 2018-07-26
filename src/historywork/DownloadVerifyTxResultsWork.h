// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "historywork/BatchWork.h"
#include "ledger/CheckpointRange.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"

namespace stellar
{

class DownloadVerifyTxResultsWork : public BatchWork
{
    // Batch commander class that downloads and verifies
    // transaction results snapshots for given checkpoint range.
    // Note that each snapshot verification is independent,
    // and this requires no work blocking.
    CheckpointRange mCheckpointRange;
    TmpDir const& mDownloadDir;
    uint32_t mNext;

    // Download Metrics
    medida::Meter& mDownloadStart;
    medida::Meter& mDownloadSuccess;
    medida::Meter& mDownloadFailure;

  public:
    DownloadVerifyTxResultsWork(Application& app, WorkParent& parent,
                                CheckpointRange range,
                                TmpDir const& downloadDir);
    ~DownloadVerifyTxResultsWork() override;
    std::string getStatus() const override;
    void notify(std::string const& child) override;

    void resetIter() override;
    bool hasNext() override;
    std::shared_ptr<BatchableWork> yieldMoreWork() override;
};
}
