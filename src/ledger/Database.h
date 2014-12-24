#ifndef __LEDGERDATABASE__
#define __LEDGERDATABASE__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include <string>

namespace stellar
{
    class Database
    {
        Application& mApp;
    public:

        Database(Application& app);

        // state store
        enum StoreStateName {
            kLastClosedLedger = 0,
            kLastEntry };

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

        //bool loadOffer()


        soci::session& getSession() { return mSession; }

    private:
        soci::session mSession;
    };
}

#endif
