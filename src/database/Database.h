#ifndef __DATABASE__
#define __DATABASE__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <string>
#include <soci.h>
#include "generated/StellarXDR.h"

#define OFFER_PRICE_DIVISOR 1000000000

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

    // state store
    enum StoreStateName {
        kLastClosedLedger = 0,
        kLastEntry
    };

    const char *getStoreStateName(StoreStateName n);
    std::string getState(const char *stateName);
    void setState(const char *stateName, const char *value);

    // transaction helpers
    void beginTransaction();
    void endTransaction(bool rollback);
    int getTransactionLevel();

    bool loadAccount(const stellarxdr::uint160& accountID, stellarxdr::LedgerEntry& retEntry);
    bool loadTrustLine(const stellarxdr::uint160& accountID,
        const stellarxdr::CurrencyIssuer& currency,
        stellarxdr::LedgerEntry& retEntry);
    bool loadOffer(const stellarxdr::uint160& accountID,uint32_t seq, stellarxdr::LedgerEntry& retEntry);

    void loadBestOffers(int numOffers, int offset, stellarxdr::CurrencyIssuer& pays,
        stellarxdr::CurrencyIssuer& gets, std::vector<stellarxdr::LedgerEntry>& retOffers);

    //bool loadOffer()


    soci::session& getSession() { return mSession; }
};
}

#endif
