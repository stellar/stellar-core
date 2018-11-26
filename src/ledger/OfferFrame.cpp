// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/OfferFrame.h"
#include "LedgerDelta.h"
#include "crypto/KeyUtils.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "ledger/LedgerRange.h"
#include "ledger/TrustFrame.h"
#include "transactions/ManageOfferOpFrame.h"
#include "transactions/OfferExchange.h"
#include "util/types.h"

using namespace std;
using namespace soci;

namespace stellar
{
const char* OfferFrame::kSQLCreateStatement1 =
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

const char* OfferFrame::kSQLCreateStatement2 =
    "CREATE INDEX sellingissuerindex ON offers (sellingissuer);";

const char* OfferFrame::kSQLCreateStatement3 =
    "CREATE INDEX buyingissuerindex ON offers (buyingissuer);";

const char* OfferFrame::kSQLCreateStatement4 =
    "CREATE INDEX priceindex ON offers (price);";

static const char* offerColumnSelector =
    "SELECT sellerid,offerid,sellingassettype,sellingassetcode,sellingissuer,"
    "buyingassettype,buyingassetcode,buyingissuer,amount,pricen,priced,"
    "flags,lastmodified "
    "FROM offers";

int64_t
getSellingLiabilities(OfferEntry const& oe)
{
    auto res = exchangeV10WithoutPriceErrorThresholds(
        oe.price, oe.amount, INT64_MAX, INT64_MAX, INT64_MAX, false);
    return res.numWheatReceived;
}

int64_t
getBuyingLiabilities(OfferEntry const& oe)
{
    auto res = exchangeV10WithoutPriceErrorThresholds(
        oe.price, oe.amount, INT64_MAX, INT64_MAX, INT64_MAX, false);
    return res.numSheepSend;
}

OfferFrame::OfferFrame() : EntryFrame(OFFER), mOffer(mEntry.data.offer())
{
}

OfferFrame::OfferFrame(LedgerEntry const& from)
    : EntryFrame(from), mOffer(mEntry.data.offer())
{
}

OfferFrame::OfferFrame(OfferFrame const& from) : OfferFrame(from.mEntry)
{
}

OfferFrame&
OfferFrame::operator=(OfferFrame const& other)
{
    if (&other != this)
    {
        mOffer = other.mOffer;
        mKey = other.mKey;
        mKeyCalculated = other.mKeyCalculated;
    }
    return *this;
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

int64_t
OfferFrame::getSellingLiabilities() const
{
    return stellar::getSellingLiabilities(mOffer);
}

int64_t
OfferFrame::getBuyingLiabilities() const
{
    return stellar::getBuyingLiabilities(mOffer);
}

OfferFrame::pointer
OfferFrame::loadOffer(AccountID const& sellerID, uint64_t offerID, Database& db,
                      LedgerDelta* delta)
{
    OfferFrame::pointer retOffer;

    std::string actIDStrKey = KeyUtils::toStrKey(sellerID);

    std::string sql = offerColumnSelector;
    sql += " WHERE sellerid = :id AND offerid = :offerid";
    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(use(actIDStrKey));
    st.exchange(use(offerID));

    auto timer = db.getSelectTimer("offer");
    loadOffers(prep, [&retOffer](LedgerEntry const& offer) {
        retOffer = make_shared<OfferFrame>(offer);
    });

    if (delta && retOffer)
    {
        delta->recordEntry(*retOffer);
    }

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
    le.data.type(OFFER);
    OfferEntry& oe = le.data.offer();

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
    st.exchange(into(le.lastModifiedLedgerSeq));
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
    loadOffers(prep, [&retOffers](LedgerEntry const& of) {
        retOffers.emplace_back(make_shared<OfferFrame>(of));
    });
}

std::unordered_map<AccountID, std::vector<OfferFrame::pointer>>
OfferFrame::loadAllOffers(Database& db)
{
    std::unordered_map<AccountID, std::vector<OfferFrame::pointer>> retOffers;
    std::string sql = offerColumnSelector;
    sql += " ORDER BY sellerid";
    auto prep = db.getPreparedStatement(sql);

    auto timer = db.getSelectTimer("offer");
    loadOffers(prep, [&retOffers](LedgerEntry const& of) {
        auto& thisUserOffers = retOffers[of.data.offer().sellerID];
        thisUserOffers.emplace_back(make_shared<OfferFrame>(of));
    });
    return retOffers;
}

// Note: This function is currently only used in AllowTrustOpFrame, which means
// the asset parameter will never satisfy asset.type() == ASSET_TYPE_NATIVE. As
// a consequence, I have not implemented that possibility so this function
// throws in that case.
std::vector<OfferFrame::pointer>
OfferFrame::loadOffersByAccountAndAsset(AccountID const& accountID,
                                        Asset const& asset, Database& db)
{
    std::vector<OfferFrame::pointer> retOffers;
    std::string sql = offerColumnSelector;
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

    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(use(accountStr, "acc"));
    st.exchange(use(assetCode, "code"));
    st.exchange(use(assetIssuer, "iss"));

    auto timer = db.getSelectTimer("offer");
    loadOffers(prep, [&retOffers](LedgerEntry const& of) {
        retOffers.emplace_back(make_shared<OfferFrame>(of));
    });
    return retOffers;
}

bool
OfferFrame::exists(Database& db, LedgerKey const& key)
{
    std::string actIDStrKey = KeyUtils::toStrKey(key.offer().sellerID);
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

uint64_t
OfferFrame::countObjects(soci::session& sess, LedgerRange const& ledgers)
{
    uint64_t count = 0;
    sess << "SELECT COUNT(*) FROM offers"
            " WHERE lastmodified >= :v1 AND lastmodified <= :v2;",
        into(count), use(ledgers.first()), use(ledgers.last());
    return count;
}

void
OfferFrame::deleteOffersModifiedOnOrAfterLedger(Database& db,
                                                uint32_t oldestLedger)
{
    db.getEntryCache().erase_if(
        [oldestLedger](std::shared_ptr<LedgerEntry const> le) -> bool {
            return le && le->data.type() == OFFER &&
                   le->lastModifiedLedgerSeq >= oldestLedger;
        });

    {
        auto prep = db.getPreparedStatement(
            "DELETE FROM offers WHERE lastmodified >= :v1");
        auto& st = prep.statement();
        st.exchange(soci::use(oldestLedger));
        st.define_and_bind();
        st.execute(true);
    }
}

class offersAccumulator : public EntryFrame::Accumulator
{
  public:
    offersAccumulator(Database& db) : mDb(db)
    {
    }
    ~offersAccumulator()
    {
        vector<uint64> insertUpdateOfferIDs;
        vector<string> sellerIDs;
        vector<unsigned int> sellingassettypes;
        vector<string> sellingassetcodes;
        vector<soci::indicator> sellingassetcodeInds;
        vector<string> sellingissuers;
        vector<unsigned int> buyingassettypes;
        vector<string> buyingassetcodes;
        vector<soci::indicator> buyingassetcodeInds;
        vector<string> buyingissuers;
        vector<int64> amounts;
        vector<int32> pricens;
        vector<int32> priceds;
        vector<double> prices;
        vector<uint32> flagses;
        vector<uint32> lastmodifieds;

        vector<uint64> deleteOfferIDs;

        for (auto& it : mItems)
        {
            if (!it.second)
            {
                deleteOfferIDs.push_back(it.first);
                continue;
            }
            insertUpdateOfferIDs.push_back(it.first);
            sellerIDs.push_back(it.second->sellerid);
            sellingassettypes.push_back(it.second->sellingassettype);
            sellingassetcodes.push_back(it.second->sellingassetcode);
            sellingassetcodeInds.push_back(it.second->sellingassetcodeInd);
            sellingissuers.push_back(it.second->sellingissuer);
            buyingassettypes.push_back(it.second->buyingassettype);
            buyingassetcodes.push_back(it.second->buyingassetcode);
            buyingassetcodeInds.push_back(it.second->buyingassetcodeInd);
            buyingissuers.push_back(it.second->buyingissuer);
            amounts.push_back(it.second->amount);
            pricens.push_back(it.second->pricen);
            priceds.push_back(it.second->priced);
            prices.push_back(it.second->price);
            flagses.push_back(it.second->flags);
            lastmodifieds.push_back(it.second->lastmodified);
        }

        soci::session& session = mDb.getSession();
        auto pg = dynamic_cast<soci::postgresql_session_backend*>(session.get_backend());
        if (pg) {
          if (!insertUpdateOfferIDs.empty()) {
            static const char q[] = "WITH r AS ("
              "SELECT ...) "
              "INSERT INTO offers "
              "(offerid, sellerid, "
              "sellingassettype, sellingassetcode, sellingissuer, "
              "buyingassettype, buyingassetcode, buyingissuer, "
              "amount, pricen, priced, price, flags, lastmodified) "
              "SELECT oid, sid, sat, sac, si, bat, bac, bi, amt, pn, pd, p, flags, lastmod FROM r "
              "ON CONFLICT (offerid) DO UPDATE "
              "SET sellerid = r.sid, "
              "sellingassettype = r.sat, sellingassetcode = r.sac, "
              "sellingissuer = r.si, "
              "buyingassettype = r.bat, buyingassetcode = r.bac, "
              "buyingissuer = r.bi, "
              "amount = r.a, pricen = r.pn, priced = r.pd, price = r.p, "
              "flags = r.flags, lastmodified = r.lastmod";
          }
          if (!deleteOfferIDs.empty()) {
            static const char q[] = "DELETE FROM offers WHERE offerid = ANY($1::text[])";
            // xxx marshal args
            PGresult* res = PQexecParams(pg->conn_, q2, 1, 0, paramVals, 0, 0, 0); // xxx timer
            // xxx check res
          }

          return;
        }

        if (!insertUpdateOfferIDs.empty())
        {
            soci::statement st =
                session.prepare
                << "INSERT INTO offers "
                << "(offerid, sellerid, "
                << "sellingassettype, sellingassetcode, sellingissuer, "
                << "buyingassettype, buyingassetcode, buyingissuer, "
                << "amount, pricen, priced, price, flags, lastmodified) "
                << "VALUES (:oid, :sid, :sat, :sac, :si, :bat, :bac, :bi, "
                << ":a, :pn, :pd, :p, :flags, :lastmod) "
                << "ON CONFLICT (offerid) DO UPDATE "
                << "SET sellerid = :sid, "
                << "sellingassettype = :sat, sellingassetcode = :sac, "
                   "sellingissuer = :si, "
                << "buyingassettype = :bat, buyingassetcode = :bac, "
                   "buyingissuer = :bi, "
                << "amount = :a, pricen = :pn, priced = :pd, price = :p, "
                << "flags = :flags, lastmodified = :lastmod";
            st.exchange(use(insertUpdateOfferIDs, "oid"));
            st.exchange(use(sellerIDs, "sid"));
            st.exchange(use(sellingassettypes, "sat"));
            st.exchange(use(sellingassetcodes, sellingassetcodeInds, "sac"));
            st.exchange(use(sellingissuers, "si"));
            st.exchange(use(buyingassettypes, "bat"));
            st.exchange(use(buyingassetcodes, buyingassetcodeInds, "bac"));
            st.exchange(use(buyingissuers, "bi"));
            st.exchange(use(amounts, "a"));
            st.exchange(use(pricens, "pn"));
            st.exchange(use(priceds, "pd"));
            st.exchange(use(prices, "p"));
            st.exchange(use(flagses, "flags"));
            st.exchange(use(lastmodifieds, "lastmod"));
            st.define_and_bind();
            try
            {
                st.execute(true); // xxx timer
            }
            catch (const soci::soci_error& e)
            {
                cout << "xxx inserting into offers: " << e.what() << endl;
                throw;
            }
        }

        if (!deleteOfferIDs.empty())
        {
            try
            {
                session << "DELETE FROM offers WHERE offerid = :oid",
                    use(deleteOfferIDs, "oid");
            }
            catch (const soci::soci_error& e)
            {
                cout << "xxx deleting from offers: " << e.what() << endl;
            }
        }
    }

  protected:
    friend OfferFrame;

    Database& mDb;
    struct valType
    {
        string sellerid;
        unsigned int sellingassettype;
        string sellingassetcode;
        soci::indicator sellingassetcodeInd;
        string sellingissuer;
        soci::indicator sellingissuerInd;
        unsigned int buyingassettype;
        string buyingassetcode;
        soci::indicator buyingassetcodeInd;
        string buyingissuer;
        soci::indicator buyingissuerInd;
        int64 amount;
        int32 pricen;
        int32 priced;
        double price;
        uint32 flags;
        uint32 lastmodified;
    };
    map<uint64, unique_ptr<valType>> mItems;
};

unique_ptr<EntryFrame::Accumulator>
OfferFrame::createAccumulator(Database& db)
{
    return unique_ptr<EntryFrame::Accumulator>(new offersAccumulator(db));
}

void
OfferFrame::storeDelete(LedgerDelta& delta, Database& db,
                        EntryFrame::AccumulatorGroup* accums) const
{
    storeDelete(delta, db, getKey(), accums);
}

void
OfferFrame::storeDelete(LedgerDelta& delta, Database& db, LedgerKey const& key,
                        EntryFrame::AccumulatorGroup* accums)
{
    LedgerDelta::EntryDeleter entryDeleter(delta, key);

    if (accums)
    {
        offersAccumulator* offersAccum =
            dynamic_cast<offersAccumulator*>(accums->offersAccum());
        offersAccum->mItems[key.offer().offerID] =
            unique_ptr<offersAccumulator::valType>();
        return;
    }

    auto timer = db.getDeleteTimer("offer");
    auto prep = db.getPreparedStatement("DELETE FROM offers WHERE offerid=:s");
    auto& st = prep.statement();
    st.exchange(use(key.offer().offerID));
    st.define_and_bind();
    st.execute(true);
}

double
OfferFrame::computePrice() const
{
    return double(mOffer.price.n) / double(mOffer.price.d);
}

void
OfferFrame::storeAddOrChange(LedgerDelta& delta, Database& db,
                             EntryFrame::AccumulatorGroup* accums)
{
    LedgerDelta::EntryModder entryModder(delta, *this);

    touch(delta);

    std::string actIDStrKey = KeyUtils::toStrKey(mOffer.sellerID);

    unsigned int sellingType = mOffer.selling.type();
    unsigned int buyingType = mOffer.buying.type();
    std::string sellingIssuerStrKey, buyingIssuerStrKey;
    std::string sellingAssetCode, buyingAssetCode;
    soci::indicator selling_ind = soci::i_null, buying_ind = soci::i_null;

    if (sellingType == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        sellingIssuerStrKey =
            KeyUtils::toStrKey(mOffer.selling.alphaNum4().issuer);
        assetCodeToStr(mOffer.selling.alphaNum4().assetCode, sellingAssetCode);
        selling_ind = soci::i_ok;
    }
    else if (sellingType == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        sellingIssuerStrKey =
            KeyUtils::toStrKey(mOffer.selling.alphaNum12().issuer);
        assetCodeToStr(mOffer.selling.alphaNum12().assetCode, sellingAssetCode);
        selling_ind = soci::i_ok;
    }

    if (buyingType == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        buyingIssuerStrKey =
            KeyUtils::toStrKey(mOffer.buying.alphaNum4().issuer);
        assetCodeToStr(mOffer.buying.alphaNum4().assetCode, buyingAssetCode);
        buying_ind = soci::i_ok;
    }
    else if (buyingType == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        buyingIssuerStrKey =
            KeyUtils::toStrKey(mOffer.buying.alphaNum12().issuer);
        assetCodeToStr(mOffer.buying.alphaNum12().assetCode, buyingAssetCode);
        buying_ind = soci::i_ok;
    }

    if (accums)
    {
        auto val = make_unique<offersAccumulator::valType>();
        val->sellerid = actIDStrKey;
        val->sellingassettype = sellingType;
        val->sellingassetcode = sellingAssetCode;
        val->sellingassetcodeInd = selling_ind;
        val->sellingissuer = sellingIssuerStrKey;
        val->sellingissuerInd = selling_ind;
        val->buyingassettype = buyingType;
        val->buyingassetcode = buyingAssetCode;
        val->buyingassetcodeInd = buying_ind;
        val->buyingissuer = buyingIssuerStrKey;
        val->buyingissuerInd = buying_ind;
        val->amount = mOffer.amount;
        val->pricen = mOffer.price.n;
        val->priced = mOffer.price.d;
        val->price = computePrice();
        val->flags = mOffer.flags;
        val->lastmodified = getLastModified();

        offersAccumulator* offersAccum =
            dynamic_cast<offersAccumulator*>(accums->offersAccum());
        offersAccum->mItems[mOffer.offerID] = move(val);
        return;
    }

    string sql =
        ("INSERT INTO offers (offerid, sellerid, "
         "sellingassettype, sellingassetcode, sellingissuer, "
         "buyingassettype, buyingassetcode, buyingissuer, "
         "amount, pricen, priced, price, flags, lastmodified) "
         "VALUES (:oid, :sid, :sat, :sac, :si, "
         ":bat, :bac, :bi, :a, :pn, :pd, :p, :f, :l) "
         "ON CONFLICT (offerid) DO UPDATE "
         "SET sellerid = :sid, " // xxx storeUpdateHelper omitted this, omit
                                 // here too?
         "sellingassettype = :sat, "
         "sellingassetcode = :sac, sellingissuer = :si, "
         "buyingassettype = :bat, buyingassetcode = :bac, buyingissuer = :bi, "
         "amount = :a, pricen = :pn, priced = :pd, price = :p, flags = :f, "
         "lastmodified = :l");

    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();

    st.exchange(use(mOffer.offerID, "oid"));
    st.exchange(use(actIDStrKey, "sid"));
    st.exchange(use(sellingType, "sat"));
    st.exchange(use(sellingAssetCode, selling_ind, "sac"));
    st.exchange(use(sellingIssuerStrKey, selling_ind, "si"));
    st.exchange(use(buyingType, "bat"));
    st.exchange(use(buyingAssetCode, buying_ind, "bac"));
    st.exchange(use(buyingIssuerStrKey, buying_ind, "bi"));
    st.exchange(use(mOffer.amount, "a"));
    st.exchange(use(mOffer.price.n, "pn"));
    st.exchange(use(mOffer.price.d, "pd"));
    auto price = computePrice();
    st.exchange(use(price, "p"));
    st.exchange(use(mOffer.flags, "f"));
    st.exchange(use(getLastModified(), "l"));
    st.define_and_bind();

    st.execute(true); // xxx timer

    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("could not update SQL");
    }
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

void
OfferFrame::releaseLiabilities(AccountFrame::pointer const& account,
                               TrustFrame::pointer const& buyingTrust,
                               TrustFrame::pointer const& sellingTrust,
                               LedgerDelta& delta, Database& db,
                               LedgerManager& ledgerManager)
{
    acquireOrReleaseLiabilities(false, account, buyingTrust, sellingTrust,
                                delta, db, ledgerManager);
}

void
OfferFrame::acquireLiabilities(AccountFrame::pointer const& account,
                               TrustFrame::pointer const& buyingTrust,
                               TrustFrame::pointer const& sellingTrust,
                               LedgerDelta& delta, Database& db,
                               LedgerManager& ledgerManager)
{
    acquireOrReleaseLiabilities(true, account, buyingTrust, sellingTrust, delta,
                                db, ledgerManager);
}

void
OfferFrame::acquireOrReleaseLiabilities(bool isAcquire,
                                        AccountFrame::pointer const& account,
                                        TrustFrame::pointer const& buyingTrust,
                                        TrustFrame::pointer const& sellingTrust,
                                        LedgerDelta& delta, Database& db,
                                        LedgerManager& ledgerManager)
{
    // This should never happen
    if (getBuying() == getSelling())
    {
        throw std::runtime_error("buying and selling same asset");
    }

    auto loadAccountIfNecessaryAndValidate = [this, &account, &delta, &db]() {
        AccountFrame::pointer acc = account;
        if (!acc)
        {
            acc = AccountFrame::loadAccount(delta, getSellerID(), db);
            assert(acc);
        }
        assert(acc->getID() == getSellerID());
        return acc;
    };

    auto loadTrustIfNecessaryAndValidate = [this, &delta, &db](
                                               TrustFrame::pointer const& trust,
                                               Asset const& asset) {
        TrustFrame::pointer tf = trust;
        if (!tf)
        {
            tf = TrustFrame::loadTrustLine(getSellerID(), asset, db, &delta);
            assert(tf);
        }
        assert(tf->getTrustLine().accountID == getSellerID());
        assert(tf->getTrustLine().asset == asset);
        return tf;
    };

    int64_t buyingLiabilities =
        isAcquire ? getBuyingLiabilities() : -getBuyingLiabilities();
    Asset const& buyingAsset = getBuying();
    if (buyingAsset.type() == ASSET_TYPE_NATIVE)
    {
        auto acc = loadAccountIfNecessaryAndValidate();
        bool res = acc->addBuyingLiabilities(buyingLiabilities, ledgerManager);
        if (!res)
        {
            throw std::runtime_error("could not add buying liabilities");
        }
        acc->storeChange(delta, db);
    }
    else
    {
        auto trust = loadTrustIfNecessaryAndValidate(buyingTrust, buyingAsset);
        bool res =
            trust->addBuyingLiabilities(buyingLiabilities, ledgerManager);
        if (!res)
        {
            throw std::runtime_error("could not add buying liabilities");
        }
        trust->storeChange(delta, db);
    }

    int64_t sellingLiabilities =
        isAcquire ? getSellingLiabilities() : -getSellingLiabilities();
    Asset const& sellingAsset = getSelling();
    if (sellingAsset.type() == ASSET_TYPE_NATIVE)
    {
        auto acc = loadAccountIfNecessaryAndValidate();
        bool res =
            acc->addSellingLiabilities(sellingLiabilities, ledgerManager);
        if (!res)
        {
            throw std::runtime_error("could not add selling liabilities");
        }
        acc->storeChange(delta, db);
    }
    else
    {
        auto trust =
            loadTrustIfNecessaryAndValidate(sellingTrust, sellingAsset);
        bool res =
            trust->addSellingLiabilities(sellingLiabilities, ledgerManager);
        if (!res)
        {
            throw std::runtime_error("could not add selling liabilities");
        }
        trust->storeChange(delta, db);
    }
}
}