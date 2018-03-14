// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "ledger/LedgerEntryReference.h"
#include "ledger/LedgerState.h"
#include "util/types.h"

namespace stellar
{
using xdr::operator==;
using xdr::operator<;
using xdr::operator>;

void
processAsset(Asset& asset, AssetType assetType, std::string const& issuerStr,
             soci::indicator const& issuerIndicator,
             std::string const& assetCode,
             soci::indicator const& assetCodeIndicator)
{
    asset.type(assetType);
    if (assetType != ASSET_TYPE_NATIVE)
    {
        if ((assetCodeIndicator != soci::i_ok) ||
            (issuerIndicator != soci::i_ok))
        {
            throw std::runtime_error("bad database state");
        }

        if (assetType == ASSET_TYPE_CREDIT_ALPHANUM12)
        {
            asset.alphaNum12().issuer =
                KeyUtils::fromStrKey<PublicKey>(issuerStr);
            strToAssetCode(asset.alphaNum12().assetCode, assetCode);
        }
        else if (assetType == ASSET_TYPE_CREDIT_ALPHANUM4)
        {
            asset.alphaNum4().issuer =
                KeyUtils::fromStrKey<PublicKey>(issuerStr);
            strToAssetCode(asset.alphaNum4().assetCode, assetCode);
        }
        else
        {
            throw std::runtime_error("bad database state");
        }
    }
}

LedgerState::StateEntry
LedgerState::loadOfferFromDatabase(LedgerKey const& key)
{
    auto& db = mRoot->getDatabase();

    uint64_t offerID = key.offer().offerID;
    std::string actIDStrKey = KeyUtils::toStrKey(key.offer().sellerID);

    std::string sql = "SELECT sellerid, offerid, "
                      "sellingassettype, sellingassetcode, sellingissuer, "
                      "buyingassettype, buyingassetcode, buyingissuer, "
                      "amount, pricen, priced, flags, lastmodified "
                      "FROM offers "
                      "WHERE sellerid= :id AND offerid= :offerid";
    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey));
    st.exchange(soci::use(offerID));

    std::vector<StateEntry> offers;
    {
        auto timer = db.getSelectTimer("offer");
        offers = loadOffersFromDatabase(prep);
    }

    if (offers.size() == 0)
    {
        return nullptr;
    }
    else
    {
        return offers.front();
    }
}

std::vector<LedgerState::StateEntry>
LedgerState::loadOffersFromDatabase(StatementContext& prep)
{
    std::vector<StateEntry> offers;

    std::string actIDStrKey;
    unsigned int sellingAssetType, buyingAssetType;
    std::string sellingAssetCode, buyingAssetCode, sellingIssuerStrKey,
        buyingIssuerStrKey;
    soci::indicator sellingAssetCodeIndicator, buyingAssetCodeIndicator,
        sellingIssuerIndicator, buyingIssuerIndicator;

    LedgerEntry le;
    le.data.type(OFFER);
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
        processAsset(oe.selling, (AssetType)sellingAssetType,
                     sellingIssuerStrKey, sellingIssuerIndicator,
                     sellingAssetCode, sellingAssetCodeIndicator);
        processAsset(oe.buying, (AssetType)buyingAssetType, buyingIssuerStrKey,
                     buyingIssuerIndicator, buyingAssetCode,
                     buyingAssetCodeIndicator);

        auto entry = std::make_shared<LedgerEntry>(std::move(le));
        offers.emplace_back(makeStateEntry(entry, entry));
        st.fetch();
    }

    return offers;
}

std::vector<LedgerState::StateEntry>
LedgerState::loadBestOffersFromDatabase(size_t numOffers, size_t offset,
                                        Asset const& selling,
                                        Asset const& buying)
{
    auto& db = mRoot->getDatabase();

    std::string sql = "SELECT sellerid, offerid, "
                      "sellingassettype, sellingassetcode, sellingissuer, "
                      "buyingassettype, buyingassetcode, buyingissuer, "
                      "amount, pricen, priced, flags, lastmodified "
                      "FROM offers ";

    std::string sellingAssetCode, sellingIssuerStrKey;
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
        sql += " WHERE sellingassetcode = :sac AND sellingissuer = :si";
    }

    std::string buyingAssetCode, buyingIssuerStrKey;
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
        sql += " AND buyingassetcode = :bac AND buyingissuer = :bi";
    }

    // price is an approximation of the actual n/d (truncated math, 15 digits)
    // ordering by offerid gives precendence to older offers for fairness
    sql += " ORDER BY price, offerid LIMIT :n OFFSET :o";

    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    if (selling.type() != ASSET_TYPE_NATIVE)
    {
        st.exchange(soci::use(sellingAssetCode, "sac"));
        st.exchange(soci::use(sellingIssuerStrKey, "si"));
    }
    if (buying.type() != ASSET_TYPE_NATIVE)
    {
        st.exchange(soci::use(buyingAssetCode, "bac"));
        st.exchange(soci::use(buyingIssuerStrKey, "bi"));
    }
    st.exchange(soci::use(numOffers, "n"));
    st.exchange(soci::use(offset, "o"));

    std::vector<StateEntry> offers;
    {
        auto timer = db.getSelectTimer("offer");
        offers = loadOffersFromDatabase(prep);
    }
    return offers;
}

void
LedgerState::storeOfferInDatabase(StateEntry const& state)
{
    bool isInsert = !state->ignoreInvalid().previousEntry();
    auto& db = mRoot->getDatabase();

    auto const& entry = *state->ignoreInvalid().entry();
    auto const& offer = entry.data.offer();
    std::string actIDStrKey = KeyUtils::toStrKey(offer.sellerID);

    unsigned int sellingType = offer.selling.type();
    unsigned int buyingType = offer.buying.type();
    std::string sellingIssuerStrKey, buyingIssuerStrKey;
    std::string sellingAssetCode, buyingAssetCode;
    soci::indicator selling_ind = soci::i_null, buying_ind = soci::i_null;
    double price = double(offer.price.n) / double(offer.price.d);

    if (sellingType == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        sellingIssuerStrKey =
            KeyUtils::toStrKey(offer.selling.alphaNum4().issuer);
        assetCodeToStr(offer.selling.alphaNum4().assetCode, sellingAssetCode);
        selling_ind = soci::i_ok;
    }
    else if (sellingType == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        sellingIssuerStrKey =
            KeyUtils::toStrKey(offer.selling.alphaNum12().issuer);
        assetCodeToStr(offer.selling.alphaNum12().assetCode, sellingAssetCode);
        selling_ind = soci::i_ok;
    }

    if (buyingType == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        buyingIssuerStrKey =
            KeyUtils::toStrKey(offer.buying.alphaNum4().issuer);
        assetCodeToStr(offer.buying.alphaNum4().assetCode, buyingAssetCode);
        buying_ind = soci::i_ok;
    }
    else if (buyingType == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        buyingIssuerStrKey =
            KeyUtils::toStrKey(offer.buying.alphaNum12().issuer);
        assetCodeToStr(offer.buying.alphaNum12().assetCode, buyingAssetCode);
        buying_ind = soci::i_ok;
    }

    std::string sql;
    if (isInsert)
    {
        sql = "INSERT INTO offers (sellerid,offerid,"
              "sellingassettype,sellingassetcode,sellingissuer,"
              "buyingassettype,buyingassetcode,buyingissuer,"
              "amount,pricen,priced,price,flags,lastmodified) VALUES "
              "(:sid,:oid,:sat,:sac,:si,:bat,:bac,:bi,:a,:pn,:pd,:p,:f,:l)";
    }
    else
    {
        sql = "UPDATE offers SET sellingassettype=:sat,"
              "sellingassetcode=:sac,sellingissuer=:si,"
              "buyingassettype=:bat,buyingassetcode=:bac,buyingissuer=:bi,"
              "amount=:a,pricen=:pn,priced=:pd,price=:p,flags=:f,"
              "lastmodified=:l WHERE offerid=:oid";
    }

    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    if (isInsert)
    {
        st.exchange(soci::use(actIDStrKey, "sid"));
    }
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
    st.exchange(soci::use(price, "p"));
    st.exchange(soci::use(offer.flags, "f"));
    st.exchange(soci::use(entry.lastModifiedLedgerSeq, "l"));
    st.define_and_bind();
    {
        auto timer =
            isInsert ? db.getInsertTimer("offer") : db.getUpdateTimer("offer");
        st.execute(true);
    }
    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("could not update SQL");
    }
}

void
LedgerState::deleteOfferFromDatabase(StateEntry const& state)
{
    auto& db = mRoot->getDatabase();

    auto const& previousEntry = state->ignoreInvalid().previousEntry();
    auto const& offer = previousEntry->data.offer();

    auto prep = db.getPreparedStatement("DELETE FROM offers WHERE offerid=:s");
    auto& st = prep.statement();
    st.exchange(soci::use(offer.offerID));
    st.define_and_bind();
    {
        auto timer = db.getDeleteTimer("offer");
        st.execute(true);
    }
    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }
}
}
