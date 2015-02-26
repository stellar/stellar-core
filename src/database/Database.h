#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <string>
#include <soci.h>
#include "generated/StellarXDR.h"
#include "ledger/AccountFrame.h"
#include "ledger/OfferFrame.h"
#include "ledger/TrustFrame.h"
#include "medida/timer_context.h"

namespace medida { class Meter; class Timer; }

namespace stellar
{
class Application;

class Database
{
    Application& mApp;
    soci::session mSession;

    static bool gDriversRegistered;
    static void registerDrivers();

  public:
    Database(Application& app);

    medida::TimerContext getInsertTimer(std::string const& entityName);
    medida::TimerContext getSelectTimer(std::string const& entityName);
    medida::TimerContext getDeleteTimer(std::string const& entityName);
    medida::TimerContext getUpdateTimer(std::string const& entityName);

    bool isSqlite();

    void initialize();

    int64_t getBalance(const uint256& accountID, const Currency& currency);

    soci::session& getSession() { return mSession; }
};
}


