#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include <string>

namespace stellar
{

class PersistentState
{
  public:
    PersistentState(Application& app);

    enum Entry
    {
        kLastClosedLedger = 0,
        kHistoryArchiveState,
        kLastSCPData,
        kDatabaseSchema,
        kNetworkPassphrase,
        kLedgerUpgrades,
        kLastEntry,
    };

    static void dropAll(Database& db);

    std::string getState(Entry stateName);
    void setState(Entry stateName, std::string const& value);

    // Special methods for SCP state (multiple slots)
    std::vector<std::string> getSCPStateAllSlots();
    void setSCPStateForSlot(uint64 slot, std::string const& value);

  private:
    static std::string kSQLCreateStatement;
    static std::string mapping[kLastEntry];

    Application& mApp;

    std::string getStoreStateName(Entry n, uint32 subscript = 0);
    void updateDb(std::string const& entry, std::string const& value);
    std::string getFromDb(std::string const& entry);
};
}
