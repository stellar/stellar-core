// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/TrustFrame.h"
#include "ledger/AccountFrame.h"
#include "crypto/SecretKey.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "LedgerDelta.h"
#include "util/types.h"

using namespace std;
using namespace soci;

namespace stellar
{
using xdr::operator==;

const char* TrustFrame::kSQLCreateStatement1 =
    "CREATE TABLE trustlines"
    "("
    "accountid     VARCHAR(56)     NOT NULL,"
    "assettype     INT             NOT NULL,"
    "issuer        VARCHAR(56)     NOT NULL,"
    "assetcode     VARCHAR(12)     NOT NULL,"
    "tlimit        BIGINT          NOT NULL DEFAULT 0 CHECK (tlimit >= 0),"
    "balance       BIGINT          NOT NULL DEFAULT 0 CHECK (balance >= 0),"
    "flags         INT             NOT NULL,"
    "PRIMARY KEY (accountid, issuer, assetcode)"
    ");";

const char* TrustFrame::kSQLCreateStatement2 =
    "CREATE INDEX accountlines ON trustlines (accountid);";

TrustFrame::TrustFrame()
    : EntryFrame(TRUSTLINE), mTrustLine(mEntry.trustLine()), mIsIssuer(false)
{
}

TrustFrame::TrustFrame(LedgerEntry const& from)
    : EntryFrame(from), mTrustLine(mEntry.trustLine()), mIsIssuer(false)
{
    assert(isValid());
}

TrustFrame::TrustFrame(TrustFrame const& from) : TrustFrame(from.mEntry)
{
}

TrustFrame& TrustFrame::operator=(TrustFrame const& other)
{
    if (&other != this)
    {
        mTrustLine = other.mTrustLine;
        mKey = other.mKey;
        mKeyCalculated = other.mKeyCalculated;
        mIsIssuer = other.mIsIssuer;
    }
    return *this;
}

void
TrustFrame::getKeyFields(LedgerKey const& key, std::string& actIDStrKey,
                         std::string& issuerStrKey, std::string& assetCode)
{
    actIDStrKey = PubKeyUtils::toStrKey(key.trustLine().accountID);
    if(key.trustLine().asset.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        issuerStrKey =
            PubKeyUtils::toStrKey(key.trustLine().asset.alphaNum4().issuer);
        assetCodeToStr(key.trustLine().asset.alphaNum4().assetCode,
            assetCode);
    } else if(key.trustLine().asset.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        issuerStrKey =
            PubKeyUtils::toStrKey(key.trustLine().asset.alphaNum12().issuer);
        assetCodeToStr(key.trustLine().asset.alphaNum12().assetCode,
            assetCode);
    }

    if (actIDStrKey == issuerStrKey)
        throw std::runtime_error("Issuer's own trustline should not be used "
                                 "outside of OperationFrame");

}

int64_t
TrustFrame::getBalance() const
{
    assert(isValid());
    return mTrustLine.balance;
}

bool
TrustFrame::isAuthorized() const
{
    return (mTrustLine.flags & AUTHORIZED_FLAG) != 0;
}

void
TrustFrame::setAuthorized(bool authorized)
{
    if (authorized)
    {
        mTrustLine.flags |= AUTHORIZED_FLAG;
    }
    else
    {
        mTrustLine.flags &= ~AUTHORIZED_FLAG;
    }
}

bool
TrustFrame::addBalance(int64_t delta)
{
    if (mIsIssuer)
    {
        return true;
    }
    if (delta == 0)
    {
        return true;
    }
    if (!isAuthorized())
    {
        return false;
    }
    if (mTrustLine.limit < delta + mTrustLine.balance)
    {
        return false;
    }
    if ((delta + mTrustLine.balance) < 0)
    {
        return false;
    }
    mTrustLine.balance += delta;
    return true;
}

int64_t
TrustFrame::getMaxAmountReceive() const
{
    int64_t amount = 0;
    if (mIsIssuer)
    {
        amount = INT64_MAX;
    }
    else if (isAuthorized())
    {
        amount = mTrustLine.limit - mTrustLine.balance;
    }
    return amount;
}

bool
TrustFrame::isValid() const
{
    TrustLineEntry const& tl = mTrustLine;
    bool res = tl.asset.type() != ASSET_TYPE_NATIVE;
    res = res && (tl.balance >= 0);
    res = res && (tl.balance <= tl.limit);
    return res;
}

bool
TrustFrame::exists(Database& db, LedgerKey const& key)
{
    if (cachedEntryExists(key, db) && getCachedEntry(key, db) != nullptr)
    {
        return true;
    }

    std::string actIDStrKey, issuerStrKey, assetCode;
    getKeyFields(key, actIDStrKey, issuerStrKey, assetCode);
    int exists = 0;
    auto timer = db.getSelectTimer("trust-exists");
    auto prep = db.getPreparedStatement(
        "SELECT EXISTS (SELECT NULL FROM trustlines "
        "WHERE accountid=:v1 and issuer=:v2 and assetcode=:v3)");
    auto& st = prep.statement();
    st.exchange(use(actIDStrKey));
    st.exchange(use(issuerStrKey));
    st.exchange(use(assetCode));
    st.exchange(into(exists));
    st.define_and_bind();
    st.execute(true);
    return exists != 0;
}

uint64_t
TrustFrame::countObjects(soci::session& sess)
{
    uint64_t count = 0;
    sess << "SELECT COUNT(*) FROM trustlines;", into(count);
    return count;
}

void
TrustFrame::storeDelete(LedgerDelta& delta, Database& db) const
{
    storeDelete(delta, db, getKey());
}

void
TrustFrame::storeDelete(LedgerDelta& delta, Database& db, LedgerKey const& key)
{
    flushCachedEntry(key, db);

    std::string actIDStrKey, issuerStrKey, assetCode;
    getKeyFields(key, actIDStrKey, issuerStrKey, assetCode);

    auto timer = db.getDeleteTimer("trust");
    db.getSession()
        << "DELETE FROM trustlines "
           "WHERE accountid=:v1 AND issuer=:v2 AND assetcode=:v3",
        use(actIDStrKey), use(issuerStrKey), use(assetCode);

    delta.deleteEntry(key);
}

void
TrustFrame::storeChange(LedgerDelta& delta, Database& db) const
{
    assert(isValid());

    auto key = getKey();
    flushCachedEntry(key, db);

    if (mIsIssuer)
        return;

    std::string actIDStrKey, issuerStrKey, assetCode;
    getKeyFields(key, actIDStrKey, issuerStrKey, assetCode);

    auto prep = db.getPreparedStatement(
        "UPDATE trustlines "
        "SET balance=:b, tlimit=:tl, flags=:a "
        "WHERE accountid=:v1 AND issuer=:v2 AND assetcode=:v3");
    auto& st = prep.statement();
    st.exchange(use(mTrustLine.balance));
    st.exchange(use(mTrustLine.limit));
    st.exchange(use(mTrustLine.flags));
    st.exchange(use(actIDStrKey));
    st.exchange(use(issuerStrKey));
    st.exchange(use(assetCode));
    st.define_and_bind();
    {
        auto timer = db.getUpdateTimer("trust");
        st.execute(true);
    }
    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }

    delta.modEntry(*this);
}

void
TrustFrame::storeAdd(LedgerDelta& delta, Database& db) const
{
    assert(isValid());

    auto key = getKey();
    flushCachedEntry(key, db);

    if (mIsIssuer)
        return;


    std::string actIDStrKey, issuerStrKey, assetCode;
    unsigned int assetType = getKey().trustLine().asset.type();
    getKeyFields(getKey(), actIDStrKey, issuerStrKey, assetCode);

    auto prep = db.getPreparedStatement(
        "INSERT INTO trustlines "
        "(accountid, assettype, issuer, assetcode, balance, tlimit, flags) VALUES "
        "(:v1,      :v2,       :v3,    :v4,       :v5,     :v6,    :v7)");
    auto& st = prep.statement();
    st.exchange(use(actIDStrKey));
    st.exchange(use(assetType));
    st.exchange(use(issuerStrKey));
    st.exchange(use(assetCode));
    st.exchange(use(mTrustLine.balance));
    st.exchange(use(mTrustLine.limit));
    st.exchange(use(mTrustLine.flags));
    st.define_and_bind();
    {
        auto timer = db.getInsertTimer("trust");
        st.execute(true);
    }

    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }

    delta.addEntry(*this);
}

static const char* trustLineColumnSelector =
    "SELECT accountid, assettype, issuer, assetcode, tlimit,balance,flags FROM "
    "trustlines";

TrustFrame::pointer
TrustFrame::createIssuerFrame(Asset const& issuer)
{
    pointer res = make_shared<TrustFrame>();
    res->mIsIssuer = true;
    TrustLineEntry& tl = res->mEntry.trustLine();

    if(issuer.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        tl.accountID = issuer.alphaNum4().issuer;
    }
    else if(issuer.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        tl.accountID = issuer.alphaNum12().issuer;
    }

    tl.flags |= AUTHORIZED_FLAG;
    tl.balance = INT64_MAX;
    tl.asset = issuer;
    tl.limit = INT64_MAX;
    return res;
}

TrustFrame::pointer
TrustFrame::loadTrustLine(AccountID const& accountID, Asset const& asset,
                          Database& db)
{
    if(asset.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        if(accountID == asset.alphaNum4().issuer)
            return createIssuerFrame(asset);
    } else if(asset.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        if(accountID == asset.alphaNum12().issuer)
            return createIssuerFrame(asset);
    } else throw std::runtime_error("XLM TrustLine?");

    LedgerKey key;
    key.type(TRUSTLINE);
    key.trustLine().accountID = accountID;
    key.trustLine().asset = asset;
    if (cachedEntryExists(key, db))
    {
        auto p = getCachedEntry(key, db);
        return p ? std::make_shared<TrustFrame>(*p) : nullptr;
    }

    std::string accStr, issuerStr, assetStr;

    accStr = PubKeyUtils::toStrKey(accountID);
    if(asset.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        assetCodeToStr(asset.alphaNum4().assetCode, assetStr);
        issuerStr = PubKeyUtils::toStrKey(asset.alphaNum4().issuer);
    } else if(asset.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        assetCodeToStr(asset.alphaNum12().assetCode, assetStr);
        issuerStr = PubKeyUtils::toStrKey(asset.alphaNum12().issuer);
    }

    auto query = std::string(trustLineColumnSelector);
    query += (" WHERE accountid = :id "
              " AND issuer = :issuer "
              " AND assetcode = :asset");
    auto prep = db.getPreparedStatement(query);
    auto& st = prep.statement();
    st.exchange(use(accStr));
    st.exchange(use(issuerStr));
    st.exchange(use(assetStr));

    pointer retLine;
    auto timer = db.getSelectTimer("trust");
    loadLines(prep, [&retLine](LedgerEntry const& trust)
              {
                  retLine = make_shared<TrustFrame>(trust);
              });

    if (retLine)
    {
        retLine->putCachedEntry(db);
    }
    else
    {
        putCachedEntry(key, nullptr, db);
    }
    return retLine;
}

bool
TrustFrame::hasIssued(AccountID const& issuerID, Database& db)
{
    std::string accStrKey;
    accStrKey = PubKeyUtils::toStrKey(issuerID);
    int balance = 0;

    auto prep = db.getPreparedStatement(
        "SELECT balance FROM trustlines WHERE issuer=:id "
        "AND balance>0 LIMIT 1");
    auto& st = prep.statement();
    st.exchange(use(accStrKey));
    st.exchange(into(balance));
    st.define_and_bind();

    {
        auto timer = db.getSelectTimer("trust");
        st.execute(true);
    }
    if (st.got_data())
    {
        return true;
    }
    return false;
}

void
TrustFrame::loadLines(StatementContext& prep,
                      std::function<void(LedgerEntry const&)> trustProcessor)
{
    string actIDStrKey;
    std::string issuerStrKey, assetCode;
    unsigned int assetType;

    LedgerEntry le;
    le.type(TRUSTLINE);

    TrustLineEntry& tl = le.trustLine();


    auto& st = prep.statement();
    st.exchange(into(actIDStrKey));
    st.exchange(into(assetType));
    st.exchange(into(issuerStrKey));
    st.exchange(into(assetCode));
    st.exchange(into(tl.limit));
    st.exchange(into(tl.balance));
    st.exchange(into(tl.flags));
    st.define_and_bind();

    st.execute(true);
    while (st.got_data())
    {
        tl.accountID = PubKeyUtils::fromStrKey(actIDStrKey);
        tl.asset.type((AssetType)assetType);
        if(assetType == ASSET_TYPE_CREDIT_ALPHANUM4)
        {
            tl.asset.alphaNum4().issuer = PubKeyUtils::fromStrKey(issuerStrKey);
            strToAssetCode(tl.asset.alphaNum4().assetCode, assetCode);
        } else if(assetType == ASSET_TYPE_CREDIT_ALPHANUM12)
        {
            tl.asset.alphaNum12().issuer = PubKeyUtils::fromStrKey(issuerStrKey);
            strToAssetCode(tl.asset.alphaNum12().assetCode, assetCode);
        }

        trustProcessor(le);

        st.fetch();
    }
}

void
TrustFrame::loadLines(AccountID const& accountID,
                      std::vector<TrustFrame::pointer>& retLines, Database& db)
{
    std::string actIDStrKey;
    actIDStrKey = PubKeyUtils::toStrKey(accountID);

    auto query = std::string(trustLineColumnSelector);
    query += (" WHERE accountid = :id ");
    auto prep = db.getPreparedStatement(query);
    auto& st = prep.statement();
    st.exchange(use(actIDStrKey));

    auto timer = db.getSelectTimer("trust");
    loadLines(prep, [&retLines](LedgerEntry const& cur)
              {
                  retLines.emplace_back(make_shared<TrustFrame>(cur));
              });
}

void
TrustFrame::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS trustlines;";
    db.getSession() << kSQLCreateStatement1;
    db.getSession() << kSQLCreateStatement2;
}
}
