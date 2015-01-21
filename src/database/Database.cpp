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

#include <stdexcept>
#include <vector>
#include <sstream>

extern "C" void register_factory_sqlite3();

#ifdef USE_POSTGRES
extern "C" void register_factory_postgresql();
#endif

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
    mSession.open(app.getConfig().DATABASE);
    if( (mApp.getConfig().START_NEW_NETWORK) || 
        (app.getConfig().DATABASE == "sqlite3://:memory:"))  initialize();
}

void Database::initialize()
{
    try {
        AccountFrame::dropAll(*this);
        OfferFrame::dropAll(*this);
        TrustFrame::dropAll(*this);
        PeerMaster::createTable(*this);
        LedgerMaster::dropAll(*this);
        LedgerHeaderFrame::dropAll(*this);
    }catch(exception const &e)
    {
        LOG(ERROR) << "Error: " << e.what();
    }
}

void Database::addPeer(const std::string& ip, int port,int numFailures, int rank)
{
    try {
        int peerID;
        mSession << "SELECT peerID from Peers where IP=:v1 and Port=:v2",
            into(peerID), use(ip), use(port);
        if(!mSession.got_data())
        {
            mSession.close();
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
        stringstream sql;
        sql << "SELECT peerID,ip,port,numFailures from Peers where nextAttempt<now() order by rank limit " << max;
        rowset<row> rs = mSession.prepare << sql.str();
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

bool Database::loadAccount(const uint256& accountID, AccountFrame& retAcc, bool withSig)
{
    std::string base58ID = toBase58Check(VER_ACCOUNT_ID, accountID);
    std::string publicKey, inflationDest, creditAuthKey;
    std::string thresholds;
    soci::indicator inflationDestInd, thresholdsInd;

    retAcc.mEntry.type(ACCOUNT);
    retAcc.mEntry.account().accountID = accountID;
    AccountEntry& account = retAcc.mEntry.account();
    mSession << "SELECT balance,sequence,ownerCount,transferRate, \
        inflationDest, thresholds,  flags from Accounts where accountID=:v1",
        into(account.balance), into(account.sequence), into(account.ownerCount),
        into(account.transferRate), into(inflationDest, inflationDestInd),
        into(thresholds, thresholdsInd), into(account.flags),
        use(base58ID);

    if(!mSession.got_data())
        return false;

    if (thresholdsInd == soci::i_ok)
    {
        std::vector<uint8_t> bin = hexToBin(thresholds);
        for (int n = 0; (n < 4) && (n < bin.size()); n++)
        {
            retAcc.mEntry.account().thresholds[n] = bin[n];
        }
    }

    if (inflationDestInd == soci::i_ok)
    {
        account.inflationDest.activate() = fromBase58Check256(VER_ACCOUNT_ID, inflationDest);
    }

    if(withSig)
    {
        stringstream sql;
        sql << "SELECT publicKey,weight from Signers where accountID='" << base58ID << "';";
        rowset<row> rs = mSession.prepare << sql.str();
        for(rowset<row>::const_iterator it = rs.begin(); it != rs.end(); ++it)
        {
            row const& row = *it;
            Signer signer;
            signer.pubKey=fromBase58Check256(VER_ACCOUNT_ID, row.get<std::string>(0));
            signer.weight = row.get<uint32_t>(1);
            account.signers.push_back(signer);
        }
    }
    return true;
}

bool Database::loadTrustLine(const uint256& accountID,
    const Currency& currency,
    TrustFrame& retLine)
{
    std::string accStr,issuerStr,currencyStr;

    accStr = toBase58Check(VER_ACCOUNT_ID, accountID);
    currencyCodeToStr(currency.isoCI().currencyCode, currencyStr);
    issuerStr = toBase58Check(VER_ACCOUNT_ID, currency.isoCI().issuer);

    stringstream sql;
    sql << "SELECT tlimit,balance,authorized from TrustLines where \
        accountID='" << accStr << "' and issuer='" << issuerStr << "' and isoCurrency='" << currencyStr.c_str() << "';"; 

    retLine.mEntry.type(TRUSTLINE);
    retLine.mEntry.trustLine().accountID = accountID;
    int authInt;
    uint64_t tlimit, balance;

    mSession << sql.str() , into(tlimit),  into(balance), into(authInt);

    if(!mSession.got_data())
        return false;
 
    retLine.mEntry.trustLine().limit = tlimit;
    retLine.mEntry.trustLine().balance = balance;
    retLine.mEntry.trustLine().authorized = authInt;
    retLine.mEntry.trustLine().accountID = accountID;
    retLine.mEntry.trustLine().currency = currency;

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
    retOffer.mEntry.offer().sequence = row.get<uint32_t>(2);
    if(row.get_indicator(3))
    {
        retOffer.mEntry.offer().takerPays.type(ISO4217);
        strToCurrencyCode(retOffer.mEntry.offer().takerPays.isoCI().currencyCode,row.get<std::string>(3));
        retOffer.mEntry.offer().takerPays.isoCI().issuer = fromBase58Check256(VER_ACCOUNT_ID, row.get<std::string>(4));
    } else
    {
        retOffer.mEntry.offer().takerPays.type(NATIVE);
    }
    if(row.get_indicator(5))
    {
        retOffer.mEntry.offer().takerGets.type(ISO4217);
        strToCurrencyCode(retOffer.mEntry.offer().takerGets.isoCI().currencyCode,row.get<std::string>(5));
        retOffer.mEntry.offer().takerGets.isoCI().issuer = fromBase58Check256(VER_ACCOUNT_ID, row.get<std::string>(6));
    } else
    {
        retOffer.mEntry.offer().takerGets.type(NATIVE);
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
    retLine.mEntry.trustLine().currency.type(ISO4217);
    retLine.mEntry.trustLine().currency.isoCI().issuer = fromBase58Check256(VER_ACCOUNT_ID, row.get<std::string>(2));
    strToCurrencyCode(retLine.mEntry.trustLine().currency.isoCI().currencyCode,row.get<std::string>(3));
    retLine.mEntry.trustLine().limit = row.get<uint64_t>(4);
    retLine.mEntry.trustLine().balance = row.get<uint64_t>(5);
    retLine.mEntry.trustLine().authorized = row.get<uint32_t>(6);
}

void Database::loadBestOffers(int numOffers, int offset, Currency& pays,
    Currency& gets, vector<OfferFrame>& retOffers)
{
    stringstream sql;
    sql << "SELECT * from Offers where ";
    if(pays.type()==NATIVE)
    {
        std::string b58Issuer,code;
        b58Issuer=toBase58Check(VER_ACCOUNT_ID, gets.isoCI().issuer);
        currencyCodeToStr(gets.isoCI().currencyCode, code);
        sql << "paysIssuer is NULL and getsCurrency='" << code << "' and getsIssuer='" << b58Issuer << "' ";
    } else if(gets.type()==NATIVE)
    {
        std::string currencyCode, b58Issuer;
        currencyCodeToStr(pays.isoCI().currencyCode,currencyCode);
        b58Issuer = toBase58Check(VER_ACCOUNT_ID, pays.isoCI().issuer);
        sql << "getsIssuer is NULL and paysCurrency='" << currencyCode << "' and paysIssuer='" << b58Issuer << "' ";
    } else
    {
        std::string getCurrencyCode, b58GIssuer;
        std::string payCurrencyCode, b58PIssuer;
        currencyCodeToStr(gets.isoCI().currencyCode,getCurrencyCode);
        b58GIssuer = toBase58Check(VER_ACCOUNT_ID, gets.isoCI().issuer);
        sql << "getsCurrency='" << getCurrencyCode << "' and getsIssuer='" << b58GIssuer << "' ";
        
        currencyCodeToStr(pays.isoCI().currencyCode,payCurrencyCode);
        b58PIssuer = toBase58Check(VER_ACCOUNT_ID, pays.isoCI().issuer);
        sql << "paysCurrency='" << payCurrencyCode << "' and paysIssuer='" << b58PIssuer << "' ";
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
    if(currency.type()==NATIVE)
    {
        AccountFrame account;
        if(loadAccount(accountID, account))
        {
            amountFunded = account.mEntry.account().balance;
        }
    } else
    {
        TrustFrame trustLine;
        if(loadTrustLine(accountID, currency, trustLine))
        {
            if(trustLine.mEntry.trustLine().authorized)
                amountFunded = trustLine.mEntry.trustLine().balance;
        }
    }

    return amountFunded;
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

}
