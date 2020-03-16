// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "ledger/LedgerRange.h"
#include "util/XDRStream.h"
#include "work/BatchWork.h"
#include "xdr/Stellar-ledger.h"

namespace medida
{
class Meter;
}

namespace stellar
{

class TmpDir;
class HistoryArchive;
struct LedgerHeaderHistoryEntry;

class DownloadApplyTxsWork : public BatchWork
{
    LedgerRange const mRange;
    TmpDir const& mDownloadDir;
    LedgerHeaderHistoryEntry& mLastApplied;
    uint32_t mCheckpointToQueue;
    std::shared_ptr<BasicWork> mLastYieldedWork;
    bool const mWaitForPublish;
    std::shared_ptr<HistoryArchive> mArchive;

  public:
    DownloadApplyTxsWork(Application& app, TmpDir const& downloadDir,
                         LedgerRange const& range,
                         LedgerHeaderHistoryEntry& lastApplied,
                         bool waitForPublish,
                         std::shared_ptr<HistoryArchive> archive = nullptr);

    std::string getStatus() const override;

  protected:
    bool hasNext() const override;
    std::shared_ptr<BasicWork> yieldMoreWork() override;
    void resetIter() override;
    void onSuccess() override;
};
}