#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ApplicationImpl.h"
#include "main/Config.h"
#include "util/Timer.h"

namespace stellar
{

// A special "relay-mode" application that only participates in networking:
// it has all of its local state (database, buckets, ledger manager, etc.)
// removed.

class RelayApplication : public ApplicationImpl
{
  public:
    RelayApplication(VirtualClock& clock, Config const& cfg);

    virtual void initialize(InitialDBMode initDBMode) override;

  private:
    virtual std::unique_ptr<LedgerManager> createLedgerManager() override;
    virtual std::unique_ptr<OverlayManager> createOverlayManager() override;
    virtual std::unique_ptr<HistoryArchiveManager>
    createHistoryArchiveManager() override;
    virtual std::unique_ptr<HistoryManager> createHistoryManager() override;
    virtual std::unique_ptr<Herder> createHerder() override;
    virtual std::unique_ptr<HerderPersistence>
    createHerderPersistence() override;
    virtual std::unique_ptr<Database> createDatabase() override;
    virtual std::unique_ptr<BucketManager> createBucketManager() override;
    virtual std::unique_ptr<CatchupManager> createCatchupManager() override;
    virtual std::unique_ptr<Maintainer> createMaintainer() override;
    virtual std::unique_ptr<CommandHandler> createCommandHandler() override;
    virtual std::unique_ptr<BanManager> createBanManager() override;
    virtual std::unique_ptr<LedgerTxnRoot> createLedgerTxnRoot() override;
    virtual std::shared_ptr<WorkScheduler> createWorkScheduler() override;
    virtual std::unique_ptr<PersistentState> createPersistentState() override;
    virtual std::shared_ptr<ProcessManager> createProcessManager() override;
};
}
