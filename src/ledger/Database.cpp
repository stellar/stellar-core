// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Database.h"
#include "generated/StellarXDR.h"
#include "util/Logging.h"

namespace stellar
{
    Database::Database(Application& app) : mApp(app),
        mSession(mApp.getConfig().DATABASE_TYPE, mApp.getConfig().DATABASE_OPTIONS)
    {

    }

    bool Database::loadAccount(const stellarxdr::uint160& accountID, stellarxdr::LedgerEntry& retEntry)
    {
        return false;
    }

    bool Database::loadTrustLine(const stellarxdr::uint160& accountID,
        const stellarxdr::CurrencyIssuer& currency,
        stellarxdr::LedgerEntry& retEntry)
    {
        return false;
    }

    void getLines(const stellarxdr::uint160& accountID, const stellarxdr::Currency& currency, vector<TrustLine::pointer>& retList)
    {
        std::string base58ID;
        toBase58(accountID, base58ID);

        row r;

        sql << "SELECT * from TrustLines where lowAccount=" << base58ID
            << " and lowLimit>0 or balance<0", into(r);

        for(auto item : r)
        {

        }

        sql << "SELECT * from TrustLines where highAccount=" << base58ID
            << " and highLimit>0 or balance>0", into(r);
    }

    TrustLine::pointer getTrustline(const stellarxdr::uint160& accountID, const stellarxdr::CurrencyIssuer& currency)
    {
        std::string base58ID, base58Issuer;
        toBase58(accountID, base58ID);
        toBase58(currency.issuer, base58Issuer);

        uint64_t limit;
        int64_t balance;
        bool authorized;

        sql << "SELECT limit,balance,authorized from TrustLines where accountID=" << base58ID << " and issuer= " << base58Issuer << " and currency =" << ? ,
            into(limit), into(balance), into(authorized);

        return TrustLine::pointer();
    }


    /* 

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

