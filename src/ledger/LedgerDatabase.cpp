#include "LedgerDatabase.h"
#include "generated/StellarXDR.h"

#include "util/Logging.h"

namespace stellar
{
    /* NICOLAS
    LedgerDatabase::LedgerDatabase(ripple::DatabaseCon *dbCon) : mDBCon(dbCon) {
    }

    const char *LedgerDatabase::getStoreStateName(StoreStateName n) {
        static const char *mapping[kLastEntry] = { "lastClosedLedger" };
        if (n < 0 || n >= kLastEntry) {
            throw out_of_range("unknown entry");
        }
        return mapping[n];
    }

    string LedgerDatabase::getState(const char *stateName) {
        string res;
        string sql = str(boost::format("SELECT State FROM StoreState WHERE StateName = '%s';")
            % stateName
            );
        if (mDBCon->getDB()->executeSQL(sql))
        {
            mDBCon->getDB()->getStr(0, res);
        }
        return res;
    }

    void LedgerDatabase::setState(const char *stateName, const char *value) {
        string sql = str(boost::format("INSERT OR REPLACE INTO StoreState (StateName, State) VALUES ('%s','%s');")
            % stateName
            % value
            );
        if (!mDBCon->getDB()->executeSQL(sql))
        {
            CLOG(ripple::WARNING, ripple::Ledger) << "SQL failed: " << sql;
            throw std::runtime_error("could not update state in database");
        }
    }

    void LedgerDatabase::beginTransaction() {
        mDBCon->getDBLock().lock();
        try {
            mDBCon->getDB()->beginTransaction();
        }
        catch (...) {
            mDBCon->getDBLock().unlock();
            throw;
        }
    }

    void LedgerDatabase::endTransaction(bool rollback) {
        try {
            mDBCon->getDB()->endTransaction(rollback);
        }
        catch (...) {
            mDBCon->getDBLock().unlock();
            throw;
        }
        mDBCon->getDBLock().unlock();
    }

    int LedgerDatabase::getTransactionLevel() {
        return mDBCon->getDB()->getTransactionLevel();
    }
    */
}; 

