// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "database/DatabaseTypeSpecificOperation.h"
#include "ledger/LedgerTxnImpl.h"
#include "util/XDROperators.h"
#include "util/types.h"

namespace stellar
{

void
getAssetStrings(Asset const& asset, std::string& assetCodeStr,
                std::string& issuerStr, soci::indicator& assetCodeIndicator,
                soci::indicator& issuerIndicator)
{
    if (asset.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        assetCodeToStr(asset.alphaNum4().assetCode, assetCodeStr);
        issuerStr = KeyUtils::toStrKey(asset.alphaNum4().issuer);
        assetCodeIndicator = soci::i_ok;
        issuerIndicator = soci::i_ok;
    }
    else if (asset.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        assetCodeToStr(asset.alphaNum12().assetCode, assetCodeStr);
        issuerStr = KeyUtils::toStrKey(asset.alphaNum12().issuer);
        assetCodeIndicator = soci::i_ok;
        issuerIndicator = soci::i_ok;
    }
    else
    {
        assert(asset.type() == ASSET_TYPE_NATIVE);
        assetCodeStr = "";
        issuerStr = "";
        assetCodeIndicator = soci::i_null;
        issuerIndicator = soci::i_null;
    }
}

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

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::loadOffer(LedgerKey const& key) const
{
    uint64_t offerID = key.offer().offerID;
    std::string actIDStrKey = KeyUtils::toStrKey(key.offer().sellerID);

    std::string sql = "SELECT sellerid, offerid, "
                      "sellingassettype, sellingassetcode, sellingissuer, "
                      "buyingassettype, buyingassetcode, buyingissuer, "
                      "amount, pricen, priced, flags, lastmodified "
                      "FROM offers "
                      "WHERE sellerid= :id AND offerid= :offerid";
    auto prep = mDatabase.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey));
    int64_t signedOfferID = unsignedToSigned(offerID);
    st.exchange(soci::use(signedOfferID));

    std::vector<LedgerEntry> offers;
    {
        auto timer = mDatabase.getSelectTimer("offer");
        offers = loadOffers(prep);
    }

    return offers.size() == 0
               ? nullptr
               : std::make_shared<LedgerEntry const>(offers.front());
}

std::vector<LedgerEntry>
LedgerTxnRoot::Impl::loadAllOffers() const
{
    std::string sql = "SELECT sellerid, offerid, "
                      "sellingassettype, sellingassetcode, sellingissuer, "
                      "buyingassettype, buyingassetcode, buyingissuer, "
                      "amount, pricen, priced, flags, lastmodified "
                      "FROM offers";
    auto prep = mDatabase.getPreparedStatement(sql);

    std::vector<LedgerEntry> offers;
    {
        auto timer = mDatabase.getSelectTimer("offer");
        offers = loadOffers(prep);
    }
    return offers;
}

std::list<LedgerEntry>::const_iterator
LedgerTxnRoot::Impl::loadBestOffers(std::list<LedgerEntry>& offers,
                                    Asset const& buying, Asset const& selling,
                                    size_t numOffers, size_t offset) const
{
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

    auto prep = mDatabase.getPreparedStatement(sql);
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

    {
        auto timer = mDatabase.getSelectTimer("offer");
        return loadOffers(prep, offers);
    }
}

// Note: The order induced by this function must match the order used in the
// SQL query for loadBestOffers above.
bool
isBetterOffer(LedgerEntry const& lhsEntry, LedgerEntry const& rhsEntry)
{
    auto const& lhs = lhsEntry.data.offer();
    auto const& rhs = rhsEntry.data.offer();

    assert(lhs.buying == rhs.buying);
    assert(lhs.selling == rhs.selling);

    double lhsPrice = double(lhs.price.n) / double(lhs.price.d);
    double rhsPrice = double(rhs.price.n) / double(rhs.price.d);
    if (lhsPrice < rhsPrice)
    {
        return true;
    }
    else if (lhsPrice == rhsPrice)
    {
        return lhs.offerID < rhs.offerID;
    }
    else
    {
        return false;
    }
}

// Note: This function is currently only used in AllowTrustOpFrame, which means
// the asset parameter will never satisfy asset.type() == ASSET_TYPE_NATIVE. As
// a consequence, I have not implemented that possibility so this function
// throws in that case.
std::vector<LedgerEntry>
LedgerTxnRoot::Impl::loadOffersByAccountAndAsset(AccountID const& accountID,
                                                 Asset const& asset) const
{
    std::string sql = "SELECT sellerid, offerid, "
                      "sellingassettype, sellingassetcode, sellingissuer, "
                      "buyingassettype, buyingassetcode, buyingissuer, "
                      "amount, pricen, priced, flags, lastmodified "
                      "FROM offers ";
    sql += " WHERE sellerid = :acc"
           " AND ((sellingassetcode = :code AND sellingissuer = :iss)"
           " OR   (buyingassetcode = :code AND buyingissuer = :iss))";

    std::string accountStr = KeyUtils::toStrKey(accountID);

    std::string assetCode;
    std::string assetIssuer;
    if (asset.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        assetCodeToStr(asset.alphaNum4().assetCode, assetCode);
        assetIssuer = KeyUtils::toStrKey(asset.alphaNum4().issuer);
    }
    else if (asset.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        assetCodeToStr(asset.alphaNum12().assetCode, assetCode);
        assetIssuer = KeyUtils::toStrKey(asset.alphaNum12().issuer);
    }
    else
    {
        throw std::runtime_error("Invalid asset type");
    }

    auto prep = mDatabase.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::use(accountStr, "acc"));
    st.exchange(soci::use(assetCode, "code"));
    st.exchange(soci::use(assetIssuer, "iss"));

    std::vector<LedgerEntry> offers;
    {
        auto timer = mDatabase.getSelectTimer("offer");
        offers = loadOffers(prep);
    }
    return offers;
}

std::vector<LedgerEntry>
LedgerTxnRoot::Impl::loadOffers(StatementContext& prep) const
{
    std::vector<LedgerEntry> offers;

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

        offers.emplace_back(le);
        st.fetch();
    }

    return offers;
}

std::list<LedgerEntry>::const_iterator
LedgerTxnRoot::Impl::loadOffers(StatementContext& prep,
                                std::list<LedgerEntry>& offers) const
{
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

    auto iterNext = offers.cend();
    while (st.got_data())
    {
        oe.sellerID = KeyUtils::fromStrKey<PublicKey>(actIDStrKey);
        processAsset(oe.selling, (AssetType)sellingAssetType,
                     sellingIssuerStrKey, sellingIssuerIndicator,
                     sellingAssetCode, sellingAssetCodeIndicator);
        processAsset(oe.buying, (AssetType)buyingAssetType, buyingIssuerStrKey,
                     buyingIssuerIndicator, buyingAssetCode,
                     buyingAssetCodeIndicator);

        if (iterNext == offers.cend())
        {
            iterNext = offers.emplace(iterNext, le);
        }
        else
        {
            offers.emplace_back(le);
        }
        st.fetch();
    }

    return iterNext;
}

class BulkUpsertOffersOperation : public DatabaseTypeSpecificOperation
{
    Database& mDB;
    std::vector<std::string> mSellerIDs;
    std::vector<int64_t> mOfferIDs;
    std::vector<int32_t> mSellingAssetTypes;
    std::vector<std::string> mSellingAssetCodes;
    std::vector<std::string> mSellingIssuers;
    std::vector<soci::indicator> mSellingAssetCodeInds;
    std::vector<soci::indicator> mSellingIssuerInds;
    std::vector<int32_t> mBuyingAssetTypes;
    std::vector<std::string> mBuyingAssetCodes;
    std::vector<std::string> mBuyingIssuers;
    std::vector<soci::indicator> mBuyingAssetCodeInds;
    std::vector<soci::indicator> mBuyingIssuerInds;
    std::vector<int64_t> mAmounts;
    std::vector<int32_t> mPriceNs;
    std::vector<int32_t> mPriceDs;
    std::vector<double> mPrices;
    std::vector<int32_t> mFlags;
    std::vector<int32_t> mLastModifieds;

  public:
    BulkUpsertOffersOperation(Database& DB,
                              std::vector<EntryIterator> const& entries)
        : mDB(DB)
    {
        mSellerIDs.reserve(entries.size());
        mOfferIDs.reserve(entries.size());
        mSellingAssetTypes.reserve(entries.size());
        mSellingAssetCodes.reserve(entries.size());
        mSellingIssuers.reserve(entries.size());
        mBuyingAssetTypes.reserve(entries.size());
        mBuyingAssetCodes.reserve(entries.size());
        mBuyingIssuers.reserve(entries.size());
        mAmounts.reserve(entries.size());
        mPriceNs.reserve(entries.size());
        mPriceDs.reserve(entries.size());
        mPrices.reserve(entries.size());
        mFlags.reserve(entries.size());
        mLastModifieds.reserve(entries.size());

        for (auto const& e : entries)
        {
            assert(e.entryExists());
            assert(e.entry().data.type() == OFFER);
            auto const& offer = e.entry().data.offer();
            std::string sellerIDStr, sellingIssuerStr, sellingAssetCodeStr,
                buyingIssuerStr, buyingAssetCodeStr;
            soci::indicator sellingIssuerInd, sellingAssetCodeInd,
                buyingIssuerInd, buyingAssetCodeInd;
            getAssetStrings(offer.selling, sellingAssetCodeStr,
                            sellingIssuerStr, sellingAssetCodeInd,
                            sellingIssuerInd);
            getAssetStrings(offer.buying, buyingAssetCodeStr, buyingIssuerStr,
                            buyingAssetCodeInd, buyingIssuerInd);

            sellerIDStr = KeyUtils::toStrKey(offer.sellerID);
            mSellerIDs.emplace_back(sellerIDStr);
            mOfferIDs.emplace_back(offer.offerID);

            mSellingAssetTypes.emplace_back(
                unsignedToSigned(static_cast<uint32_t>(offer.selling.type())));
            mSellingAssetCodes.emplace_back(sellingAssetCodeStr);
            mSellingIssuers.emplace_back(sellingIssuerStr);
            mSellingAssetCodeInds.emplace_back(sellingAssetCodeInd);
            mSellingIssuerInds.emplace_back(sellingIssuerInd);

            mBuyingAssetTypes.emplace_back(
                unsignedToSigned(static_cast<uint32_t>(offer.buying.type())));
            mBuyingAssetCodes.emplace_back(buyingAssetCodeStr);
            mBuyingIssuers.emplace_back(buyingIssuerStr);
            mBuyingAssetCodeInds.emplace_back(buyingAssetCodeInd);
            mBuyingIssuerInds.emplace_back(buyingIssuerInd);

            mAmounts.emplace_back(offer.amount);
            mPriceNs.emplace_back(offer.price.n);
            mPriceDs.emplace_back(offer.price.d);
            double price = double(offer.price.n) / double(offer.price.d);
            mPrices.emplace_back(price);

            mFlags.emplace_back(unsignedToSigned(offer.flags));
            mLastModifieds.emplace_back(
                unsignedToSigned(e.entry().lastModifiedLedgerSeq));
        }
    }

    void
    doSociGenericOperation()
    {
        std::string sql = "INSERT INTO offers ( "
                          "sellerid, offerid, "
                          "sellingassettype, sellingassetcode, sellingissuer, "
                          "buyingassettype, buyingassetcode, buyingissuer, "
                          "amount, pricen, priced, price, flags, lastmodified "
                          ") VALUES ( "
                          ":sellerid, :offerid, :v1, :v2, :v3, :v4, :v5, :v6, "
                          ":v7, :v8, :v9, :v10, :v11, :v12 "
                          ") ON CONFLICT (offerid) DO UPDATE SET "
                          "sellerid = excluded.sellerid, "
                          "sellingassettype = excluded.sellingassettype, "
                          "sellingassetcode = excluded.sellingassetcode, "
                          "sellingissuer = excluded.sellingissuer, "
                          "buyingassettype = excluded.buyingassettype, "
                          "buyingassetcode = excluded.buyingassetcode, "
                          "buyingissuer = excluded.buyingissuer, "
                          "amount = excluded.amount, "
                          "pricen = excluded.pricen, "
                          "priced = excluded.priced, "
                          "price = excluded.price, "
                          "flags = excluded.flags, "
                          "lastmodified = excluded.lastmodified ";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(mSellerIDs));
        st.exchange(soci::use(mOfferIDs));
        st.exchange(soci::use(mSellingAssetTypes));
        st.exchange(soci::use(mSellingAssetCodes, mSellingAssetCodeInds));
        st.exchange(soci::use(mSellingIssuers, mSellingIssuerInds));
        st.exchange(soci::use(mBuyingAssetTypes));
        st.exchange(soci::use(mBuyingAssetCodes, mBuyingAssetCodeInds));
        st.exchange(soci::use(mBuyingIssuers, mBuyingIssuerInds));
        st.exchange(soci::use(mAmounts));
        st.exchange(soci::use(mPriceNs));
        st.exchange(soci::use(mPriceDs));
        st.exchange(soci::use(mPrices));
        st.exchange(soci::use(mFlags));
        st.exchange(soci::use(mLastModifieds));
        st.define_and_bind();
        {
            auto timer = mDB.getUpsertTimer("offer");
            st.execute(true);
        }
        if (st.get_affected_rows() != mOfferIDs.size())
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }

    void
    doSqliteSpecificOperation(soci::sqlite3_session_backend* sq) override
    {
        doSociGenericOperation();
    }

#ifdef USE_POSTGRES
    void
    doPostgresSpecificOperation(soci::postgresql_session_backend* pg) override
    {

        std::string strSellerIDs, strOfferIDs, strSellingAssetTypes,
            strSellingAssetCodes, strSellingIssuers, strBuyingAssetTypes,
            strBuyingAssetCodes, strBuyingIssuers, strAmounts, strPriceNs,
            strPriceDs, strPrices, strFlags, strLastModifieds;

        PGconn* conn = pg->conn_;
        marshalToPGArray(conn, strSellerIDs, mSellerIDs);
        marshalToPGArray(conn, strOfferIDs, mOfferIDs);

        marshalToPGArray(conn, strSellingAssetTypes, mSellingAssetTypes);
        marshalToPGArray(conn, strSellingAssetCodes, mSellingAssetCodes,
                         &mSellingAssetCodeInds);
        marshalToPGArray(conn, strSellingIssuers, mSellingIssuers,
                         &mSellingIssuerInds);

        marshalToPGArray(conn, strBuyingAssetTypes, mBuyingAssetTypes);
        marshalToPGArray(conn, strBuyingAssetCodes, mBuyingAssetCodes,
                         &mBuyingAssetCodeInds);
        marshalToPGArray(conn, strBuyingIssuers, mBuyingIssuers,
                         &mBuyingIssuerInds);

        marshalToPGArray(conn, strAmounts, mAmounts);
        marshalToPGArray(conn, strPriceNs, mPriceNs);
        marshalToPGArray(conn, strPriceDs, mPriceDs);
        marshalToPGArray(conn, strPrices, mPrices);
        marshalToPGArray(conn, strFlags, mFlags);
        marshalToPGArray(conn, strLastModifieds, mLastModifieds);

        std::string sql = "WITH r AS (SELECT "
                          "unnest(:sellerids::TEXT[]), "
                          "unnest(:offerids::BIGINT[]), "
                          "unnest(:v1::INT[]), "
                          "unnest(:v2::TEXT[]), "
                          "unnest(:v3::TEXT[]), "
                          "unnest(:v4::INT[]), "
                          "unnest(:v5::TEXT[]), "
                          "unnest(:v6::TEXT[]), "
                          "unnest(:v7::BIGINT[]), "
                          "unnest(:v8::INT[]), "
                          "unnest(:v9::INT[]), "
                          "unnest(:v10::DOUBLE PRECISION[]), "
                          "unnest(:v11::INT[]), "
                          "unnest(:v12::INT[]) "
                          ")"
                          "INSERT INTO offers ( "
                          "sellerid, offerid, "
                          "sellingassettype, sellingassetcode, sellingissuer, "
                          "buyingassettype, buyingassetcode, buyingissuer, "
                          "amount, pricen, priced, price, flags, lastmodified "
                          ") SELECT * from r "
                          "ON CONFLICT (offerid) DO UPDATE SET "
                          "sellerid = excluded.sellerid, "
                          "sellingassettype = excluded.sellingassettype, "
                          "sellingassetcode = excluded.sellingassetcode, "
                          "sellingissuer = excluded.sellingissuer, "
                          "buyingassettype = excluded.buyingassettype, "
                          "buyingassetcode = excluded.buyingassetcode, "
                          "buyingissuer = excluded.buyingissuer, "
                          "amount = excluded.amount, "
                          "pricen = excluded.pricen, "
                          "priced = excluded.priced, "
                          "price = excluded.price, "
                          "flags = excluded.flags, "
                          "lastmodified = excluded.lastmodified ";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(strSellerIDs));
        st.exchange(soci::use(strOfferIDs));
        st.exchange(soci::use(strSellingAssetTypes));
        st.exchange(soci::use(strSellingAssetCodes));
        st.exchange(soci::use(strSellingIssuers));
        st.exchange(soci::use(strBuyingAssetTypes));
        st.exchange(soci::use(strBuyingAssetCodes));
        st.exchange(soci::use(strBuyingIssuers));
        st.exchange(soci::use(strAmounts));
        st.exchange(soci::use(strPriceNs));
        st.exchange(soci::use(strPriceDs));
        st.exchange(soci::use(strPrices));
        st.exchange(soci::use(strFlags));
        st.exchange(soci::use(strLastModifieds));
        st.define_and_bind();
        {
            auto timer = mDB.getUpsertTimer("offer");
            st.execute(true);
        }
        if (st.get_affected_rows() != mOfferIDs.size())
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }
#endif
};

class BulkDeleteOffersOperation : public DatabaseTypeSpecificOperation
{
    Database& mDB;
    LedgerTxnConsistency mCons;
    std::vector<int64_t> mOfferIDs;

  public:
    BulkDeleteOffersOperation(Database& DB, LedgerTxnConsistency cons,
                              std::vector<EntryIterator> const& entries)
        : mDB(DB), mCons(cons)
    {
        for (auto const& e : entries)
        {
            assert(!e.entryExists());
            assert(e.key().type() == OFFER);
            auto const& offer = e.key().offer();
            mOfferIDs.emplace_back(offer.offerID);
        }
    }

    void
    doSociGenericOperation()
    {
        std::string sql = "DELETE FROM offers WHERE offerid = :id";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(mOfferIDs));
        st.define_and_bind();
        {
            auto timer = mDB.getDeleteTimer("offer");
            st.execute(true);
        }
        if (st.get_affected_rows() != mOfferIDs.size() &&
            mCons == LedgerTxnConsistency::EXACT)
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }

    void
    doSqliteSpecificOperation(soci::sqlite3_session_backend* sq) override
    {
        doSociGenericOperation();
    }

#ifdef USE_POSTGRES
    void
    doPostgresSpecificOperation(soci::postgresql_session_backend* pg) override
    {
        PGconn* conn = pg->conn_;
        std::string strOfferIDs;
        marshalToPGArray(conn, strOfferIDs, mOfferIDs);
        std::string sql = "WITH r AS (SELECT "
                          "unnest(:ids::BIGINT[]) "
                          ") "
                          "DELETE FROM offers WHERE "
                          "offerid IN (SELECT * FROM r)";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(strOfferIDs));
        st.define_and_bind();
        {
            auto timer = mDB.getDeleteTimer("offer");
            st.execute(true);
        }
        if (st.get_affected_rows() != mOfferIDs.size() &&
            mCons == LedgerTxnConsistency::EXACT)
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }
#endif
};

void
LedgerTxnRoot::Impl::bulkUpsertOffers(std::vector<EntryIterator> const& entries)
{
    BulkUpsertOffersOperation op(mDatabase, entries);
    mDatabase.doDatabaseTypeSpecificOperation(op);
}

void
LedgerTxnRoot::Impl::bulkDeleteOffers(std::vector<EntryIterator> const& entries,
                                      LedgerTxnConsistency cons)
{
    BulkDeleteOffersOperation op(mDatabase, cons, entries);
    mDatabase.doDatabaseTypeSpecificOperation(op);
}

void
LedgerTxnRoot::Impl::dropOffers()
{
    throwIfChild();
    mEntryCache.clear();
    mBestOffersCache.clear();

    mDatabase.getSession() << "DROP TABLE IF EXISTS offers;";
    mDatabase.getSession()
        << "CREATE TABLE offers"
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
    mDatabase.getSession()
        << "CREATE INDEX sellingissuerindex ON offers (sellingissuer);";
    mDatabase.getSession()
        << "CREATE INDEX buyingissuerindex ON offers (buyingissuer);";
    mDatabase.getSession() << "CREATE INDEX priceindex ON offers (price);";
}
}
