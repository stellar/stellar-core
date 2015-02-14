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
    PeerMaster::createTable(*this);
    LedgerMaster::dropAll(*this);
    LedgerHeaderFrame::dropAll(*this);
    TransactionFrame::dropAll(*this);
}

void Database::addPeer(const std::string& ip, int port,int numFailures, int rank)
{
    try {
        int peerID;
        mSession << "SELECT peerID from Peers where ip=:v1 and port=:v2",
            into(peerID), use(ip), use(port);
        if(!mSession.got_data())
        {
            mSession << "INSERT INTO Peers (IP,Port,numFailures,Rank) values (:v1, :v2, :v3, :v4)",
                use(ip), use(port), use(numFailures), use(rank);
        }
    }
    catch(soci_error& err)
    {
        LOG(ERROR) << "DB addPeer: " << err.what();
    }
}

void Database::loadPeers(int max, vector<PeerRecord>& retList)
{
    try {
        std::tm nextAttempt = VirtualClock::pointToTm(mApp.getClock().now());
        rowset<row> rs =
            (mSession.prepare <<
             "SELECT peerID,ip,port,numFailures from Peers "
             " where nextAttempt < :nextAttempt "
             " order by rank limit :max ",
             use(nextAttempt), use(max));
        for(rowset<row>::const_iterator it = rs.begin(); it != rs.end(); ++it)
        {
            row const& row = *it;
            retList.push_back(PeerRecord(row.get<int>(0), row.get<std::string>(1), row.get<int>(2), row.get<int>(3)));
        }
    }
    catch(soci_error& err)
    {
        LOG(ERROR) << "loadPeers Error: " << err.what();
    }
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
