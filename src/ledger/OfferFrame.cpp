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
#include "transactions/ManageOfferOpFrame.h"
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

double
OfferFrame::computePrice() const
{
    return double(mOffer.price.n) / double(mOffer.price.d);
}

void
OfferFrame::storeChange(LedgerDelta& delta, Database& db)
{
    storeUpdateHelper(delta, db, false);
}

void
OfferFrame::storeAdd(LedgerDelta& delta, Database& db)
{
    storeUpdateHelper(delta, db, true);
}

void
OfferFrame::storeUpdateHelper(LedgerDelta& delta, Database& db, bool insert)
{
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

    string sql;

    if (insert)
    {
        sql = "INSERT INTO offers (sellerid,offerid,"
              "sellingassettype,sellingassetcode,sellingissuer,"
              "buyingassettype,buyingassetcode,buyingissuer,"
              "amount,pricen,priced,price,flags,lastmodified) VALUES "
              "(:sid,:oid,:sat,:sac,:si,:bat,:bac,:bi,:a,:pn,:pd,:p,:f,:l)";
    }
    else
    {
        sql = "UPDATE offers SET sellingassettype=:sat "
              ",sellingassetcode=:sac,sellingissuer=:si,"
              "buyingassettype=:bat,buyingassetcode=:bac,buyingissuer=:bi,"
              "amount=:a,pricen=:pn,priced=:pd,price=:p,flags=:f,"
              "lastmodified=:l WHERE offerid=:oid";
    }

    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();

    if (insert)
    {
        st.exchange(use(actIDStrKey, "sid"));
    }
    st.exchange(use(mOffer.offerID, "oid"));
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

    auto timer =
        insert ? db.getInsertTimer("offer") : db.getUpdateTimer("offer");
    st.execute(true);

    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("could not update SQL");
    }

    if (insert)
    {
        delta.addEntry(*this);
    }
    else
    {
        delta.modEntry(*this);
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
}
