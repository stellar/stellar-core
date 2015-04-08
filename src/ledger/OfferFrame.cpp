// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/OfferFrame.h"
#include "transactions/OperationFrame.h"
#include "database/Database.h"
#include "crypto/Base58.h"
#include "crypto/SHA.h"
#include "LedgerDelta.h"
#include "util/types.h"

using namespace std;
using namespace soci;

namespace stellar
{
const char* OfferFrame::kSQLCreateStatement =
    "CREATE TABLE Offers"
    "("
    "accountID       VARCHAR(51)  NOT NULL,"
    "offerID         BIGINT       NOT NULL CHECK (offerID >= 0),"
    "paysIsoCurrency VARCHAR(4)   NOT NULL,"
    "paysIssuer      VARCHAR(51)  NOT NULL,"
    "getsIsoCurrency VARCHAR(4)   NOT NULL,"
    "getsIssuer      VARCHAR(51)  NOT NULL,"
    "amount          BIGINT       NOT NULL CHECK (amount >= 0),"
    "priceN          INT          NOT NULL,"
    "priceD          INT          NOT NULL,"
    "price           BIGINT       NOT NULL,"
    "PRIMARY KEY (offerID)"
    ");";

OfferFrame::OfferFrame() : EntryFrame(OFFER), mOffer(mEntry.offer())
{
}

OfferFrame::OfferFrame(LedgerEntry const& from)
    : EntryFrame(from), mOffer(mEntry.offer())
{
}

OfferFrame::OfferFrame(OfferFrame const& from) : OfferFrame(from.mEntry)
{
}

OfferFrame& OfferFrame::operator=(OfferFrame const& other)
{
    if (&other != this)
    {
        mOffer = other.mOffer;
        mKey = other.mKey;
        mKeyCalculated = other.mKeyCalculated;
    }
    return *this;
}

void
OfferFrame::from(OperationFrame const& op)
{
    assert(mEntry.type() == OFFER);
    mOffer.accountID = op.getSourceID();
    CreateOfferOp const& create = op.getOperation().body.createOfferOp();
    mOffer.amount = create.amount;
    mOffer.price = create.price;
    mOffer.offerID = create.offerID;
    mOffer.takerGets = create.takerGets;
    mOffer.takerPays = create.takerPays;
    clearCached();
}

Price const&
OfferFrame::getPrice() const
{
    return mOffer.price;
}

int64_t
OfferFrame::getAmount() const
{
    return mOffer.amount;
}

AccountID const&
OfferFrame::getAccountID() const
{
    return mOffer.accountID;
}

Currency const&
OfferFrame::getTakerPays() const
{
    return mOffer.takerPays;
}
Currency const&
OfferFrame::getTakerGets() const
{
    return mOffer.takerGets;
}

uint64
OfferFrame::getOfferID() const
{
    return mOffer.offerID;
}

static const char* offerColumnSelector =
    "SELECT accountID,offerID,paysIsoCurrency,paysIssuer,"
    "getsIsoCurrency,getsIssuer,amount,priceN,priceD FROM Offers";

bool
OfferFrame::loadOffer(AccountID const& accountID, uint64_t offerID,
                      OfferFrame& retOffer, Database& db)
{
    std::string accStr;
    accStr = toBase58Check(VER_ACCOUNT_ID, accountID);

    soci::session& session = db.getSession();

    soci::details::prepare_temp_type sql =
        (session.prepare << offerColumnSelector
                         << " where accountID=:id and offerID=:offerID",
         use(accStr), use(offerID));

    bool res = false;

    auto timer = db.getSelectTimer("offer");
    retOffer.clearCached();
    loadOffers(sql, [&retOffer, &res](OfferFrame const& offer)
               {
                   retOffer = offer;
                   res = true;
               });

    return res;
}

void
OfferFrame::loadOffers(soci::details::prepare_temp_type& prep,
                       std::function<void(OfferFrame const&)> offerProcessor)
{
    string accountID;
    std::string paysIsoCurrency, getsIsoCurrency, paysIssuer, getsIssuer;

    soci::indicator paysIsoIndicator, getsIsoIndicator;

    OfferFrame offerFrame;

    offerFrame.clearCached();
    OfferEntry& oe = offerFrame.mOffer;

    statement st = (prep, into(accountID), into(oe.offerID),
                    into(paysIsoCurrency, paysIsoIndicator), into(paysIssuer),
                    into(getsIsoCurrency, getsIsoIndicator), into(getsIssuer),
                    into(oe.amount), into(oe.price.n), into(oe.price.d));

    st.execute(true);
    while (st.got_data())
    {
        oe.accountID = fromBase58Check256(VER_ACCOUNT_ID, accountID);
        if (paysIsoIndicator == soci::i_ok)
        {
            oe.takerPays.type(ISO4217);
            strToCurrencyCode(oe.takerPays.isoCI().currencyCode,
                              paysIsoCurrency);
            oe.takerPays.isoCI().issuer =
                fromBase58Check256(VER_ACCOUNT_ID, paysIssuer);
        }
        else
        {
            oe.takerPays.type(NATIVE);
        }
        if (getsIsoIndicator == soci::i_ok)
        {
            oe.takerGets.type(ISO4217);
            strToCurrencyCode(oe.takerGets.isoCI().currencyCode,
                              getsIsoCurrency);
            oe.takerGets.isoCI().issuer =
                fromBase58Check256(VER_ACCOUNT_ID, getsIssuer);
        }
        else
        {
            oe.takerGets.type(NATIVE);
        }
        offerFrame.mKeyCalculated = false;
        offerProcessor(offerFrame);
        st.fetch();
    }
}

void
OfferFrame::loadBestOffers(size_t numOffers, size_t offset,
                           Currency const& pays, Currency const& gets,
                           vector<OfferFrame>& retOffers, Database& db)
{
    soci::session& session = db.getSession();

    soci::details::prepare_temp_type sql =
        (session.prepare << offerColumnSelector);

    std::string getCurrencyCode, b58GIssuer;
    std::string payCurrencyCode, b58PIssuer;

    if (pays.type() == NATIVE)
    {
        sql << " WHERE paysIssuer IS NULL";
    }
    else
    {
        currencyCodeToStr(pays.isoCI().currencyCode, payCurrencyCode);
        b58PIssuer = toBase58Check(VER_ACCOUNT_ID, pays.isoCI().issuer);
        sql << " WHERE paysIsoCurrency=:pcur AND paysIssuer = :pi",
            use(payCurrencyCode), use(b58PIssuer);
    }

    if (gets.type() == NATIVE)
    {
        sql << " AND getsIssuer IS NULL";
    }
    else
    {
        currencyCodeToStr(gets.isoCI().currencyCode, getCurrencyCode);
        b58GIssuer = toBase58Check(VER_ACCOUNT_ID, gets.isoCI().issuer);

        sql << " AND getsIsoCurrency=:gcur AND getsIssuer = :gi",
            use(getCurrencyCode), use(b58GIssuer);
    }
    sql << " ORDER BY price,offerID,accountID LIMIT :n OFFSET :o",
        use(numOffers), use(offset);

    auto timer = db.getSelectTimer("offer");
    loadOffers(sql, [&retOffers](OfferFrame const& of)
               {
                   retOffers.push_back(of);
               });
}

void
OfferFrame::loadOffers(AccountID const& accountID,
                       std::vector<OfferFrame>& retOffers, Database& db)
{
    soci::session& session = db.getSession();

    std::string accStr;
    accStr = toBase58Check(VER_ACCOUNT_ID, accountID);

    soci::details::prepare_temp_type sql =
        (session.prepare << offerColumnSelector << " WHERE accountID=:id",
         use(accStr));

    auto timer = db.getSelectTimer("offer");
    loadOffers(sql, [&retOffers](OfferFrame const& of)
               {
                   retOffers.push_back(of);
               });
}

bool
OfferFrame::exists(Database& db, LedgerKey const& key)
{
    std::string b58AccountID =
        toBase58Check(VER_ACCOUNT_ID, key.offer().accountID);
    int exists = 0;
    auto timer = db.getSelectTimer("offer-exists");
    db.getSession() << "SELECT EXISTS (SELECT NULL FROM Offers \
             WHERE accountID=:id AND offerID=:s)",
        use(b58AccountID), use(key.offer().offerID), into(exists);
    return exists != 0;
}

void
OfferFrame::storeDelete(LedgerDelta& delta, Database& db) const
{
    storeDelete(delta, db, getKey());
}

void
OfferFrame::storeDelete(LedgerDelta& delta, Database& db, LedgerKey const& key)
{
    auto timer = db.getDeleteTimer("offer");

    db.getSession() << "DELETE FROM Offers WHERE offerID=:s",
        use(key.offer().offerID);

    delta.deleteEntry(key);
}

int64_t
OfferFrame::computePrice() const
{
    return bigDivide(mOffer.price.n, OFFER_PRICE_DIVISOR, mOffer.price.d);
}

void
OfferFrame::storeChange(LedgerDelta& delta, Database& db) const
{

    auto timer = db.getUpdateTimer("offer");

    soci::statement st =
        (db.getSession().prepare << "UPDATE Offers SET amount=:a, priceN=:n, "
                                    "priceD=:D, price=:p WHERE offerID=:s",
         use(mOffer.amount), use(mOffer.price.n), use(mOffer.price.d),
         use(computePrice()), use(mOffer.offerID));

    st.execute(true);

    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("could not update SQL");
    }

    delta.modEntry(*this);
}

void
OfferFrame::storeAdd(LedgerDelta& delta, Database& db) const
{
    std::string b58AccountID = toBase58Check(VER_ACCOUNT_ID, mOffer.accountID);

    soci::statement st(db.getSession().prepare << "select 1");

    auto timer = db.getInsertTimer("offer");

    if (mOffer.takerGets.type() == NATIVE)
    {
        std::string b58issuer =
            toBase58Check(VER_ACCOUNT_ID, mOffer.takerPays.isoCI().issuer);
        std::string currencyCode;
        currencyCodeToStr(mOffer.takerPays.isoCI().currencyCode, currencyCode);
        st = (db.getSession().prepare
                  << "INSERT into Offers "
                     "(accountID,offerID,paysIsoCurrency,paysIssuer,"
                     "amount,priceN,priceD,price) values"
                     "(:v1,:v2,:v3,:v4,:v5,:v6,:v7,:v8)",
              use(b58AccountID), use(mOffer.offerID), use(b58issuer),
              use(currencyCode), use(mOffer.amount), use(mOffer.price.n),
              use(mOffer.price.d), use(computePrice()));
        st.execute(true);
    }
    else if (mOffer.takerPays.type() == NATIVE)
    {
        std::string b58issuer =
            toBase58Check(VER_ACCOUNT_ID, mOffer.takerGets.isoCI().issuer);
        std::string currencyCode;
        currencyCodeToStr(mOffer.takerGets.isoCI().currencyCode, currencyCode);
        st = (db.getSession().prepare
                  << "INSERT into Offers "
                     "(accountID,offerID,getsIsoCurrency,getsIssuer,"
                     "amount,priceN,priceD,price) values"
                     "(:v1,:v2,:v3,:v4,:v5,:v6,:v7,:v8)",
              use(b58AccountID), use(mOffer.offerID), use(b58issuer),
              use(currencyCode), use(mOffer.amount), use(mOffer.price.n),
              use(mOffer.price.d), use(computePrice()));
        st.execute(true);
    }
    else
    {
        std::string b58PaysIssuer =
            toBase58Check(VER_ACCOUNT_ID, mOffer.takerPays.isoCI().issuer);
        std::string paysIsoCurrency, getsIsoCurrency;
        currencyCodeToStr(mOffer.takerPays.isoCI().currencyCode,
                          paysIsoCurrency);
        std::string b58GetsIssuer =
            toBase58Check(VER_ACCOUNT_ID, mOffer.takerGets.isoCI().issuer);
        currencyCodeToStr(mOffer.takerGets.isoCI().currencyCode,
                          getsIsoCurrency);
        st = (db.getSession().prepare
                  << "INSERT into Offers (accountID,offerID,"
                     "paysIsoCurrency,paysIssuer,getsIsoCurrency,getsIssuer,"
                     "amount,priceN,priceD,price) values "
                     "(:v1,:v2,:v3,:v4,:v5,:v6,:v7,:v8,:v9,:v10)",
              use(b58AccountID), use(mOffer.offerID), use(paysIsoCurrency),
              use(b58PaysIssuer), use(getsIsoCurrency), use(b58GetsIssuer),
              use(mOffer.amount), use(mOffer.price.n), use(mOffer.price.d),
              use(computePrice()));
        st.execute(true);
    }

    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("could not update SQL");
    }

    delta.addEntry(*this);
}

void
OfferFrame::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS Offers;";
    db.getSession() << kSQLCreateStatement;
}
}
