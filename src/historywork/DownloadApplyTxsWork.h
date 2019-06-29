// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "herder/TxSetFrame.h"
#include "ledger/LedgerRange.h"
#include "util/XDRStream.h"
#include "work/BatchWork.h"
#include "work/Work.h"
#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-ledger.h"

namespace medida
{
class Meter;
}

namespace stellar
{

class TmpDir;
struct LedgerHeaderHistoryEntry;

class DownloadApplyTxsWork : public BatchWork
{
    LedgerRange const mRange;
    TmpDir const& mDownloadDir;
    LedgerHeaderHistoryEntry& mLastApplied;
    uint32_t mCheckpointToQueue;
    mutable std::list<std::shared_ptr<BasicWork>> mRunning;

    void wakeWaitingWork();

  public:
    DownloadApplyTxsWork(Application& app, TmpDir const& downloadDir,
                         LedgerRange range,
                         LedgerHeaderHistoryEntry& lastApplied);
    virtual ~DownloadApplyTxsWork() = default;

  protected:
    bool hasNext() override;
    std::shared_ptr<BasicWork> yieldMoreWork() override;
    void resetIter() override;
    void onSuccess() override;
};
}
