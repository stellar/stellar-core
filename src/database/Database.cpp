// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "database/Database.h"
#include "generated/StellarXDR.h"
#include "overlay/PeerMaster.h"
#include "main/Application.h"
#include "main/Config.h"
#include "crypto/Hex.h"
#include "crypto/Base58.h"
#include "util/Logging.h"
#include "ledger/LedgerMaster.h"
#include "ledger/LedgerHeaderFrame.h"
#include "util/types.h"
#include "util/make_unique.h"
#include "medida/metrics_registry.h"
#include "medida/timer.h"

#include <stdexcept>
#include <vector>
#include <sstream>

extern "C" void register_factory_sqlite3();

#ifdef USE_POSTGRES
extern "C" void register_factory_postgresql();
#endif

// NOTE: soci will just crash and not throw
//  if you misname a column in a query. yay!

namespace stellar
{

using namespace soci;
using namespace std;

bool Database::gDriversRegistered = false;

static void
setSerializable(soci::session& sess)
{
    sess << "SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL "
            "SERIALIZABLE";
}

void
Database::registerDrivers()
{
    if (!gDriversRegistered)
    {
        register_factory_sqlite3();
#ifdef USE_POSTGRES
        register_factory_postgresql();
#endif
        gDriversRegistered = true;
    }
}

Database::Database(Application& app) : mApp(app)
{
    registerDrivers();
    CLOG(INFO, "Database") << "Connecting to: " << app.getConfig().DATABASE;
    mSession.open(app.getConfig().DATABASE);
    if (isSqlite())
    {
        mSession << "PRAGMA journal_mode = WAL";
    }
    else
    {
        setSerializable(mSession);
    }
}

medida::TimerContext
Database::getInsertTimer(std::string const& entityName)
{
    return mApp.getMetrics()
        .NewTimer({"database", "insert", entityName})
        .TimeScope();
}

medida::TimerContext
Database::getSelectTimer(std::string const& entityName)
{
    return mApp.getMetrics()
        .NewTimer({"database", "select", entityName})
        .TimeScope();
}

medida::TimerContext
Database::getDeleteTimer(std::string const& entityName)
{
    return mApp.getMetrics()
        .NewTimer({"database", "delete", entityName})
        .TimeScope();
}

medida::TimerContext
Database::getUpdateTimer(std::string const& entityName)
{
    return mApp.getMetrics()
        .NewTimer({"database", "update", entityName})
        .TimeScope();
}

bool
Database::isSqlite() const
{
    return mApp.getConfig().DATABASE.find("sqlite3:") != std::string::npos;
}

bool
Database::canUsePool() const
{
    return !(mApp.getConfig().DATABASE == ("sqlite3://:memory:"));
}

void
Database::initialize()
{
    AccountFrame::dropAll(*this);
    OfferFrame::dropAll(*this);
    TrustFrame::dropAll(*this);
    PeerMaster::dropAll(*this);
    PersistentState::dropAll(*this);
    LedgerHeaderFrame::dropAll(*this);
    TransactionFrame::dropAll(*this);
}

soci::connection_pool&
Database::getPool()
{
    if (!mPool)
    {
        std::string const& c = mApp.getConfig().DATABASE;
        if (!canUsePool())
        {
            std::string s("Can't create connection pool to ");
            s += c;
            throw std::runtime_error(s);
        }
        size_t n = std::thread::hardware_concurrency();
        LOG(INFO) << "Establishing " << n << "-entry connection pool to: " << c;
        mPool = make_unique<soci::connection_pool>(n);
        for (size_t i = 0; i < n; ++i)
        {
            LOG(DEBUG) << "Opening pool entry " << i;
            soci::session& sess = mPool->at(i);
            sess.open(c);
            if (!isSqlite())
            {
                setSerializable(sess);
            }
        }
    }
    assert(mPool);
    return *mPool;
}

int64_t
Database::getBalance(const uint256& accountID, const Currency& currency)
{
    int64_t amountFunded = 0;
    if (currency.type() == NATIVE)
    {
        AccountFrame account;
        if (AccountFrame::loadAccount(accountID, account, *this))
        {
            amountFunded = account.getAccount().balance;
        }
    }
    else
    {
        TrustFrame trustLine;
        if (TrustFrame::loadTrustLine(accountID, currency, trustLine, *this))
        {
            if (trustLine.getTrustLine().authorized)
                amountFunded = trustLine.getTrustLine().balance;
        }
    }

    return amountFunded;
}
}
