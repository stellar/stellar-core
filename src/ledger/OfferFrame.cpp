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
    "sellerid       VARCHAR(56)  NOT NULL,"
    "offerid         BIGINT       NOT NULL CHECK (offerid >= 0),"
    "sellingassettype    INT,"
    "sellingassetcode    VARCHAR(12),"
    "sellingissuer       VARCHAR(56),"
    "buyingassettype    INT,"
    "buyingassetcode    VARCHAR(12),"
    "buyingissuer       VARCHAR(56),"
    "amount          BIGINT       NOT NULL CHECK (amount >= 0),"
    "pricen          INT          NOT NULL,"
    "priced          INT          NOT NULL,"
    "price           BIGINT       NOT NULL,"
    "flags           INT          NOT NULL,"
    "PRIMARY KEY (offerid)"
    ");";

const char* OfferFrame::kSQLCreateStatement2 =
    "CREATE INDEX sellingissuerindex ON offers (sellingissuer);";

const char* OfferFrame::kSQLCreateStatement3 =
    "CREATE INDEX buyingissuerindex ON offers (buyingissuer);";

const char* OfferFrame::kSQLCreateStatement4 =
    "CREATE INDEX priceindex ON offers (price);";

static const char* offerColumnSelector =
    "SELECT sellerid,offerid,sellingassettype,sellingassetcode,sellingissuer,"
    "buyingassettype,buyingassetcode,buyingissuer,amount,pricen,priced,flags "
    "FROM offers";

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
    o.sellerID = account;
    o.amount = op.amount;
    o.price = op.price;
    o.offerID = op.offerID;
    o.selling = op.selling;
    o.buying = op.buying;
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
OfferFrame::getSellerID() const
{
    return mOffer.sellerID;
}

Asset const&
OfferFrame::getBuying() const
{
    return mOffer.buying;
}
Asset const&
OfferFrame::getSelling() const
{
    return mOffer.selling;
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

OfferFrame::pointer
OfferFrame::loadOffer(AccountID const& sellerID, uint64_t offerID, Database& db)
{
    OfferFrame::pointer retOffer;

    std::string actIDStrKey = PubKeyUtils::toStrKey(sellerID);

    std::string sql = offerColumnSelector;
    sql += " WHERE sellerid = :id AND offerid = :offerid";
    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(use(actIDStrKey));
    st.exchange(use(offerID));

    auto timer = db.getSelectTimer("offer");
    loadOffers(prep, [&retOffer](LedgerEntry const& offer)
               {
                   retOffer = make_shared<OfferFrame>(offer);
               });

    return retOffer;
}

void
OfferFrame::loadOffers(StatementContext& prep,
                       std::function<void(LedgerEntry const&)> offerProcessor)
{
    string actIDStrKey;
    unsigned int sellingAssetType, buyingAssetType;
    std::string sellingAssetCode, buyingAssetCode, sellingIssuerStrKey,
        buyingIssuerStrKey;

    soci::indicator sellingAssetCodeIndicator, buyingAssetCodeIndicator,
        sellingIssuerIndicator, buyingIssuerIndicator;

    LedgerEntry le;
    le.type(OFFER);
    OfferEntry& oe = le.offer();

    statement& st = prep.statement();
    st.exchange(into(actIDStrKey));
    st.exchange(into(oe.offerID));
    st.exchange(into(sellingAssetType));
    st.exchange(into(sellingAssetCode, sellingAssetCodeIndicator));
    st.exchange(into(sellingIssuerStrKey, sellingIssuerIndicator));
    st.exchange(into(buyingAssetType));
    st.exchange(into(buyingAssetCode, buyingAssetCodeIndicator));
    st.exchange(into(buyingIssuerStrKey, buyingIssuerIndicator));
    st.exchange(into(oe.amount));
    st.exchange(into(oe.price.n));
    st.exchange(into(oe.price.d));
    st.exchange(into(oe.flags));
    st.define_and_bind();
    st.execute(true);
    while (st.got_data())
    {
        oe.sellerID = PubKeyUtils::fromStrKey(actIDStrKey);
        if ((buyingAssetType > ASSET_TYPE_CREDIT_ALPHANUM12) ||
            (sellingAssetType > ASSET_TYPE_CREDIT_ALPHANUM12))
            throw std::runtime_error("bad database state");

        oe.buying.type((AssetType)buyingAssetType);
        oe.selling.type((AssetType)sellingAssetType);
        if (sellingAssetType != ASSET_TYPE_NATIVE)
        {
            if ((sellingAssetCodeIndicator != soci::i_ok) ||
                (sellingIssuerIndicator != soci::i_ok))
            {
                throw std::runtime_error("bad database state");
            }

            if (sellingAssetType == ASSET_TYPE_CREDIT_ALPHANUM12)
            {
                oe.selling.alphaNum12().issuer =
                    PubKeyUtils::fromStrKey(sellingIssuerStrKey);
                strToAssetCode(oe.selling.alphaNum12().assetCode,
                               sellingAssetCode);
            }
            else if (sellingAssetType == ASSET_TYPE_CREDIT_ALPHANUM4)
            {
                oe.selling.alphaNum4().issuer =
                    PubKeyUtils::fromStrKey(sellingIssuerStrKey);
                strToAssetCode(oe.selling.alphaNum4().assetCode,
                               sellingAssetCode);
            }
        }

        if (buyingAssetType != ASSET_TYPE_NATIVE)
        {
            if ((buyingAssetCodeIndicator != soci::i_ok) ||
                (buyingIssuerIndicator != soci::i_ok))
            {
                throw std::runtime_error("bad database state");
            }

            if (buyingAssetType == ASSET_TYPE_CREDIT_ALPHANUM12)
            {
                oe.buying.alphaNum12().issuer =
                    PubKeyUtils::fromStrKey(buyingIssuerStrKey);
                strToAssetCode(oe.buying.alphaNum12().assetCode,
                               buyingAssetCode);
            }
            else if (buyingAssetType == ASSET_TYPE_CREDIT_ALPHANUM4)
            {
                oe.buying.alphaNum4().issuer =
                    PubKeyUtils::fromStrKey(buyingIssuerStrKey);
                strToAssetCode(oe.buying.alphaNum4().assetCode,
                               buyingAssetCode);
            }
        }

        offerProcessor(le);
        st.fetch();
    }
}

void
OfferFrame::loadBestOffers(size_t numOffers, size_t offset,
                           Asset const& selling, Asset const& buying,
                           vector<OfferFrame::pointer>& retOffers, Database& db)
{
    std::string sql = offerColumnSelector;

    std::string sellingAssetCode, sellingIssuerStrKey;
    std::string buyingAssetCode, buyingIssuerStrKey;

    bool useSellingAsset = false;
    bool useBuyingAsset = false;

    if (selling.type() == ASSET_TYPE_NATIVE)
    {
        sql += " WHERE sellingassettype = 0";
    }
    else
    {
        if (selling.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
        {
            assetCodeToStr(selling.alphaNum4().assetCode, sellingAssetCode);
            sellingIssuerStrKey =
                PubKeyUtils::toStrKey(selling.alphaNum4().issuer);
        }
        else if (selling.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
        {
            assetCodeToStr(selling.alphaNum12().assetCode, sellingAssetCode);
            sellingIssuerStrKey =
                PubKeyUtils::toStrKey(selling.alphaNum12().issuer);
        }
        else
        {
            throw std::runtime_error("unknown asset type");
        }

        useSellingAsset = true;
        sql += " WHERE sellingassetcode = :pcur AND sellingissuer = :pi";
    }

    if (buying.type() == ASSET_TYPE_NATIVE)
    {
        sql += " AND buyingassettype = 0";
    }
    else
    {
        if (buying.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
        {
            assetCodeToStr(buying.alphaNum4().assetCode, buyingAssetCode);
            buyingIssuerStrKey =
                PubKeyUtils::toStrKey(buying.alphaNum4().issuer);
        }
        else if (buying.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
        {
            assetCodeToStr(buying.alphaNum12().assetCode, buyingAssetCode);
            buyingIssuerStrKey =
                PubKeyUtils::toStrKey(buying.alphaNum12().issuer);
        }
        else
        {
            throw std::runtime_error("unknown asset type");
        }

        useBuyingAsset = true;
        sql += " AND buyingassetcode = :gcur AND buyingissuer = :gi";
    }

    sql += " ORDER BY price, offerid LIMIT :n OFFSET :o";

    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();

    if (useSellingAsset)
    {
        st.exchange(use(sellingAssetCode));
        st.exchange(use(sellingIssuerStrKey));
    }

    if (useBuyingAsset)
    {
        st.exchange(use(buyingAssetCode));
        st.exchange(use(buyingIssuerStrKey));
    }

    st.exchange(use(numOffers));
    st.exchange(use(offset));

    auto timer = db.getSelectTimer("offer");
    loadOffers(prep, [&retOffers](LedgerEntry const& of)
               {
                   retOffers.emplace_back(make_shared<OfferFrame>(of));
               });
}

void
OfferFrame::loadOffers(AccountID const& accountID,
                       std::vector<OfferFrame::pointer>& retOffers,
                       Database& db)
{
    std::string actIDStrKey;
    actIDStrKey = PubKeyUtils::toStrKey(accountID);

    std::string sql = offerColumnSelector;
    sql += " WHERE sellerid = :id";
    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(use(actIDStrKey));

    auto timer = db.getSelectTimer("offer");
    loadOffers(prep, [&retOffers](LedgerEntry const& of)
               {
                   retOffers.emplace_back(make_shared<OfferFrame>(of));
               });
}

bool
OfferFrame::exists(Database& db, LedgerKey const& key)
{
    std::string actIDStrKey = PubKeyUtils::toStrKey(key.offer().sellerID);
    int exists = 0;
    auto timer = db.getSelectTimer("offer-exists");
    auto prep =
        db.getPreparedStatement("SELECT EXISTS (SELECT NULL FROM offers "
                                "WHERE sellerid=:id AND offerid=:s)");
    auto& st = prep.statement();
    st.exchange(use(actIDStrKey));
    st.exchange(use(key.offer().offerID));
    st.exchange(into(exists));
    st.define_and_bind();
    st.execute(true);
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
    auto prep = db.getPreparedStatement("DELETE FROM offers WHERE offerid=:s");
    auto& st = prep.statement();
    st.exchange(use(key.offer().offerID));
    st.define_and_bind();
    st.execute(true);
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
    auto prep =
        db.getPreparedStatement("UPDATE offers SET amount=:a, pricen=:n, "
                                "priced=:D, price=:p WHERE offerid=:s");
    auto& st = prep.statement();
    st.exchange(use(mOffer.amount));
    st.exchange(use(mOffer.price.n));
    st.exchange(use(mOffer.price.d));
    auto price = computePrice();
    st.exchange(use(price));
    st.exchange(use(mOffer.offerID));
    st.define_and_bind();
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
    std::string actIDStrKey = PubKeyUtils::toStrKey(mOffer.sellerID);
    auto timer = db.getInsertTimer("offer");

    unsigned int sellingType = mOffer.selling.type();
    unsigned int buyingType = mOffer.buying.type();
    std::string sellingIssuerStrKey, buyingIssuerStrKey;
    std::string sellingAssetCode, buyingAssetCode;

    if (sellingType == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        sellingIssuerStrKey =
            PubKeyUtils::toStrKey(mOffer.selling.alphaNum4().issuer);
        assetCodeToStr(mOffer.selling.alphaNum4().assetCode, sellingAssetCode);
    }
    else if (sellingType == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        sellingIssuerStrKey =
            PubKeyUtils::toStrKey(mOffer.selling.alphaNum12().issuer);
        assetCodeToStr(mOffer.selling.alphaNum12().assetCode, sellingAssetCode);
    }

    if (buyingType == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        buyingIssuerStrKey =
            PubKeyUtils::toStrKey(mOffer.buying.alphaNum4().issuer);
        assetCodeToStr(mOffer.buying.alphaNum4().assetCode, buyingAssetCode);
    }
    else if (buyingType == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        buyingIssuerStrKey =
            PubKeyUtils::toStrKey(mOffer.buying.alphaNum12().issuer);
        assetCodeToStr(mOffer.buying.alphaNum12().assetCode, buyingAssetCode);
    }

    auto prep = db.getPreparedStatement(
        "INSERT INTO offers (sellerid,offerid,"
        "sellingassettype,sellingassetcode,sellingissuer,"
        "buyingassettype,buyingassetcode,buyingissuer,"
        "amount,pricen,priced,price,flags) VALUES "
        "(:v1,:v2,:v3,:v4,:v5,:v6,:v7,:v8,:v9,:v10,:v11,:v12,:v13)");
    auto& st = prep.statement();
    st.exchange(use(actIDStrKey));
    st.exchange(use(mOffer.offerID));
    st.exchange(use(sellingType));
    st.exchange(use(sellingAssetCode));
    st.exchange(use(sellingIssuerStrKey));
    st.exchange(use(buyingType));
    st.exchange(use(buyingAssetCode));
    st.exchange(use(buyingIssuerStrKey));
    st.exchange(use(mOffer.amount));
    st.exchange(use(mOffer.price.n));
    st.exchange(use(mOffer.price.d));
    auto price = computePrice();
    st.exchange(use(price));
    st.exchange(use(mOffer.flags));
    st.define_and_bind();
    st.execute(true);

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
