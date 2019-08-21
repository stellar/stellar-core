#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include <string>

namespace stellar
{

class PersistentState
{
  public:
    static std::unique_ptr<PersistentState> create(Application& app);

    enum Entry
    {
        kLastClosedLedger = 0,
        kHistoryArchiveState,
        kForceSCPOnNextLaunch,
        kLastSCPData,
        kDatabaseSchema,
        kNetworkPassphrase,
        kLedgerUpgrades,
        kLastEntry,
    };

    static void dropAll(Database& db);

    virtual std::string getState(Entry stateName) = 0;
    virtual void setState(Entry stateName, std::string const& value) = 0;

    // Special methods for SCP state (multiple slots)
    virtual std::vector<std::string> getSCPStateAllSlots() = 0;
    virtual void setSCPStateForSlot(uint64 slot, std::string const& value) = 0;

    virtual ~PersistentState()
    {
    }
};
}
