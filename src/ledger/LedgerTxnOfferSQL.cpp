// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "database/DatabaseTypeSpecificOperation.h"
#include "ledger/LedgerTxnImpl.h"
#include "transactions/TransactionUtils.h"
#include "util/Decoder.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "util/types.h"
#include "xdrpp/marshal.h"
#include <Tracy.hpp>

namespace stellar
{

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::loadOffer(LedgerKey const& key) const
{
    ZoneScoped;
    int64_t offerID = key.offer().offerID;
    if (offerID < 0)
    {
        return nullptr;
    }

    std::string actIDStrKey = KeyUtils::toStrKey(key.offer().sellerID);

    std::string sql = "SELECT sellerid, offerid, sellingasset, buyingasset, "
                      "amount, pricen, priced, flags, lastmodified, extension, "
                      "ledgerext "
                      "FROM offers "
                      "WHERE sellerid= :id AND offerid= :offerid";
    auto prep = mDatabase.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey));
    st.exchange(soci::use(offerID));

    std::vector<LedgerEntry> offers;
    {
        auto timer = mDatabase.getSelectTimer("offer");
        offers = loadOffers(prep);
    }

    return offers.empty() ? nullptr
                          : std::make_shared<LedgerEntry const>(offers.front());
}

std::vector<LedgerEntry>
LedgerTxnRoot::Impl::loadAllOffers() const
{
    ZoneScoped;
    std::string sql = "SELECT sellerid, offerid, sellingasset, buyingasset, "
                      "amount, pricen, priced, flags, lastmodified, extension, "
                      "ledgerext FROM offers";
    auto prep = mDatabase.getPreparedStatement(sql);

    std::vector<LedgerEntry> offers;
    {
        auto timer = mDatabase.getSelectTimer("offer");
        offers = loadOffers(prep);
    }
    return offers;
}

std::deque<LedgerEntry>::const_iterator
LedgerTxnRoot::Impl::loadBestOffers(std::deque<LedgerEntry>& offers,
                                    Asset const& buying, Asset const& selling,
                                    size_t numOffers) const
{
    ZoneScoped;
    // price is an approximation of the actual n/d (truncated math, 15 digits)
    // ordering by offerid gives precendence to older offers for fairness
    std::string sql = "SELECT sellerid, offerid, sellingasset, buyingasset, "
                      "amount, pricen, priced, flags, lastmodified, extension, "
                      "ledgerext FROM offers "
                      "WHERE sellingasset = :v1 AND buyingasset = :v2 "
                      "ORDER BY price, offerid LIMIT :n";

    std::string buyingAsset, sellingAsset;
    buyingAsset = decoder::encode_b64(xdr::xdr_to_opaque(buying));
    sellingAsset = decoder::encode_b64(xdr::xdr_to_opaque(selling));

    auto prep = mDatabase.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::use(sellingAsset));
    st.exchange(soci::use(buyingAsset));
    st.exchange(soci::use(numOffers));

    {
        auto timer = mDatabase.getSelectTimer("offer");
        return loadOffers(prep, offers);
    }
}

std::deque<LedgerEntry>::const_iterator
LedgerTxnRoot::Impl::loadBestOffers(std::deque<LedgerEntry>& offers,
                                    Asset const& buying, Asset const& selling,
                                    OfferDescriptor const& worseThan,
                                    size_t numOffers) const
{
    ZoneScoped;
    // ManageOffer and related operations won't work correctly with an offerID
    // equal to or exceeding INT64_MAX, so there is no reason to support it
    // here. We are far from this limit anyway.
    if (worseThan.offerID == INT64_MAX)
    {
        throw std::runtime_error("maximum offerID encountered");
    }

    // price is an approximation of the actual n/d (truncated math, 15 digits)
    // ordering by offerid gives precendence to older offers for fairness
    std::string sql =
        "WITH r1 AS "
        "(SELECT sellerid, offerid, sellingasset, buyingasset, amount, price, "
        "pricen, priced, flags, lastmodified, extension, "
        "ledgerext FROM offers "
        "WHERE sellingasset = :v1 AND buyingasset = :v2 AND price > :v3 "
        "ORDER BY price, offerid LIMIT :v4), "
        "r2 AS "
        "(SELECT sellerid, offerid, sellingasset, buyingasset, amount, price, "
        "pricen, priced, flags, lastmodified, extension, "
        "ledgerext FROM offers "
        "WHERE sellingasset = :v5 AND buyingasset = :v6 AND price = :v7 "
        "AND offerid >= :v8 ORDER BY price, offerid LIMIT :v9) "
        "SELECT sellerid, offerid, sellingasset, buyingasset, "
        "amount, pricen, priced, flags, lastmodified, extension, "
        "ledgerext "
        "FROM (SELECT * FROM r1 UNION ALL SELECT * FROM r2) AS res "
        "ORDER BY price, offerid LIMIT :v10";

    std::string buyingAsset, sellingAsset;
    buyingAsset = decoder::encode_b64(xdr::xdr_to_opaque(buying));
    sellingAsset = decoder::encode_b64(xdr::xdr_to_opaque(selling));

    double worseThanPrice =
        (double)worseThan.price.n / (double)worseThan.price.d;
    int64_t worseThanOfferID = worseThan.offerID + 1;

    auto prep = mDatabase.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::use(sellingAsset));
    st.exchange(soci::use(buyingAsset));
    st.exchange(soci::use(worseThanPrice));
    st.exchange(soci::use(numOffers));
    st.exchange(soci::use(sellingAsset));
    st.exchange(soci::use(buyingAsset));
    st.exchange(soci::use(worseThanPrice));
    st.exchange(soci::use(worseThanOfferID));
    st.exchange(soci::use(numOffers));
    st.exchange(soci::use(numOffers));

    {
        auto timer = mDatabase.getSelectTimer("offer");
        return loadOffers(prep, offers);
    }
}

bool
isBetterOffer(OfferDescriptor const& lhs, OfferDescriptor const& rhs)
{
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

bool
isBetterOffer(OfferDescriptor const& lhs, LedgerEntry const& rhsEntry)
{
    auto const& rhs = rhsEntry.data.offer();
    return isBetterOffer(lhs, {rhs.price, rhs.offerID});
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

    return isBetterOffer({lhs.price, lhs.offerID}, {rhs.price, rhs.offerID});
}

// Note: This function is currently only used in AllowTrustOpFrame, which means
// the asset parameter will never satisfy asset.type() == ASSET_TYPE_NATIVE. As
// a consequence, this function throws in that case.
std::vector<LedgerEntry>
LedgerTxnRoot::Impl::loadOffersByAccountAndAsset(AccountID const& accountID,
                                                 Asset const& asset) const
{
    ZoneScoped;
    std::string sql = "SELECT sellerid, offerid, sellingasset, buyingasset, "
                      "amount, pricen, priced, flags, lastmodified, extension, "
                      "ledgerext "
                      "FROM offers WHERE sellerid = :v1 AND "
                      "(sellingasset = :v2 OR buyingasset = :v3)";
    // Note: v2 == v3 but positional parameters are faster

    std::string accountStr = KeyUtils::toStrKey(accountID);

    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        throw std::runtime_error("Invalid asset type");
    }
    std::string assetStr = decoder::encode_b64(xdr::xdr_to_opaque(asset));

    auto prep = mDatabase.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::use(accountStr));
    st.exchange(soci::use(assetStr));
    st.exchange(soci::use(assetStr));

    std::vector<LedgerEntry> offers;
    {
        auto timer = mDatabase.getSelectTimer("offer");
        offers = loadOffers(prep);
    }
    return offers;
}

static Asset
processAsset(std::string const& asset)
{
    Asset res;
    std::vector<uint8_t> assetOpaque;
    decoder::decode_b64(asset, assetOpaque);
    xdr::xdr_from_opaque(assetOpaque, res);
    return res;
}

std::deque<LedgerEntry>::const_iterator
LedgerTxnRoot::Impl::loadOffers(StatementContext& prep,
                                std::deque<LedgerEntry>& offers) const
{
    ZoneScoped;
    std::string actIDStrKey;
    std::string sellingAsset, buyingAsset;
    std::string extensionStr;
    soci::indicator extensionInd;
    std::string ledgerExtStr;
    soci::indicator ledgerExtInd;

    LedgerEntry le;
    le.data.type(OFFER);
    OfferEntry& oe = le.data.offer();

    auto& st = prep.statement();
    st.exchange(soci::into(actIDStrKey));
    st.exchange(soci::into(oe.offerID));
    st.exchange(soci::into(sellingAsset));
    st.exchange(soci::into(buyingAsset));
    st.exchange(soci::into(oe.amount));
    st.exchange(soci::into(oe.price.n));
    st.exchange(soci::into(oe.price.d));
    st.exchange(soci::into(oe.flags));
    st.exchange(soci::into(le.lastModifiedLedgerSeq));
    st.exchange(soci::into(extensionStr, extensionInd));
    st.exchange(soci::into(ledgerExtStr, ledgerExtInd));
    st.define_and_bind();
    st.execute(true);

    size_t n = 0;
    while (st.got_data())
    {
        ++n;
        oe.sellerID = KeyUtils::fromStrKey<PublicKey>(actIDStrKey);
        oe.selling = processAsset(sellingAsset);
        oe.buying = processAsset(buyingAsset);

        decodeOpaqueXDR(extensionStr, extensionInd, oe.ext);

        decodeOpaqueXDR(ledgerExtStr, ledgerExtInd, le.ext);

        offers.emplace_back(le);
        st.fetch();
    }

    return offers.cend() - n;
}

std::vector<LedgerEntry>
LedgerTxnRoot::Impl::loadOffers(StatementContext& prep) const
{
    ZoneScoped;
    std::vector<LedgerEntry> offers;

    std::string actIDStrKey;
    std::string sellingAsset, buyingAsset;
    std::string extensionStr;
    soci::indicator extensionInd;
    std::string ledgerExtStr;
    soci::indicator ledgerExtInd;

    LedgerEntry le;
    le.data.type(OFFER);
    OfferEntry& oe = le.data.offer();

    auto& st = prep.statement();
    st.exchange(soci::into(actIDStrKey));
    st.exchange(soci::into(oe.offerID));
    st.exchange(soci::into(sellingAsset));
    st.exchange(soci::into(buyingAsset));
    st.exchange(soci::into(oe.amount));
    st.exchange(soci::into(oe.price.n));
    st.exchange(soci::into(oe.price.d));
    st.exchange(soci::into(oe.flags));
    st.exchange(soci::into(le.lastModifiedLedgerSeq));
    st.exchange(soci::into(extensionStr, extensionInd));
    st.exchange(soci::into(ledgerExtStr, ledgerExtInd));
    st.define_and_bind();
    st.execute(true);

    while (st.got_data())
    {
        oe.sellerID = KeyUtils::fromStrKey<PublicKey>(actIDStrKey);
        oe.selling = processAsset(sellingAsset);
        oe.buying = processAsset(buyingAsset);

        decodeOpaqueXDR(extensionStr, extensionInd, oe.ext);

        decodeOpaqueXDR(ledgerExtStr, ledgerExtInd, le.ext);

        offers.emplace_back(le);
        st.fetch();
    }

    return offers;
}

class BulkUpsertOffersOperation : public DatabaseTypeSpecificOperation<void>
{
    Database& mDB;
    std::vector<std::string> mSellerIDs;
    std::vector<int64_t> mOfferIDs;
    std::vector<std::string> mSellingAssets;
    std::vector<std::string> mBuyingAssets;
    std::vector<int64_t> mAmounts;
    std::vector<int32_t> mPriceNs;
    std::vector<int32_t> mPriceDs;
    std::vector<double> mPrices;
    std::vector<int32_t> mFlags;
    std::vector<int32_t> mLastModifieds;
    std::vector<std::string> mExtensions;
    std::vector<std::string> mLedgerExtensions;

    void
    accumulateEntry(LedgerEntry const& entry)
    {
        assert(entry.data.type() == OFFER);
        auto const& offer = entry.data.offer();

        mSellerIDs.emplace_back(KeyUtils::toStrKey(offer.sellerID));
        mOfferIDs.emplace_back(offer.offerID);

        mSellingAssets.emplace_back(
            decoder::encode_b64(xdr::xdr_to_opaque(offer.selling)));
        mBuyingAssets.emplace_back(
            decoder::encode_b64(xdr::xdr_to_opaque(offer.buying)));

        mAmounts.emplace_back(offer.amount);
        mPriceNs.emplace_back(offer.price.n);
        mPriceDs.emplace_back(offer.price.d);
        double price = double(offer.price.n) / double(offer.price.d);
        mPrices.emplace_back(price);

        mFlags.emplace_back(unsignedToSigned(offer.flags));
        mLastModifieds.emplace_back(
            unsignedToSigned(entry.lastModifiedLedgerSeq));
        mExtensions.emplace_back(
            decoder::encode_b64(xdr::xdr_to_opaque(offer.ext)));
        mLedgerExtensions.emplace_back(
            decoder::encode_b64(xdr::xdr_to_opaque(entry.ext)));
    }

  public:
    BulkUpsertOffersOperation(Database& DB,
                              std::vector<LedgerEntry> const& entries)
        : mDB(DB)
    {
        mSellerIDs.reserve(entries.size());
        mOfferIDs.reserve(entries.size());
        mSellingAssets.reserve(entries.size());
        mBuyingAssets.reserve(entries.size());
        mAmounts.reserve(entries.size());
        mPriceNs.reserve(entries.size());
        mPriceDs.reserve(entries.size());
        mPrices.reserve(entries.size());
        mFlags.reserve(entries.size());
        mLastModifieds.reserve(entries.size());
        mExtensions.reserve(entries.size());
        mLedgerExtensions.reserve(entries.size());

        for (auto const& e : entries)
        {
            accumulateEntry(e);
        }
    }

    BulkUpsertOffersOperation(Database& DB,
                              std::vector<EntryIterator> const& entries)
        : mDB(DB)
    {
        mSellerIDs.reserve(entries.size());
        mOfferIDs.reserve(entries.size());
        mSellingAssets.reserve(entries.size());
        mBuyingAssets.reserve(entries.size());
        mAmounts.reserve(entries.size());
        mPriceNs.reserve(entries.size());
        mPriceDs.reserve(entries.size());
        mPrices.reserve(entries.size());
        mFlags.reserve(entries.size());
        mLastModifieds.reserve(entries.size());
        mExtensions.reserve(entries.size());
        mLedgerExtensions.reserve(entries.size());

        for (auto const& e : entries)
        {
            assert(e.entryExists());
            assert(e.entry().type() == InternalLedgerEntryType::LEDGER_ENTRY);
            accumulateEntry(e.entry().ledgerEntry());
        }
    }

    void
    doSociGenericOperation()
    {
        std::string sql =
            "INSERT INTO offers ( "
            "sellerid, offerid, sellingasset, buyingasset, "
            "amount, pricen, priced, price, flags, lastmodified, extension, "
            "ledgerext "
            ") VALUES ( "
            ":v1, :v2, :v3, :v4, :v5, :v6, :v7, :v8, :v9, :v10, :v11, :v12 "
            ") ON CONFLICT (offerid) DO UPDATE SET "
            "sellerid = excluded.sellerid, "
            "sellingasset = excluded.sellingasset, "
            "buyingasset = excluded.buyingasset, "
            "amount = excluded.amount, "
            "pricen = excluded.pricen, "
            "priced = excluded.priced, "
            "price = excluded.price, "
            "flags = excluded.flags, "
            "lastmodified = excluded.lastmodified, "
            "extension = excluded.extension, "
            "ledgerext = excluded.ledgerext";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(mSellerIDs));
        st.exchange(soci::use(mOfferIDs));
        st.exchange(soci::use(mSellingAssets));
        st.exchange(soci::use(mBuyingAssets));
        st.exchange(soci::use(mAmounts));
        st.exchange(soci::use(mPriceNs));
        st.exchange(soci::use(mPriceDs));
        st.exchange(soci::use(mPrices));
        st.exchange(soci::use(mFlags));
        st.exchange(soci::use(mLastModifieds));
        st.exchange(soci::use(mExtensions));
        st.exchange(soci::use(mLedgerExtensions));
        st.define_and_bind();
        {
            auto timer = mDB.getUpsertTimer("offer");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mOfferIDs.size())
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

        std::string strSellerIDs, strOfferIDs, strSellingAssets,
            strBuyingAssets, strAmounts, strPriceNs, strPriceDs, strPrices,
            strFlags, strLastModifieds, strExtensions, strLedgerExtensions;

        PGconn* conn = pg->conn_;
        marshalToPGArray(conn, strSellerIDs, mSellerIDs);
        marshalToPGArray(conn, strOfferIDs, mOfferIDs);

        marshalToPGArray(conn, strSellingAssets, mSellingAssets);
        marshalToPGArray(conn, strBuyingAssets, mBuyingAssets);

        marshalToPGArray(conn, strAmounts, mAmounts);
        marshalToPGArray(conn, strPriceNs, mPriceNs);
        marshalToPGArray(conn, strPriceDs, mPriceDs);
        marshalToPGArray(conn, strPrices, mPrices);
        marshalToPGArray(conn, strFlags, mFlags);
        marshalToPGArray(conn, strLastModifieds, mLastModifieds);
        marshalToPGArray(conn, strExtensions, mExtensions);
        marshalToPGArray(conn, strLedgerExtensions, mLedgerExtensions);

        std::string sql =
            "WITH r AS (SELECT "
            "unnest(:v1::TEXT[]), "
            "unnest(:v2::BIGINT[]), "
            "unnest(:v3::TEXT[]), "
            "unnest(:v4::TEXT[]), "
            "unnest(:v5::BIGINT[]), "
            "unnest(:v6::INT[]), "
            "unnest(:v7::INT[]), "
            "unnest(:v8::DOUBLE PRECISION[]), "
            "unnest(:v9::INT[]), "
            "unnest(:v10::INT[]), "
            "unnest(:v11::TEXT[]), "
            "unnest(:v12::TEXT[]) "
            ")"
            "INSERT INTO offers ( "
            "sellerid, offerid, sellingasset, buyingasset, "
            "amount, pricen, priced, price, flags, lastmodified, extension, "
            "ledgerext "
            ") SELECT * from r "
            "ON CONFLICT (offerid) DO UPDATE SET "
            "sellerid = excluded.sellerid, "
            "sellingasset = excluded.sellingasset, "
            "buyingasset = excluded.buyingasset, "
            "amount = excluded.amount, "
            "pricen = excluded.pricen, "
            "priced = excluded.priced, "
            "price = excluded.price, "
            "flags = excluded.flags, "
            "lastmodified = excluded.lastmodified, "
            "extension = excluded.extension, "
            "ledgerext = excluded.ledgerext";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(strSellerIDs));
        st.exchange(soci::use(strOfferIDs));
        st.exchange(soci::use(strSellingAssets));
        st.exchange(soci::use(strBuyingAssets));
        st.exchange(soci::use(strAmounts));
        st.exchange(soci::use(strPriceNs));
        st.exchange(soci::use(strPriceDs));
        st.exchange(soci::use(strPrices));
        st.exchange(soci::use(strFlags));
        st.exchange(soci::use(strLastModifieds));
        st.exchange(soci::use(strExtensions));
        st.exchange(soci::use(strLedgerExtensions));
        st.define_and_bind();
        {
            auto timer = mDB.getUpsertTimer("offer");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mOfferIDs.size())
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }
#endif
};

class BulkDeleteOffersOperation : public DatabaseTypeSpecificOperation<void>
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
            assert(e.key().type() == InternalLedgerEntryType::LEDGER_ENTRY);
            assert(e.key().ledgerKey().type() == OFFER);
            auto const& offer = e.key().ledgerKey().offer();
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
        if (static_cast<size_t>(st.get_affected_rows()) != mOfferIDs.size() &&
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
        if (static_cast<size_t>(st.get_affected_rows()) != mOfferIDs.size() &&
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
    ZoneScoped;
    ZoneValue(static_cast<int64_t>(entries.size()));
    BulkUpsertOffersOperation op(mDatabase, entries);
    mDatabase.doDatabaseTypeSpecificOperation(op);
}

void
LedgerTxnRoot::Impl::bulkDeleteOffers(std::vector<EntryIterator> const& entries,
                                      LedgerTxnConsistency cons)
{
    ZoneScoped;
    ZoneValue(static_cast<int64_t>(entries.size()));
    BulkDeleteOffersOperation op(mDatabase, cons, entries);
    mDatabase.doDatabaseTypeSpecificOperation(op);
}

void
LedgerTxnRoot::Impl::dropOffers()
{
    throwIfChild();
    mEntryCache.clear();
    mBestOffers.clear();

    std::string coll = mDatabase.getSimpleCollationClause();

    mDatabase.getSession() << "DROP TABLE IF EXISTS offers;";
    mDatabase.getSession()
        << "CREATE TABLE offers"
        << "("
        << "sellerid         VARCHAR(56) " << coll << "NOT NULL,"
        << "offerid          BIGINT           NOT NULL CHECK (offerid >= 0),"
        << "sellingasset     TEXT " << coll << " NOT NULL,"
        << "buyingasset      TEXT " << coll << " NOT NULL,"
        << "amount           BIGINT           NOT NULL CHECK (amount >= 0),"
           "pricen           INT              NOT NULL,"
           "priced           INT              NOT NULL,"
           "price            DOUBLE PRECISION NOT NULL,"
           "flags            INT              NOT NULL,"
           "lastmodified     INT              NOT NULL,"
           "PRIMARY KEY      (offerid)"
           ");";
    mDatabase.getSession() << "CREATE INDEX bestofferindex ON offers "
                              "(sellingasset,buyingasset,price,offerid);";
    if (!mDatabase.isSqlite())
    {
        mDatabase.getSession() << "ALTER TABLE offers "
                               << "ALTER COLUMN sellerid "
                               << "TYPE VARCHAR(56) COLLATE \"C\", "
                               << "ALTER COLUMN buyingasset "
                               << "TYPE TEXT COLLATE \"C\", "
                               << "ALTER COLUMN sellingasset "
                               << "TYPE TEXT COLLATE \"C\"";
    }
}

class BulkLoadOffersOperation
    : public DatabaseTypeSpecificOperation<std::vector<LedgerEntry>>
{
    Database& mDb;
    std::vector<int64_t> mOfferIDs;
    UnorderedSet<LedgerKey> mKeys;

    std::vector<LedgerEntry>
    executeAndFetch(soci::statement& st)
    {
        std::string sellerID, sellingAsset, buyingAsset;
        int64_t amount;
        int64_t offerID;
        uint32_t flags, lastModified;
        std::string extension;
        soci::indicator extensionInd;
        std::string ledgerExtension;
        soci::indicator ledgerExtInd;
        Price price;

        st.exchange(soci::into(sellerID));
        st.exchange(soci::into(offerID));
        st.exchange(soci::into(sellingAsset));
        st.exchange(soci::into(buyingAsset));
        st.exchange(soci::into(amount));
        st.exchange(soci::into(price.n));
        st.exchange(soci::into(price.d));
        st.exchange(soci::into(flags));
        st.exchange(soci::into(lastModified));
        st.exchange(soci::into(extension, extensionInd));
        st.exchange(soci::into(ledgerExtension, ledgerExtInd));
        st.define_and_bind();
        {
            auto timer = mDb.getSelectTimer("offer");
            st.execute(true);
        }

        std::vector<LedgerEntry> res;
        while (st.got_data())
        {
            auto pubKey = KeyUtils::fromStrKey<PublicKey>(sellerID);

            res.emplace_back();
            auto& le = res.back();
            le.data.type(OFFER);
            auto& oe = le.data.offer();

            oe.sellerID = pubKey;
            oe.offerID = offerID;

            oe.selling = processAsset(sellingAsset);
            oe.buying = processAsset(buyingAsset);

            oe.amount = amount;
            oe.price = price;
            oe.flags = flags;
            le.lastModifiedLedgerSeq = lastModified;

            decodeOpaqueXDR(extension, extensionInd, oe.ext);

            decodeOpaqueXDR(ledgerExtension, ledgerExtInd, le.ext);

            st.fetch();
        }
        return res;
    }

  public:
    BulkLoadOffersOperation(Database& db, UnorderedSet<LedgerKey> const& keys)
        : mDb(db)
    {
        mOfferIDs.reserve(keys.size());
        for (auto const& k : keys)
        {
            assert(k.type() == OFFER);
            if (k.offer().offerID >= 0)
            {
                mOfferIDs.emplace_back(k.offer().offerID);
            }
        }
    }

    virtual std::vector<LedgerEntry>
    doSqliteSpecificOperation(soci::sqlite3_session_backend* sq) override
    {
        std::string sql =
            "SELECT sellerid, offerid, sellingasset, buyingasset, "
            "amount, pricen, priced, flags, lastmodified, extension, "
            "ledgerext "
            "FROM offers WHERE offerid IN carray(?, ?, 'int64')";

        auto prep = mDb.getPreparedStatement(sql);
        auto be = prep.statement().get_backend();
        if (be == nullptr)
        {
            throw std::runtime_error("no sql backend");
        }
        auto sqliteStatement =
            dynamic_cast<soci::sqlite3_statement_backend*>(be);
        auto st = sqliteStatement->stmt_;

        sqlite3_reset(st);
        sqlite3_bind_pointer(st, 1, (void*)mOfferIDs.data(), "carray", 0);
        sqlite3_bind_int(st, 2, static_cast<int>(mOfferIDs.size()));
        return executeAndFetch(prep.statement());
    }

#ifdef USE_POSTGRES
    std::vector<LedgerEntry>
    doPostgresSpecificOperation(soci::postgresql_session_backend* pg) override
    {
        std::string strOfferIDs;
        marshalToPGArray(pg->conn_, strOfferIDs, mOfferIDs);

        std::string sql =
            "WITH r AS (SELECT unnest(:v1::BIGINT[])) "
            "SELECT sellerid, offerid, sellingasset, buyingasset, "
            "amount, pricen, priced, flags, lastmodified, extension, "
            "ledgerext "
            "FROM offers WHERE offerid IN (SELECT * FROM r)";
        auto prep = mDb.getPreparedStatement(sql);
        auto& st = prep.statement();
        st.exchange(soci::use(strOfferIDs));
        return executeAndFetch(st);
    }
#endif
};

UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>>
LedgerTxnRoot::Impl::bulkLoadOffers(UnorderedSet<LedgerKey> const& keys) const
{
    ZoneScoped;
    ZoneValue(static_cast<int64_t>(keys.size()));
    if (!keys.empty())
    {
        BulkLoadOffersOperation op(mDatabase, keys);
        return populateLoadedEntries(
            keys, mDatabase.doDatabaseTypeSpecificOperation(op));
    }
    else
    {
        return {};
    }
}
}
