// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "herder/LedgerCloseData.h"
#include "work/Work.h"
#include <memory>

namespace stellar
{

using SCPHistoryEntryVec = std::vector<std::shared_ptr<SCPHistoryEntry>>;

class ApplyLedgerWork : public BasicWork
{
    Application& mApp;
    LedgerCloseData const mLedgerCloseData;
    // SCP messages for the ledger to be applied
    std::unique_ptr<SCPHistoryEntryVec const> mHEntries;

  public:
    ApplyLedgerWork(Application& app, LedgerCloseData const& ledgerCloseData,
                    std::unique_ptr<SCPHistoryEntryVec const> hEntries);

    std::string getStatus() const override;

  protected:
    State onRun() override;
    bool onAbort() override;
};
}