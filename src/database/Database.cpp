// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "database/Database.h"
#include "main/Application.h"
#include "crypto/Hex.h"
#include "crypto/Base58.h"
#include "util/Logging.h"

extern "C" void register_factory_sqlite3();

#ifdef USE_POSTGRES
extern "C" void register_factory_postgresql();
#endif

using namespace soci;

namespace stellar
{

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
    mSession.open(app.getConfig().DATABASE);
    if(mApp.getConfig().START_NEW_NETWORK) initialize();
}

void Database::initialize()
{
    try {
        AccountFrame::dropAll(*this);
        OfferFrame::dropAll(*this);
        TrustFrame::dropAll(*this);
        TxDelta::dropAll(*this);
    }catch(exception const &e)
    {
        LOG(ERROR) << "Error: " << e.what();
    }
}

bool Database::loadAccount(const uint256& accountID, AccountFrame& retAcc)
{
    std::string base58ID = toBase58Check(VER_ACCOUNT_ID, accountID);
    std::string publicKey, inflationDest, creditAuthKey;

    soci::indicator publicKeyInd, inflationDestInd, creditAuthKeyInd;

    retAcc.mEntry.type(ACCOUNT);
    retAcc.mEntry.account().accountID = accountID;
    AccountEntry& account = retAcc.mEntry.account();
    mSession << "SELECT balance,sequence,ownerCount,transferRate,publicKey, \
        inflationDest,creditAuthKey,flags from Accounts where accountID=:v1",
        into(account.balance), into(account.sequence), into(account.ownerCount),
        into(account.transferRate), into(publicKey, publicKeyInd), 
        into(inflationDest, inflationDestInd),
        into(creditAuthKey, creditAuthKeyInd), into(account.flags),
        use(base58ID);

    if(!mSession.got_data())
        return false;

    if(publicKeyInd==soci::i_ok) account.pubKey.activate() = fromBase58Check256(VER_ACCOUNT_PUBLIC, publicKey);
    if(inflationDestInd == soci::i_ok) account.inflationDest.activate() = fromBase58Check256(VER_ACCOUNT_PUBLIC, inflationDest);
    if(creditAuthKeyInd == soci::i_ok) account.creditAuthKey.activate() = fromBase58Check256(VER_ACCOUNT_PUBLIC, creditAuthKey);

    return true;
}

bool Database::loadTrustLine(const uint256& accountID,
    const CurrencyIssuer& currency,
    TrustFrame& retLine)
{
    std::string accStr,issuerStr,currencyStr;

    accStr = toBase58Check(VER_ACCOUNT_ID, accountID);
    currencyStr = binToHex(currency.currencyCode);
    issuerStr = binToHex(issuerStr);

    retLine.mEntry.type(TRUSTLINE);
    retLine.mEntry.trustLine().accountID = accountID;
    int authInt;
    mSession << "SELECT tlimit,balance,authorized from TrustLines where \
        accountID=:v1 and issuer=:v2 and currency=:v3",
        into(retLine.mEntry.trustLine().limit),
        into(retLine.mEntry.trustLine().balance),
        into(authInt),
        use(accStr), use(issuerStr), use(currencyStr);
    if(!mSession.got_data())
        return false;
    
    retLine.mEntry.trustLine().authorized = authInt;
    retLine.mEntry.trustLine().accountID = accountID;
    retLine.mEntry.trustLine().currencyCode = currency.currencyCode;
    retLine.mEntry.trustLine().issuer = currency.issuer;

    return true;
}

bool Database::loadOffer(const uint256& accountID, uint32_t seq, OfferFrame& retOffer)
{
    std::string accStr;
    accStr = toBase58Check(VER_ACCOUNT_ID, accountID);
 
    stringstream sql;
    sql << "SELECT * from Offers where accountID='" << accStr << "' and sequence=" << seq;
    rowset<row> rs = mSession.prepare << sql.str();
    rowset<row>::const_iterator it = rs.begin();
    if(rs.end() == it) return false;
    row const& row = *it;
    loadOffer(row, retOffer);
    
    return true;
}

/*
0 offerIndex CHARACTER(35) PRIMARY KEY, \
1 accountID		CHARACTER(35), \
2 sequence		INT UNSIGNED, \
3 takerPaysCurrency Blob(20), \
4 takerPaysIssuer CHARACTER(35), \
5 takerGetsCurrency Blob(20), \
6 takerGetsIssuer CHARACTER(35), \
7 amount BIGINT UNSIGNED, \
8 price BIGINT UNSIGNED, \
9 flags INT UNSIGNED					    \
*/
void Database::loadOffer(const soci::row& row, OfferFrame& retOffer)
{
    retOffer.mEntry.type(OFFER);
    retOffer.mEntry.offer().accountID = fromBase58Check256(VER_ACCOUNT_ID,row.get<std::string>(1));
    retOffer.mEntry.offer().sequence = row.get<uint64_t>(2);
    if(row.get_indicator(3))
    {
        retOffer.mEntry.offer().takerPays.native(false);
        retOffer.mEntry.offer().takerPays.ci().currencyCode=fromBase58Check256(VER_ACCOUNT_ID,row.get<std::string>(3));
        retOffer.mEntry.offer().takerPays.ci().issuer = fromBase58Check256(VER_ACCOUNT_ID, row.get<std::string>(4));
    } else
    {
        retOffer.mEntry.offer().takerPays.native(true);
    }
    if(row.get_indicator(5))
    {
        retOffer.mEntry.offer().takerGets.native(false);
        retOffer.mEntry.offer().takerGets.ci().currencyCode = fromBase58Check256(VER_ACCOUNT_ID, row.get<std::string>(5));
        retOffer.mEntry.offer().takerGets.ci().issuer = fromBase58Check256(VER_ACCOUNT_ID, row.get<std::string>(6));
    } else
    {
        retOffer.mEntry.offer().takerGets.native(true);
    }
    retOffer.mEntry.offer().amount = row.get<uint64_t>(7);
    retOffer.mEntry.offer().price = row.get<uint64_t>(8);
    retOffer.mEntry.offer().flags = row.get<int32_t>(9);
}

/*
0 trustIndex CHARACTER(35) PRIMARY KEY,				\
1 accountID	CHARACTER(35),			\
2 issuer CHARACTER(35),				\
3 currency CHARACTER(35),				\
4 tlimit UNSIGNED INT,		   		\
5 balance UNSIGNED INT,				\
6 authorized BOOL						\
*/

void Database::loadLine(const soci::row& row, TrustFrame& retLine)
{
    retLine.mEntry.type(TRUSTLINE);
    retLine.mEntry.trustLine().accountID = fromBase58Check256(VER_ACCOUNT_ID, row.get<std::string>(1));
    retLine.mEntry.trustLine().issuer = fromBase58Check256(VER_ACCOUNT_ID, row.get<std::string>(2));
    retLine.mEntry.trustLine().currencyCode = fromBase58Check256(VER_ACCOUNT_ID, row.get<std::string>(3));
    retLine.mEntry.trustLine().limit = row.get<uint64_t>(4);
    retLine.mEntry.trustLine().balance = row.get<uint64_t>(5);
    retLine.mEntry.trustLine().authorized = row.get<uint32_t>(6);
}

void Database::loadBestOffers(int numOffers, int offset, Currency& pays,
    Currency& gets, vector<OfferFrame>& retOffers)
{
    
    stringstream sql;
    sql << "SELECT * from Offers where ";
    if(pays.native())
    {
        std::string b58Currency, b58Issuer;
        b58Currency=toBase58Check(VER_ACCOUNT_ID, gets.ci().currencyCode);
        b58Issuer=toBase58Check(VER_ACCOUNT_ID, gets.ci().issuer);
        sql << "takerPaysIssuer is NULL and takerGetsCurrency='" << b58Currency << "' and takerGetsIssuer='" << b58Issuer << "' ";
    } else if(gets.native())
    {
        std::string b58Currency, b58Issuer;
        b58Currency = toBase58Check(VER_ACCOUNT_ID, pays.ci().currencyCode);
        b58Issuer = toBase58Check(VER_ACCOUNT_ID, pays.ci().issuer);
        sql << "takerGetsIssuer is NULL and takerPaysCurrency='" << b58Currency << "' and takerPaysIssuer='" << b58Issuer << "' ";
    } else
    {
        std::string b58GCurrency, b58GIssuer;
        std::string b58PCurrency, b58PIssuer;
        b58GCurrency = toBase58Check(VER_ACCOUNT_ID, gets.ci().currencyCode);
        b58GIssuer = toBase58Check(VER_ACCOUNT_ID, gets.ci().issuer);
        sql << "takerGetsCurrency='" << b58GCurrency << "' and takerGetsIssuer='" << b58GIssuer << "' ";
        
        b58PCurrency = toBase58Check(VER_ACCOUNT_ID, pays.ci().currencyCode);
        b58PIssuer = toBase58Check(VER_ACCOUNT_ID, pays.ci().issuer);
        sql << "takerPaysCurrency='" << b58PCurrency << "' and takerPaysIssuer='" << b58PIssuer << "' ";
    }
    sql << " order by price limit " << offset << " ," << numOffers;
    rowset<row> rs = mSession.prepare << sql.str();
    for(rowset<row>::const_iterator it = rs.begin(); it != rs.end(); ++it)
    {
        row const& row = *it;
        retOffers.resize(retOffers.size() + 1);
        loadOffer(row, retOffers[retOffers.size() - 1]);
    }
}

void Database::loadOffers(const uint256& accountID, std::vector<OfferFrame>& retOffers)
{
    std::string accStr;
    accStr = toBase58Check(VER_ACCOUNT_ID, accountID);

    stringstream sql;
    sql << "SELECT * from Offers where accountID='" << accStr << "' ";
    rowset<row> rs = mSession.prepare << sql.str();
    for(rowset<row>::const_iterator it = rs.begin(); it != rs.end(); ++it)
    {
        row const& row = *it;
        retOffers.resize(retOffers.size() + 1);
        loadOffer(row, retOffers[retOffers.size() - 1]);
    }
}

void Database::loadLines(const uint256& accountID, std::vector<TrustFrame>& retLines)
{
    std::string accStr;
    accStr = toBase58Check(VER_ACCOUNT_ID, accountID);

    stringstream sql;
    sql << "SELECT * from TrustLines where accountID='" << accStr << "' ";
    rowset<row> rs = mSession.prepare << sql.str();
    for(rowset<row>::const_iterator it = rs.begin(); it != rs.end(); ++it)
    {
        row const& row = *it;
        retLines.resize(retLines.size() + 1);
        loadLine(row, retLines[retLines.size() - 1]);
    }
}

int64_t Database::getBalance(const uint256& accountID,const Currency& currency)
{
    int64_t amountFunded = 0;
    if(currency.native())
    {
        AccountFrame account;
        if(loadAccount(accountID, account))
        {
            amountFunded = account.mEntry.account().balance;
        }
    } else
    {
        TrustFrame trustLine;
        if(loadTrustLine(accountID, currency.ci(), trustLine))
        {
            if(trustLine.mEntry.trustLine().authorized)
                amountFunded = trustLine.mEntry.trustLine().balance;
        }
    }

    return amountFunded;
}

void Database::beginTransaction() {
    /* TODO.2
    mDBCon->getDBLock().lock();
    try {
        mDBCon->getDB()->beginTransaction();
    }
    catch(...) {
        mDBCon->getDBLock().unlock();
        throw;
    }
    */
}

void Database::endTransaction(bool rollback) {
    /* TODO.2
    try {
        mDBCon->getDB()->endTransaction(rollback);
    }
    catch(...) {
        mDBCon->getDBLock().unlock();
        throw;
    }
    mDBCon->getDBLock().unlock();
    */
}

/*
void Database::getLines(const uint160& accountID, const Currency& currency, vector<TrustLine::pointer>& retList)
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

TrustLine::pointer getTrustline(const uint160& accountID, const CurrencyIssuer& currency)
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
}*/


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



int LedgerDatabase::getTransactionLevel() {
return mDBCon->getDB()->getTransactionLevel();
}
*/

}
