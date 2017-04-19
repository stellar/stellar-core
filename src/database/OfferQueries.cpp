// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TrustLineQueries.h"
#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "crypto/SignerKey.h"
#include "database/Database.h"
#include "ledger/OfferFrame.h"
#include "util/types.h"

namespace stellar
{

namespace
{

const auto OFFER_COLUMN_SELECTOR =
    "SELECT sellerid,offerid,sellingassettype,sellingassetcode,sellingissuer,"
    "buyingassettype,buyingassetcode,buyingissuer,amount,pricen,priced,"
    "flags,lastmodified "
    "FROM offers";

const auto DROP_OFFERS_TABLE = "DROP TABLE IF EXISTS offers;";

const auto CREATE_OFFERS_TABLE =
    "CREATE TABLE offers"
    "("
    "sellerid         VARCHAR(56)  NOT NULL,"
    "offerid          BIGINT       NOT NULL CHECK (offerid >= 0),"
    "sellingassettype INT          NOT NULL,"
    "sellingassetcode VARCHAR(12),"
    "sellingissuer    VARCHAR(56),"
    "buyingassettype  INT          NOT NULL,"
    "buyingassetcode  VARCHAR(12),"
    "buyingissuer     VARCHAR(56),"
    "amount           BIGINT           NOT NULL CHECK (amount >= 0),"
    "pricen           INT              NOT NULL,"
    "priced           INT              NOT NULL,"
    "price            DOUBLE PRECISION NOT NULL,"
    "flags            INT              NOT NULL,"
    "lastmodified     INT              NOT NULL,"
    "PRIMARY KEY      (offerid)"
    ");";

const auto CREATE_SELLING_ISSUER_INDEX =
    "CREATE INDEX sellingissuerindex ON offers (sellingissuer);";

const auto CREATE_BUYING_ISSUER_INDEX =
    "CREATE INDEX buyingissuerindex ON offers (buyingissuer);";

const auto CREATE_PRICE_INDEX = "CREATE INDEX priceindex ON offers (price);";

std::vector<LedgerEntry>
loadOffers(StatementContext& prep, Database& db)
{
    auto result = std::vector<LedgerEntry>{};

    std::string actIDStrKey;
    unsigned int sellingAssetType, buyingAssetType;
    std::string sellingAssetCode, buyingAssetCode, sellingIssuerStrKey,
        buyingIssuerStrKey;

    soci::indicator sellingAssetCodeIndicator, buyingAssetCodeIndicator,
        sellingIssuerIndicator, buyingIssuerIndicator;

    auto offer = OfferFrame{};
    auto& le = offer.getEntry();
    OfferEntry& oe = le.data.offer();

    auto& st = prep.statement();
    st.exchange(soci::into(actIDStrKey));
    st.exchange(soci::into(oe.offerID));
    st.exchange(soci::into(sellingAssetType));
    st.exchange(soci::into(sellingAssetCode, sellingAssetCodeIndicator));
    st.exchange(soci::into(sellingIssuerStrKey, sellingIssuerIndicator));
    st.exchange(soci::into(buyingAssetType));
    st.exchange(soci::into(buyingAssetCode, buyingAssetCodeIndicator));
    st.exchange(soci::into(buyingIssuerStrKey, buyingIssuerIndicator));
    st.exchange(soci::into(oe.amount));
    st.exchange(soci::into(oe.price.n));
    st.exchange(soci::into(oe.price.d));
    st.exchange(soci::into(oe.flags));
    st.exchange(soci::into(le.lastModifiedLedgerSeq));
    st.define_and_bind();
    st.execute(true);
    while (st.got_data())
    {
        oe.sellerID = KeyUtils::fromStrKey<PublicKey>(actIDStrKey);
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
                    KeyUtils::fromStrKey<PublicKey>(sellingIssuerStrKey);
                strToAssetCode(oe.selling.alphaNum12().assetCode,
                               sellingAssetCode);
            }
            else if (sellingAssetType == ASSET_TYPE_CREDIT_ALPHANUM4)
            {
                oe.selling.alphaNum4().issuer =
                    KeyUtils::fromStrKey<PublicKey>(sellingIssuerStrKey);
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
                    KeyUtils::fromStrKey<PublicKey>(buyingIssuerStrKey);
                strToAssetCode(oe.buying.alphaNum12().assetCode,
                               buyingAssetCode);
            }
            else if (buyingAssetType == ASSET_TYPE_CREDIT_ALPHANUM4)
            {
                oe.buying.alphaNum4().issuer =
                    KeyUtils::fromStrKey<PublicKey>(buyingIssuerStrKey);
                strToAssetCode(oe.buying.alphaNum4().assetCode,
                               buyingAssetCode);
            }
        }

        if (!OfferFrame{oe}.isValid())
        {
            throw std::runtime_error("Invalid asset");
        }

        result.push_back(le);
        st.fetch();
    }

    return result;
}
}

void
createOffersTable(Database& db)
{
    db.getSession() << DROP_OFFERS_TABLE;
    db.getSession() << CREATE_OFFERS_TABLE;
    db.getSession() << CREATE_SELLING_ISSUER_INDEX;
    db.getSession() << CREATE_BUYING_ISSUER_INDEX;
    db.getSession() << CREATE_PRICE_INDEX;
}

std::vector<LedgerEntry>
selectBestOffers(size_t numOffers, size_t offset, Asset const& selling,
                 Asset const& buying, Database& db)
{
    std::string sql = OFFER_COLUMN_SELECTOR;

    std::string sellingAssetCode, sellingIssuerStrKey;
    std::string buyingAssetCode, buyingIssuerStrKey;

    bool useSellingAsset = false;
    bool useBuyingAsset = false;

    if (selling.type() == ASSET_TYPE_NATIVE)
    {
        sql += " WHERE sellingassettype = 0 AND sellingissuer IS NULL";
    }
    else
    {
        if (selling.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
        {
            assetCodeToStr(selling.alphaNum4().assetCode, sellingAssetCode);
            sellingIssuerStrKey =
                KeyUtils::toStrKey(selling.alphaNum4().issuer);
        }
        else if (selling.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
        {
            assetCodeToStr(selling.alphaNum12().assetCode, sellingAssetCode);
            sellingIssuerStrKey =
                KeyUtils::toStrKey(selling.alphaNum12().issuer);
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
        sql += " AND buyingassettype = 0 AND buyingissuer IS NULL";
    }
    else
    {
        if (buying.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
        {
            assetCodeToStr(buying.alphaNum4().assetCode, buyingAssetCode);
            buyingIssuerStrKey = KeyUtils::toStrKey(buying.alphaNum4().issuer);
        }
        else if (buying.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
        {
            assetCodeToStr(buying.alphaNum12().assetCode, buyingAssetCode);
            buyingIssuerStrKey = KeyUtils::toStrKey(buying.alphaNum12().issuer);
        }
        else
        {
            throw std::runtime_error("unknown asset type");
        }

        useBuyingAsset = true;
        sql += " AND buyingassetcode = :gcur AND buyingissuer = :gi";
    }

    // price is an approximation of the actual n/d (truncated math, 15 digits)
    // ordering by offerid gives precendence to older offers for fairness
    sql += " ORDER BY price, offerid LIMIT :n OFFSET :o";

    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();

    if (useSellingAsset)
    {
        st.exchange(soci::use(sellingAssetCode));
        st.exchange(soci::use(sellingIssuerStrKey));
    }

    if (useBuyingAsset)
    {
        st.exchange(soci::use(buyingAssetCode));
        st.exchange(soci::use(buyingIssuerStrKey));
    }

    st.exchange(soci::use(numOffers));
    st.exchange(soci::use(offset));

    auto timer = db.getSelectTimer("offer");
    return loadOffers(prep, db);
}

optional<LedgerEntry const>
selectOffer(AccountID const& sellerID, uint64_t offerID, Database& db)
{
    auto actIDStrKey = KeyUtils::toStrKey(sellerID);

    std::string sql = OFFER_COLUMN_SELECTOR;
    sql += " WHERE sellerid = :id AND offerid = :offerid";
    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey));
    st.exchange(soci::use(offerID));

    auto timer = db.getSelectTimer("offer");
    auto offers = loadOffers(prep, db);
    if (offers.size() == 1)
    {
        return make_optional<LedgerEntry const>(offers[0]);
    }

    return nullopt<LedgerEntry>();
}

void
insertOffer(LedgerEntry const& entry, Database& db)
{
    assert(entry.data.type() == OFFER);

    auto offerFrame = OfferFrame{entry};
    if (!offerFrame.isValid())
    {
        throw std::runtime_error("Invalid asset");
    }

    auto& offer = entry.data.offer();
    auto actIDStrKey = KeyUtils::toStrKey(offer.sellerID);

    unsigned int sellingType = offer.selling.type();
    unsigned int buyingType = offer.buying.type();
    std::string sellingIssuerStrKey, buyingIssuerStrKey;
    std::string sellingAssetCode, buyingAssetCode;
    soci::indicator selling_ind = soci::i_null, buying_ind = soci::i_null;

    if (sellingType == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        sellingIssuerStrKey =
            KeyUtils::toStrKey(offer.selling.alphaNum4().issuer);
        assetCodeToStr(offer.selling.alphaNum4().assetCode,
                       sellingAssetCode);
        selling_ind = soci::i_ok;
    }
    else if (sellingType == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        sellingIssuerStrKey =
            KeyUtils::toStrKey(offer.selling.alphaNum12().issuer);
        assetCodeToStr(offer.selling.alphaNum12().assetCode,
                       sellingAssetCode);
        selling_ind = soci::i_ok;
    }

    if (buyingType == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        buyingIssuerStrKey =
            KeyUtils::toStrKey(offer.buying.alphaNum4().issuer);
        assetCodeToStr(offer.buying.alphaNum4().assetCode,
                       buyingAssetCode);
        buying_ind = soci::i_ok;
    }
    else if (buyingType == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        buyingIssuerStrKey =
            KeyUtils::toStrKey(offer.buying.alphaNum12().issuer);
        assetCodeToStr(offer.buying.alphaNum12().assetCode,
                       buyingAssetCode);
        buying_ind = soci::i_ok;
    }

    auto sql = "INSERT INTO offers (sellerid,offerid,"
               "sellingassettype,sellingassetcode,sellingissuer,"
               "buyingassettype,buyingassetcode,buyingissuer,"
               "amount,pricen,priced,price,flags,lastmodified) VALUES "
               "(:sid,:oid,:sat,:sac,:si,:bat,:bac,:bi,:a,:pn,:pd,:p,:f,:l)";
    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey, "sid"));
    st.exchange(soci::use(offer.offerID, "oid"));
    st.exchange(soci::use(sellingType, "sat"));
    st.exchange(soci::use(sellingAssetCode, selling_ind, "sac"));
    st.exchange(soci::use(sellingIssuerStrKey, selling_ind, "si"));
    st.exchange(soci::use(buyingType, "bat"));
    st.exchange(soci::use(buyingAssetCode, buying_ind, "bac"));
    st.exchange(soci::use(buyingIssuerStrKey, buying_ind, "bi"));
    st.exchange(soci::use(offer.amount, "a"));
    st.exchange(soci::use(offer.price.n, "pn"));
    st.exchange(soci::use(offer.price.d, "pd"));
    auto computedPrice = offerFrame.computePrice(); // do not ever pass result of function to soci::use
    st.exchange(soci::use(computedPrice, "p"));
    st.exchange(soci::use(offer.flags, "f"));
    st.exchange(soci::use(entry.lastModifiedLedgerSeq, "l"));
    st.define_and_bind();

    auto timer = db.getInsertTimer("offer");
    st.execute(true);

    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("could not update SQL");
    }
}

void
updateOffer(LedgerEntry const& entry, Database& db)
{
    assert(entry.data.type() == OFFER);

    auto offerFrame = OfferFrame{entry};
    if (!offerFrame.isValid())
    {
        throw std::runtime_error("Invalid asset");
    }

    auto& offer = entry.data.offer();
    auto actIDStrKey = KeyUtils::toStrKey(offer.sellerID);

    unsigned int sellingType = offer.selling.type();
    unsigned int buyingType = offer.buying.type();
    std::string sellingIssuerStrKey, buyingIssuerStrKey;
    std::string sellingAssetCode, buyingAssetCode;
    soci::indicator selling_ind = soci::i_null, buying_ind = soci::i_null;

    if (sellingType == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        sellingIssuerStrKey =
            KeyUtils::toStrKey(offer.selling.alphaNum4().issuer);
        assetCodeToStr(offer.selling.alphaNum4().assetCode,
                       sellingAssetCode);
        selling_ind = soci::i_ok;
    }
    else if (sellingType == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        sellingIssuerStrKey =
            KeyUtils::toStrKey(offer.selling.alphaNum12().issuer);
        assetCodeToStr(offer.selling.alphaNum12().assetCode,
                       sellingAssetCode);
        selling_ind = soci::i_ok;
    }

    if (buyingType == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        buyingIssuerStrKey =
            KeyUtils::toStrKey(offer.buying.alphaNum4().issuer);
        assetCodeToStr(offer.buying.alphaNum4().assetCode,
                       buyingAssetCode);
        buying_ind = soci::i_ok;
    }
    else if (buyingType == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        buyingIssuerStrKey =
            KeyUtils::toStrKey(offer.buying.alphaNum12().issuer);
        assetCodeToStr(offer.buying.alphaNum12().assetCode,
                       buyingAssetCode);
        buying_ind = soci::i_ok;
    }

    auto sql = "UPDATE offers SET sellingassettype=:sat "
               ",sellingassetcode=:sac,sellingissuer=:si,"
               "buyingassettype=:bat,buyingassetcode=:bac,buyingissuer=:bi,"
               "amount=:a,pricen=:pn,priced=:pd,price=:p,flags=:f,"
               "lastmodified=:l WHERE offerid=:oid";

    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();

    st.exchange(soci::use(offer.offerID, "oid"));
    st.exchange(soci::use(sellingType, "sat"));
    st.exchange(soci::use(sellingAssetCode, selling_ind, "sac"));
    st.exchange(soci::use(sellingIssuerStrKey, selling_ind, "si"));
    st.exchange(soci::use(buyingType, "bat"));
    st.exchange(soci::use(buyingAssetCode, buying_ind, "bac"));
    st.exchange(soci::use(buyingIssuerStrKey, buying_ind, "bi"));
    st.exchange(soci::use(offer.amount, "a"));
    st.exchange(soci::use(offer.price.n, "pn"));
    st.exchange(soci::use(offer.price.d, "pd"));
    auto computedPrice = offerFrame.computePrice(); // do not ever pass result of function to soci::use
    st.exchange(soci::use(computedPrice, "p"));
    st.exchange(soci::use(offer.flags, "f"));
    st.exchange(soci::use(entry.lastModifiedLedgerSeq, "l"));
    st.define_and_bind();

    auto timer = db.getUpdateTimer("offer");
    st.execute(true);

    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("could not update SQL");
    }
}

bool
offerExists(LedgerKey const& key, Database& db)
{
    assert(key.type() == OFFER);

    auto actIDStrKey = KeyUtils::toStrKey(key.offer().sellerID);
    auto exists = 0;
    auto timer = db.getSelectTimer("offer-exists");
    auto prep =
        db.getPreparedStatement("SELECT EXISTS (SELECT NULL FROM offers "
                                "WHERE sellerid=:id AND offerid=:s)");
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey));
    st.exchange(soci::use(key.offer().offerID));
    st.exchange(soci::into(exists));
    st.define_and_bind();
    st.execute(true);
    return exists != 0;
}

void
deleteOffer(LedgerKey const& key, Database& db)
{
    assert(key.type() == OFFER);

    auto timer = db.getDeleteTimer("offer");
    auto prep = db.getPreparedStatement("DELETE FROM offers WHERE offerid=:s");
    auto& st = prep.statement();
    st.exchange(soci::use(key.offer().offerID));
    st.define_and_bind();
    st.execute(true);
}

std::unordered_map<AccountID, int>
selectOfferCountPerAccount(Database& db)
{
    auto result = std::unordered_map<AccountID, int>{};
    auto count = 0;
    auto sellerId = std::string{};

    auto sql = std::string{R"(
        SELECT COUNT(*), sellerid FROM offers GROUP BY sellerid
    )"};

    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::into(count));
    st.exchange(soci::into(sellerId));
    st.define_and_bind();

    auto timer = db.getSelectTimer("offer");
    st.execute(true);

    while (st.got_data())
    {
        result.insert(
            std::make_pair(KeyUtils::fromStrKey<PublicKey>(sellerId), count));
        st.fetch();
    }

    return result;
}

uint64_t
countOffers(Database& db)
{
    auto query = std::string{R"(
        SELECT COUNT(*) FROM offers
    )"};

    auto result = 0;
    auto prep = db.getPreparedStatement(query);
    auto& st = prep.statement();
    st.exchange(soci::into(result));
    st.define_and_bind();
    st.execute(true);

    return result;
}
}
