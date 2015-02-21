// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"

#include "overlay/PeerMaster.h"
#include "database/Database.h"
#include "main/Application.h"
#include "main/Config.h"
#include "crypto/Hex.h"
#include "crypto/Base58.h"
#include "util/Logging.h"
#include "ledger/LedgerMaster.h"
#include "ledger/LedgerHeaderFrame.h"
#include "util/types.h"

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

bool
Database::gDriversRegistered = false;

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

Database::Database(Application& app)
    : mApp(app)
{
    registerDrivers();
    LOG(INFO) << "Connecting to: " << app.getConfig().DATABASE;
    mSession.open(app.getConfig().DATABASE);
    if( (mApp.getConfig().START_NEW_NETWORK) || 
        (app.getConfig().DATABASE == "sqlite3://:memory:"))  initialize();
}

bool
Database::isSqlite()
{
    return mApp.getConfig().DATABASE.find("sqlite3:") != std::string::npos;
}

void Database::initialize()
{
    AccountFrame::dropAll(*this);
    OfferFrame::dropAll(*this);
    TrustFrame::dropAll(*this);
    PeerMaster::dropAll(*this);
    LedgerMaster::dropAll(*this);
    LedgerHeaderFrame::dropAll(*this);
    TransactionFrame::dropAll(*this);
}


int64_t Database::getBalance(const uint256& accountID,const Currency& currency)
{
    int64_t amountFunded = 0;
    if(currency.type()==NATIVE)
    {
        AccountFrame account;
        if(AccountFrame::loadAccount(accountID, account, *this))
        {
            amountFunded = account.getAccount().balance;
        }
    } else
    {
        TrustFrame trustLine;
        if(TrustFrame::loadTrustLine(accountID, currency, trustLine, *this))
        {
            if(trustLine.getTrustLine().authorized)
                amountFunded = trustLine.getTrustLine().balance;
        }
    }

    return amountFunded;
}

}
