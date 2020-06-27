#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/CatchupManager.h"
#include "catchup/CatchupWork.h"
#include <memory>

namespace medida
{
class Meter;
class Counter;
}

namespace stellar
{

class Application;
class Work;

class CatchupManagerImpl : public CatchupManager
{
    Application& mApp;
    std::shared_ptr<CatchupWork> mCatchupWork;

    // key is ledgerSeq
    std::map<uint32_t, LedgerCloseData> mSyncingLedgers;
    medida::Counter& mSyncingLedgersSize;

    void addToSyncingLedgers(LedgerCloseData const& ledgerData);
    void startOnlineCatchup();
    void trimSyncingLedgers();
    void trimAndReset();
    void tryApplySyncingLedgers();
    uint32_t getCatchupCount();

  public:
    CatchupManagerImpl(Application& app);
    ~CatchupManagerImpl() override;

    void processLedger(LedgerCloseData const& ledgerData) override;
    void startCatchup(CatchupConfiguration configuration,
                      std::shared_ptr<HistoryArchive> archive) override;

    std::string getStatus() const override;

    BasicWork::State getCatchupWorkState() const override;
    bool catchupWorkIsDone() const override;
    bool isCatchupInitialized() const override;

    void logAndUpdateCatchupStatus(bool contiguous,
                                   std::string const& message) override;
    void logAndUpdateCatchupStatus(bool contiguous) override;

    bool hasBufferedLedger() const override;
    LedgerCloseData const& getFirstBufferedLedger() const override;
    LedgerCloseData const& getLastBufferedLedger() const override;
    void popBufferedLedger() override;

    void syncMetrics() override;
};
}
