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

namespace stellar
{
class Application;
class PeerRecord;

class Database
{
    Application& mApp;
    soci::session mSession;

    static bool gDriversRegistered;
    static void registerDrivers();

  public:
    Database(Application& app);

    bool isSqlite();

    void initialize();

    int64_t getBalance(const uint256& accountID, const Currency& currency);
    
    void addPeer(const std::string& ip, int port,int numFailures, int rank);
    void loadPeers(int max, std::vector<PeerRecord>& retList);


    soci::session& getSession() { return mSession; }
};
}


