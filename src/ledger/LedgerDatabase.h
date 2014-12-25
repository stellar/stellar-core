#ifndef __LEDGERDATABASE__
#define __LEDGERDATABASE__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <string>
#include "database/Database.h"

namespace stellar
{
    class LedgerDatabase
    {
    public:

        LedgerDatabase(Database* dbCon);

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

        Database* getDBCon() { return mDBCon; }

    private:
        Database* mDBCon;
    };
}

#endif
