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

namespace medida
{
class Meter;
class Timer;
}

namespace stellar
{
class Application;
class SQLLogContext;

class Database
{
    Application& mApp;
    soci::session mSession;
    std::unique_ptr<soci::connection_pool> mPool;

    static bool gDriversRegistered;
    static void registerDrivers();

  public:
    Database(Application& app);

    std::shared_ptr<SQLLogContext> captureAndLogSQL(std::string contextName);

    medida::TimerContext getInsertTimer(std::string const& entityName);
    medida::TimerContext getSelectTimer(std::string const& entityName);
    medida::TimerContext getDeleteTimer(std::string const& entityName);
    medida::TimerContext getUpdateTimer(std::string const& entityName);

    bool isSqlite() const;
    bool canUsePool() const;

    void initialize();

    int64_t getBalance(const uint256& accountID, const Currency& currency);

    soci::session&
    getSession()
    {
        return mSession;
    }
    soci::connection_pool& getPool();
};
}
