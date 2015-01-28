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

void Database::initialize()
{
    try {
        AccountFrame::dropAll(*this);
        OfferFrame::dropAll(*this);
        TrustFrame::dropAll(*this);
        PeerMaster::createTable(*this);
        LedgerMaster::dropAll(*this);
        LedgerHeaderFrame::dropAll(*this);
        TransactionFrame::dropAll(*this);
    }catch(exception const &e)
    {
        LOG(ERROR) << "Error: " << e.what();
    }
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

// TODO: move this and related SQL code to OfferFrame
static const char *offerColumnSelector = "SELECT accountID,sequence,paysIsoCurrency,paysIssuer,getsIsoCurrency,getsIssuer,amount,price,flags FROM Offers";

bool Database::loadOffer(const uint256& accountID, uint32_t seq, OfferFrame& retOffer)
{
    std::string accStr;
    accStr = toBase58Check(VER_ACCOUNT_ID, accountID);
 
    soci::details::prepare_temp_type sql = (mSession.prepare <<
    offerColumnSelector << " where accountID=:id and sequence=:seq",
    use(accStr), use(seq));
    
    bool res = false;

    loadOffers(sql, [&retOffer, &res](OfferFrame const& offer) {
        retOffer = offer;
        res = true;
    });
    
    return res;
}


void Database::loadOffers(soci::details::prepare_temp_type &prep, std::function<void(OfferFrame const&)> offerProcessor)
{
    // std::string const &sql
    string accountID;
    std::string paysIsoCurrency, getsIsoCurrency, paysIssuer, getsIssuer;
    
    soci::indicator paysIsoIndicator, getsIsoIndicator;

    OfferFrame offerFrame;

    OfferEntry &oe = offerFrame.mEntry.offer();

    statement st = (prep,
        into(accountID), into(oe.sequence),
        into(paysIsoCurrency, paysIsoIndicator), into(paysIssuer),
        into(getsIsoCurrency, getsIsoIndicator), into(getsIssuer),
        into(oe.amount), into(oe.price), into(oe.flags)
        );

    st.execute(true);
    while (st.got_data())
    {
        oe.accountID = fromBase58Check256(VER_ACCOUNT_ID, accountID);
        if (paysIsoIndicator == soci::i_ok)
        {
            oe.takerPays.type(ISO4217);
            strToCurrencyCode(oe.takerPays.isoCI().currencyCode, paysIsoCurrency);
            oe.takerPays.isoCI().issuer = fromBase58Check256(VER_ACCOUNT_ID, paysIssuer);
        }
        else
        {
            oe.takerPays.type(NATIVE);
        }
        if (getsIsoIndicator == soci::i_ok)
        {
            oe.takerGets.type(ISO4217);
            strToCurrencyCode(oe.takerGets.isoCI().currencyCode, getsIsoCurrency);
            oe.takerGets.isoCI().issuer = fromBase58Check256(VER_ACCOUNT_ID, getsIssuer);
        }
        else
        {
            oe.takerGets.type(NATIVE);
        }
        offerProcessor(offerFrame);
        st.fetch();
    }
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

void Database::loadBestOffers(size_t numOffers, size_t offset, const Currency & pays,
    const Currency & gets, vector<OfferFrame>& retOffers)
{
    soci::details::prepare_temp_type sql = (mSession.prepare <<
        offerColumnSelector );

    std::string getCurrencyCode, b58GIssuer;
    std::string payCurrencyCode, b58PIssuer;

    if(pays.type()==NATIVE)
    {
        sql << " WHERE paysIssuer IS NULL";
    }
    else
    {
        currencyCodeToStr(pays.isoCI().currencyCode, payCurrencyCode);
        b58PIssuer = toBase58Check(VER_ACCOUNT_ID, pays.isoCI().issuer);
        sql << " WHERE paysIsoCurrency=:pcur AND paysIssuer = :pi", use(payCurrencyCode), use(b58PIssuer);
    }
    
    if(gets.type()==NATIVE)
    {
        sql << " AND getsIssuer IS NULL";
    }
    else
    {
        currencyCodeToStr(gets.isoCI().currencyCode, getCurrencyCode);
        b58GIssuer = toBase58Check(VER_ACCOUNT_ID, gets.isoCI().issuer);

        sql << " AND getsIsoCurrency=:gcur AND getsIssuer = :gi", use(getCurrencyCode), use(b58GIssuer);
    }
    sql << " order by price,sequence,accountID limit :o,:n", use(offset), use(numOffers);

    loadOffers(sql, [&retOffers](OfferFrame const &of)
    {
        retOffers.push_back(of);
    });
}

void Database::loadOffers(const uint256& accountID, std::vector<OfferFrame>& retOffers)
{
    std::string accStr;
    accStr = toBase58Check(VER_ACCOUNT_ID, accountID);

    soci::details::prepare_temp_type sql = (mSession.prepare <<
        offerColumnSelector << " WHERE accountID=:id", use(accStr));

    loadOffers(sql, [&retOffers](OfferFrame const &of)
    {
        retOffers.push_back(of);
    });
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
