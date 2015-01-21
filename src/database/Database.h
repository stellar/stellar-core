#ifndef __DATABASE__
#define __DATABASE__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <string>
#include <soci.h>
#include "generated/StellarXDR.h"
#include "ledger/AccountFrame.h"
#include "ledger/OfferFrame.h"
#include "ledger/TrustFrame.h"

#define OFFER_PRICE_DIVISOR 10000000
#define TRANSFER_RATE_DIVISOR 10000000
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

    void loadOffer(const soci::row& row, OfferFrame& offer);
    void loadLine(const soci::row& row, TrustFrame& offer);
  public:
    Database(Application& app);

    void initialize();

    bool loadAccount(const uint256& accountID, AccountFrame& retEntry, bool withSig=false);
    bool loadTrustLine(const uint256& accountID, const Currency& currency,
        TrustFrame& retEntry);
    bool loadOffer(const uint256& accountID,uint32_t seq, OfferFrame& retEntry);

    void loadBestOffers(size_t numOffers, size_t offset, Currency& pays,
        Currency& gets, std::vector<OfferFrame>& retOffers);

    void loadOffers(const uint256& accountID, std::vector<OfferFrame>& retOffers);
    void loadLines(const uint256& accountID, std::vector<TrustFrame>& retLines);

    int64_t getBalance(const uint256& accountID, const Currency& currency);
    
    void addPeer(const std::string& ip, int port,int numFailures, int rank);
    void loadPeers(int max, std::vector<PeerRecord>& retList);


    soci::session& getSession() { return mSession; }
};
}

#endif
