// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/OfferFrame.h"
#include "transactions/ManageOfferOpFrame.h"
#include "database/Database.h"
#include "crypto/SecretKey.h"
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
    "accountid       VARCHAR(56)  NOT NULL,"
    "offerid         BIGINT       NOT NULL CHECK (offerid >= 0),"
    "paysalphanumcurrency VARCHAR(4),"
    "paysissuer      VARCHAR(56),"
    "getsalphanumcurrency VARCHAR(4),"
    "getsissuer      VARCHAR(56),"
    "amount          BIGINT       NOT NULL CHECK (amount >= 0),"
    "pricen          INT          NOT NULL,"
    "priced          INT          NOT NULL,"
    "price           BIGINT       NOT NULL,"
    "flags           INT          NOT NULL,"
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
OfferFrame::from(AccountID const& account, ManageOfferOp const& op)
{
    OfferFrame::pointer res = make_shared<OfferFrame>();
    OfferEntry& o = res->mEntry.offer();
    o.accountID = account;
    o.amount = op.amount;
    o.price = op.price;
    o.offerID = op.offerID;
    o.takerGets = op.takerGets;
    o.takerPays = op.takerPays;
    o.flags = 0;
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

uint32
OfferFrame::getFlags() const
{
    return mOffer.flags;
}

static const char* offerColumnSelector =
    "SELECT accountid,offerid,paysalphanumcurrency,paysissuer,"
    "getsalphanumcurrency,getsissuer,amount,pricen,priced,flags FROM offers";

OfferFrame::pointer
OfferFrame::loadOffer(AccountID const& accountID, uint64_t offerID,
                      Database& db)
{
    OfferFrame::pointer retOffer;

    std::string actIDStrKey;
    actIDStrKey = PubKeyUtils::toStrKey(accountID);

    soci::session& session = db.getSession();

    soci::details::prepare_temp_type sql =
        (session.prepare << offerColumnSelector
                         << " where accountid=:id and offerid=:offerid",
         use(actIDStrKey), use(offerID));

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
    string actIDStrKey;
    std::string paysAlphaNumCurrency, getsAlphaNumCurrency, paysIssuerStrKey,
        getsIssuerStrKey;

    soci::indicator paysAlphaNumIndicator, getsAlphaNumIndicator,
        paysIssuerIndicator, getsIssuerIndicator;

    LedgerEntry le;
    le.type(OFFER);
    OfferEntry& oe = le.offer();

    statement st =
        (prep, into(actIDStrKey), into(oe.offerID),
         into(paysAlphaNumCurrency, paysAlphaNumIndicator),
         into(paysIssuerStrKey, paysIssuerIndicator),
         into(getsAlphaNumCurrency, getsAlphaNumIndicator),
         into(getsIssuerStrKey, getsIssuerIndicator), into(oe.amount),
         into(oe.price.n), into(oe.price.d), into(oe.flags));

    st.execute(true);
    while (st.got_data())
    {
        oe.accountID = PubKeyUtils::fromStrKey(actIDStrKey);
        if (paysAlphaNumIndicator == soci::i_ok)
        {
            if (paysIssuerIndicator != soci::i_ok)
            {
                throw std::runtime_error("bad database state");
            }
            oe.takerPays.type(CURRENCY_TYPE_ALPHANUM);
            strToCurrencyCode(oe.takerPays.alphaNum().currencyCode,
                              paysAlphaNumCurrency);
            oe.takerPays.alphaNum().issuer =
                PubKeyUtils::fromStrKey(paysIssuerStrKey);
        }
        else
        {
            oe.takerPays.type(CURRENCY_TYPE_NATIVE);
        }
        if (getsAlphaNumIndicator == soci::i_ok)
        {
            if (getsIssuerIndicator != soci::i_ok)
            {
                throw std::runtime_error("bad database state");
            }
            oe.takerGets.type(CURRENCY_TYPE_ALPHANUM);
            strToCurrencyCode(oe.takerGets.alphaNum().currencyCode,
                              getsAlphaNumCurrency);
            oe.takerGets.alphaNum().issuer =
                PubKeyUtils::fromStrKey(getsIssuerStrKey);
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

    std::string getCurrencyCode, getsIssuerStrKey;
    std::string payCurrencyCode, paysIssuerStrKey;

    if (pays.type() == CURRENCY_TYPE_NATIVE)
    {
        sql << " WHERE paysissuer IS NULL";
    }
    else
    {
        currencyCodeToStr(pays.alphaNum().currencyCode, payCurrencyCode);
        paysIssuerStrKey = PubKeyUtils::toStrKey(pays.alphaNum().issuer);
        sql << " WHERE paysalphanumcurrency=:pcur AND paysissuer = :pi",
            use(payCurrencyCode), use(paysIssuerStrKey);
    }

    if (gets.type() == CURRENCY_TYPE_NATIVE)
    {
        sql << " AND getsissuer IS NULL";
    }
    else
    {
        currencyCodeToStr(gets.alphaNum().currencyCode, getCurrencyCode);
        getsIssuerStrKey = PubKeyUtils::toStrKey(gets.alphaNum().issuer);

        sql << " AND getsalphanumcurrency=:gcur AND getsissuer = :gi",
            use(getCurrencyCode), use(getsIssuerStrKey);
    }
    sql << " ORDER BY price,offerid LIMIT :n OFFSET :o", use(numOffers),
        use(offset);

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

    std::string actIDStrKey;
    actIDStrKey = PubKeyUtils::toStrKey(accountID);

    soci::details::prepare_temp_type sql =
        (session.prepare << offerColumnSelector << " WHERE accountid=:id",
         use(actIDStrKey));

    auto timer = db.getSelectTimer("offer");
    loadOffers(sql, [&retOffers](LedgerEntry const& of)
               {
                   retOffers.emplace_back(make_shared<OfferFrame>(of));
               });
}

bool
OfferFrame::exists(Database& db, LedgerKey const& key)
{
    std::string actIDStrKey = PubKeyUtils::toStrKey(key.offer().accountID);
    int exists = 0;
    auto timer = db.getSelectTimer("offer-exists");
    db.getSession() << "SELECT EXISTS (SELECT NULL FROM offers "
                       "WHERE accountid=:id AND offerid=:s)",
        use(actIDStrKey), use(key.offer().offerID), into(exists);
    return exists != 0;
}

uint64_t
OfferFrame::countObjects(soci::session& sess)
{
    uint64_t count = 0;
    sess << "SELECT COUNT(*) FROM offers;", into(count);
    return count;
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
    std::string actIDStrKey = PubKeyUtils::toStrKey(mOffer.accountID);

    soci::statement st(db.getSession().prepare << "select 1");

    auto timer = db.getInsertTimer("offer");

    if (mOffer.takerGets.type() == CURRENCY_TYPE_NATIVE)
    {
        std::string issuerStrKey =
            PubKeyUtils::toStrKey(mOffer.takerPays.alphaNum().issuer);
        std::string currencyCode;
        currencyCodeToStr(mOffer.takerPays.alphaNum().currencyCode,
                          currencyCode);
        st = (db.getSession().prepare
                  << "INSERT INTO offers "
                     "(accountid,offerid,paysalphanumcurrency,paysissuer,"
                     "amount,pricen,priced,price,flags) VALUES"
                     "(:v1,:v2,:v3,:v4,:v5,:v6,:v7,:v8,:v9)",
              use(actIDStrKey), use(mOffer.offerID), use(currencyCode),
              use(issuerStrKey), use(mOffer.amount), use(mOffer.price.n),
              use(mOffer.price.d), use(computePrice()), use(mOffer.flags));
        st.execute(true);
    }
    else if (mOffer.takerPays.type() == CURRENCY_TYPE_NATIVE)
    {
        std::string issuerStrKey =
            PubKeyUtils::toStrKey(mOffer.takerGets.alphaNum().issuer);
        std::string currencyCode;
        currencyCodeToStr(mOffer.takerGets.alphaNum().currencyCode,
                          currencyCode);
        st = (db.getSession().prepare
                  << "INSERT INTO offers "
                     "(accountid,offerid,getsalphanumcurrency,getsissuer,"
                     "amount,pricen,priced,price,flags) VALUES"
                     "(:v1,:v2,:v3,:v4,:v5,:v6,:v7,:v8,:v9)",
              use(actIDStrKey), use(mOffer.offerID), use(currencyCode),
              use(issuerStrKey), use(mOffer.amount), use(mOffer.price.n),
              use(mOffer.price.d), use(computePrice()), use(mOffer.flags));
        st.execute(true);
    }
    else
    {
        std::string paysIssuerStrKey =
            PubKeyUtils::toStrKey(mOffer.takerPays.alphaNum().issuer);
        std::string paysAlphaNumCurrency, getsAlphaNumCurrency;
        currencyCodeToStr(mOffer.takerPays.alphaNum().currencyCode,
                          paysAlphaNumCurrency);
        std::string getsIssuerStrKey =
            PubKeyUtils::toStrKey(mOffer.takerGets.alphaNum().issuer);
        currencyCodeToStr(mOffer.takerGets.alphaNum().currencyCode,
                          getsAlphaNumCurrency);
        st = (db.getSession().prepare
                  << "INSERT INTO offers (accountid,offerid,"
                     "paysalphanumcurrency,paysissuer,getsalphanumcurrency,"
                     "getsissuer,"
                     "amount,pricen,priced,price,flags) VALUES "
                     "(:v1,:v2,:v3,:v4,:v5,:v6,:v7,:v8,:v9,:v10,:v11)",
              use(actIDStrKey), use(mOffer.offerID), use(paysAlphaNumCurrency),
              use(paysIssuerStrKey), use(getsAlphaNumCurrency),
              use(getsIssuerStrKey), use(mOffer.amount), use(mOffer.price.n),
              use(mOffer.price.d), use(computePrice()), use(mOffer.flags));
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
