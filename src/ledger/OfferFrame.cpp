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
const char* OfferFrame::kSQLCreateStatement1 =
    "CREATE TABLE offers"
    "("
    "accountid       VARCHAR(51)  NOT NULL,"
    "offerid         BIGINT       NOT NULL CHECK (offerid >= 0),"
    "paysalphanumcurrency VARCHAR(4)   NOT NULL,"
    "paysissuer      VARCHAR(51)  NOT NULL,"
    "getsalphanumcurrency VARCHAR(4)   NOT NULL,"
    "getsissuer      VARCHAR(51)  NOT NULL,"
    "amount          BIGINT       NOT NULL CHECK (amount >= 0),"
    "pricen          INT          NOT NULL,"
    "priced          INT          NOT NULL,"
    "price           BIGINT       NOT NULL,"
    "PRIMARY KEY (offerid)"
    ");";

const char* OfferFrame::kSQLCreateStatement2 =
    "CREATE INDEX paysissuerindex ON offers (paysissuer);";

const char* OfferFrame::kSQLCreateStatement3 =
    "CREATE INDEX getsissuerindex ON offers (getsissuer);";

const char* OfferFrame::kSQLCreateStatement4 =
    "CREATE INDEX priceindex ON offers (price);";

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

OfferFrame::pointer
OfferFrame::from(OperationFrame const& op)
{
    OfferFrame::pointer res = make_shared<OfferFrame>();
    OfferEntry& o = res->mEntry.offer();
    o.accountID = op.getSourceID();
    CreateOfferOp const& create = op.getOperation().body.createOfferOp();
    o.amount = create.amount;
    o.price = create.price;
    o.offerID = create.offerID;
    o.takerGets = create.takerGets;
    o.takerPays = create.takerPays;
    return res;
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
    "SELECT accountid,offerid,paysalphanumcurrency,paysissuer,"
    "getsalphanumcurrency,getsissuer,amount,pricen,priced FROM offers";

OfferFrame::pointer
OfferFrame::loadOffer(AccountID const& accountID, uint64_t offerID,
                      Database& db)
{
    OfferFrame::pointer retOffer;

    std::string accStr;
    accStr = toBase58Check(VER_ACCOUNT_ID, accountID);

    soci::session& session = db.getSession();

    soci::details::prepare_temp_type sql =
        (session.prepare << offerColumnSelector
                         << " where accountid=:id and offerid=:offerid",
         use(accStr), use(offerID));

    auto timer = db.getSelectTimer("offer");
    loadOffers(sql, [&retOffer](LedgerEntry const& offer)
               {
                   retOffer = make_shared<OfferFrame>(offer);
               });

    return retOffer;
}

void
OfferFrame::loadOffers(soci::details::prepare_temp_type& prep,
                       std::function<void(LedgerEntry const&)> offerProcessor)
{
    string accountID;
    std::string paysAlphaNumCurrency, getsAlphaNumCurrency, paysIssuer,
        getsIssuer;

    soci::indicator paysAlphaNumIndicator, getsAlphaNumIndicator;

    LedgerEntry le;
    le.type(OFFER);
    OfferEntry& oe = le.offer();

    statement st =
        (prep, into(accountID), into(oe.offerID),
         into(paysAlphaNumCurrency, paysAlphaNumIndicator), into(paysIssuer),
         into(getsAlphaNumCurrency, getsAlphaNumIndicator), into(getsIssuer),
         into(oe.amount), into(oe.price.n), into(oe.price.d));

    st.execute(true);
    while (st.got_data())
    {
        oe.accountID = fromBase58Check256(VER_ACCOUNT_ID, accountID);
        if (paysAlphaNumIndicator == soci::i_ok)
        {
            oe.takerPays.type(CURRENCY_TYPE_ALPHANUM);
            strToCurrencyCode(oe.takerPays.alphaNum().currencyCode,
                              paysAlphaNumCurrency);
            oe.takerPays.alphaNum().issuer =
                fromBase58Check256(VER_ACCOUNT_ID, paysIssuer);
        }
        else
        {
            oe.takerPays.type(CURRENCY_TYPE_NATIVE);
        }
        if (getsAlphaNumIndicator == soci::i_ok)
        {
            oe.takerGets.type(CURRENCY_TYPE_ALPHANUM);
            strToCurrencyCode(oe.takerGets.alphaNum().currencyCode,
                              getsAlphaNumCurrency);
            oe.takerGets.alphaNum().issuer =
                fromBase58Check256(VER_ACCOUNT_ID, getsIssuer);
        }
        else
        {
            oe.takerGets.type(CURRENCY_TYPE_NATIVE);
        }
        offerProcessor(le);
        st.fetch();
    }
}

void
OfferFrame::loadBestOffers(size_t numOffers, size_t offset,
                           Currency const& pays, Currency const& gets,
                           vector<OfferFrame::pointer>& retOffers, Database& db)
{
    soci::session& session = db.getSession();

    soci::details::prepare_temp_type sql =
        (session.prepare << offerColumnSelector);

    std::string getCurrencyCode, b58GIssuer;
    std::string payCurrencyCode, b58PIssuer;

    if (pays.type() == CURRENCY_TYPE_NATIVE)
    {
        sql << " WHERE paysissuer IS NULL";
    }
    else
    {
        currencyCodeToStr(pays.alphaNum().currencyCode, payCurrencyCode);
        b58PIssuer = toBase58Check(VER_ACCOUNT_ID, pays.alphaNum().issuer);
        sql << " WHERE paysalphanumcurrency=:pcur AND paysissuer = :pi",
            use(payCurrencyCode), use(b58PIssuer);
    }

    if (gets.type() == CURRENCY_TYPE_NATIVE)
    {
        sql << " AND getsissuer IS NULL";
    }
    else
    {
        currencyCodeToStr(gets.alphaNum().currencyCode, getCurrencyCode);
        b58GIssuer = toBase58Check(VER_ACCOUNT_ID, gets.alphaNum().issuer);

        sql << " AND getsalphanumcurrency=:gcur AND getsissuer = :gi",
            use(getCurrencyCode), use(b58GIssuer);
    }
    sql << " ORDER BY price,offerid,accountid LIMIT :n OFFSET :o",
        use(numOffers), use(offset);

    auto timer = db.getSelectTimer("offer");
    loadOffers(sql, [&retOffers](LedgerEntry const& of)
               {
                   retOffers.emplace_back(make_shared<OfferFrame>(of));
               });
}

void
OfferFrame::loadOffers(AccountID const& accountID,
                       std::vector<OfferFrame::pointer>& retOffers,
                       Database& db)
{
    soci::session& session = db.getSession();

    std::string accStr;
    accStr = toBase58Check(VER_ACCOUNT_ID, accountID);

    soci::details::prepare_temp_type sql =
        (session.prepare << offerColumnSelector << " WHERE accountid=:id",
         use(accStr));

    auto timer = db.getSelectTimer("offer");
    loadOffers(sql, [&retOffers](LedgerEntry const& of)
               {
                   retOffers.emplace_back(make_shared<OfferFrame>(of));
               });
}

bool
OfferFrame::exists(Database& db, LedgerKey const& key)
{
    std::string b58AccountID =
        toBase58Check(VER_ACCOUNT_ID, key.offer().accountID);
    int exists = 0;
    auto timer = db.getSelectTimer("offer-exists");
    db.getSession() << "SELECT EXISTS (SELECT NULL FROM offers "
                       "WHERE accountid=:id AND offerid=:s)",
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

    db.getSession() << "DELETE FROM offers WHERE offerid=:s",
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
        (db.getSession().prepare << "UPDATE offers SET amount=:a, pricen=:n, "
                                    "priced=:D, price=:p WHERE offerid=:s",
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

    if (mOffer.takerGets.type() == CURRENCY_TYPE_NATIVE)
    {
        std::string b58issuer =
            toBase58Check(VER_ACCOUNT_ID, mOffer.takerPays.alphaNum().issuer);
        std::string currencyCode;
        currencyCodeToStr(mOffer.takerPays.alphaNum().currencyCode,
                          currencyCode);
        st = (db.getSession().prepare
                  << "INSERT INTO offers "
                     "(accountid,offerid,paysalphanumcurrency,paysissuer,"
                     "amount,pricen,priced,price) VALUES"
                     "(:v1,:v2,:v3,:v4,:v5,:v6,:v7,:v8)",
              use(b58AccountID), use(mOffer.offerID), use(b58issuer),
              use(currencyCode), use(mOffer.amount), use(mOffer.price.n),
              use(mOffer.price.d), use(computePrice()));
        st.execute(true);
    }
    else if (mOffer.takerPays.type() == CURRENCY_TYPE_NATIVE)
    {
        std::string b58issuer =
            toBase58Check(VER_ACCOUNT_ID, mOffer.takerGets.alphaNum().issuer);
        std::string currencyCode;
        currencyCodeToStr(mOffer.takerGets.alphaNum().currencyCode,
                          currencyCode);
        st = (db.getSession().prepare
                  << "INSERT INTO offers "
                     "(accountid,offerid,getsalphanumcurrency,getsissuer,"
                     "amount,pricen,priced,price) VALUES"
                     "(:v1,:v2,:v3,:v4,:v5,:v6,:v7,:v8)",
              use(b58AccountID), use(mOffer.offerID), use(b58issuer),
              use(currencyCode), use(mOffer.amount), use(mOffer.price.n),
              use(mOffer.price.d), use(computePrice()));
        st.execute(true);
    }
    else
    {
        std::string b58PaysIssuer =
            toBase58Check(VER_ACCOUNT_ID, mOffer.takerPays.alphaNum().issuer);
        std::string paysAlphaNumCurrency, getsAlphaNumCurrency;
        currencyCodeToStr(mOffer.takerPays.alphaNum().currencyCode,
                          paysAlphaNumCurrency);
        std::string b58GetsIssuer =
            toBase58Check(VER_ACCOUNT_ID, mOffer.takerGets.alphaNum().issuer);
        currencyCodeToStr(mOffer.takerGets.alphaNum().currencyCode,
                          getsAlphaNumCurrency);
        st = (db.getSession().prepare
                  << "INSERT INTO offers (accountid,offerid,"
                     "paysalphanumcurrency,paysissuer,getsalphanumcurrency,"
                     "getsissuer,"
                     "amount,pricen,priced,price) VALUES "
                     "(:v1,:v2,:v3,:v4,:v5,:v6,:v7,:v8,:v9,:v10)",
              use(b58AccountID), use(mOffer.offerID), use(paysAlphaNumCurrency),
              use(b58PaysIssuer), use(getsAlphaNumCurrency), use(b58GetsIssuer),
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
    db.getSession() << "DROP TABLE IF EXISTS offers;";
    db.getSession() << kSQLCreateStatement1;
    db.getSession() << kSQLCreateStatement2;
    db.getSession() << kSQLCreateStatement3;
    db.getSession() << kSQLCreateStatement4;
}
}
