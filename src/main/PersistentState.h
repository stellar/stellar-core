#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

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
        kForceSCPOnNextLaunch,
        kDatabaseInitialized,
        kLastEntry
    };

    static void dropAll(Database& db);

    std::string getStoreStateName(Entry n);

    std::string getState(Entry stateName);

    void setState(Entry stateName, const std::string& value);

  private:
    static std::string kSQLCreateStatement;
    static std::string mapping[kLastEntry];

    Application& mApp;
};
}
