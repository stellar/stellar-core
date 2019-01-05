// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "ledger/LedgerTxnImpl.h"
#include "util/XDROperators.h"
#include "util/types.h"

namespace stellar
{

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::loadTrustLine(LedgerKey const& key) const
{
    auto const& asset = key.trustLine().asset;
    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        throw std::runtime_error("XLM TrustLine?");
    }
    else if (key.trustLine().accountID == getIssuer(asset))
    {
        throw std::runtime_error("TrustLine accountID is issuer");
    }

    std::string actIDStrKey = KeyUtils::toStrKey(key.trustLine().accountID);
    std::string issuerStr, assetStr;
    if (asset.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        assetCodeToStr(asset.alphaNum4().assetCode, assetStr);
        issuerStr = KeyUtils::toStrKey(asset.alphaNum4().issuer);
    }
    else if (asset.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        assetCodeToStr(asset.alphaNum12().assetCode, assetStr);
        issuerStr = KeyUtils::toStrKey(asset.alphaNum12().issuer);
    }

    Liabilities liabilities;
    soci::indicator buyingLiabilitiesInd, sellingLiabilitiesInd;

    LedgerEntry le;
    le.data.type(TRUSTLINE);
    TrustLineEntry& tl = le.data.trustLine();

    auto prep = mDatabase.getPreparedStatement(
        "SELECT tlimit, balance, flags, lastmodified, buyingliabilities, "
        "sellingliabilities FROM trustlines "
        "WHERE accountid= :id AND issuer= :issuer AND assetcode= :asset");
    auto& st = prep.statement();
    st.exchange(soci::into(tl.limit));
    st.exchange(soci::into(tl.balance));
    st.exchange(soci::into(tl.flags));
    st.exchange(soci::into(le.lastModifiedLedgerSeq));
    st.exchange(soci::into(liabilities.buying, buyingLiabilitiesInd));
    st.exchange(soci::into(liabilities.selling, sellingLiabilitiesInd));
    st.exchange(soci::use(actIDStrKey));
    st.exchange(soci::use(issuerStr));
    st.exchange(soci::use(assetStr));
    st.define_and_bind();
    {
        auto timer = mDatabase.getSelectTimer("trust");
        st.execute(true);
    }
    if (!st.got_data())
    {
        return nullptr;
    }

    tl.accountID = key.trustLine().accountID;
    tl.asset = key.trustLine().asset;

    assert(buyingLiabilitiesInd == sellingLiabilitiesInd);
    if (buyingLiabilitiesInd == soci::i_ok)
    {
        tl.ext.v(1);
        tl.ext.v1().liabilities = liabilities;
    }

    return std::make_shared<LedgerEntry>(std::move(le));
}

void
LedgerTxnRoot::Impl::insertOrUpdateTrustLine(LedgerEntry const& entry,
                                             bool isInsert)
{
    auto const& tl = entry.data.trustLine();

    std::string actIDStrKey = KeyUtils::toStrKey(tl.accountID);
    unsigned int assetType = tl.asset.type();
    std::string issuerStr, assetCode;
    if (tl.asset.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        issuerStr = KeyUtils::toStrKey(tl.asset.alphaNum4().issuer);
        assetCodeToStr(tl.asset.alphaNum4().assetCode, assetCode);
    }
    else if (tl.asset.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        issuerStr = KeyUtils::toStrKey(tl.asset.alphaNum12().issuer);
        assetCodeToStr(tl.asset.alphaNum12().assetCode, assetCode);
    }
    if (actIDStrKey == issuerStr)
    {
        throw std::runtime_error("Issuer's own trustline should not be used "
                                 "outside of OperationFrame");
    }

    Liabilities liabilities;
    soci::indicator liabilitiesInd = soci::i_null;
    if (tl.ext.v() == 1)
    {
        liabilities = tl.ext.v1().liabilities;
        liabilitiesInd = soci::i_ok;
    }

    std::string sql;
    if (isInsert)
    {
        sql = "INSERT INTO trustlines "
              "(accountid, assettype, issuer, assetcode, balance, tlimit, "
              "flags, lastmodified, buyingliabilities, sellingliabilities) "
              "VALUES (:id, :at, :iss, :ac, :b, :tl, :f, :lm, :bl, :sl)";
    }
    else
    {
        sql = "UPDATE trustlines "
              "SET balance=:b, tlimit=:tl, flags=:f, lastmodified=:lm, "
              "buyingliabilities=:bl, sellingliabilities=:sl "
              "WHERE accountid=:id AND issuer=:iss AND assetcode=:ac";
    }
    auto prep = mDatabase.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey, "id"));
    if (isInsert)
    {
        st.exchange(soci::use(assetType, "at"));
    }
    st.exchange(soci::use(issuerStr, "iss"));
    st.exchange(soci::use(assetCode, "ac"));
    st.exchange(soci::use(tl.balance, "b"));
    st.exchange(soci::use(tl.limit, "tl"));
    st.exchange(soci::use(tl.flags, "f"));
    st.exchange(soci::use(entry.lastModifiedLedgerSeq, "lm"));
    st.exchange(soci::use(liabilities.buying, liabilitiesInd, "bl"));
    st.exchange(soci::use(liabilities.selling, liabilitiesInd, "sl"));
    st.define_and_bind();
    {
        auto timer = isInsert ? mDatabase.getInsertTimer("trust")
                              : mDatabase.getUpdateTimer("trust");
        st.execute(true);
    }
    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }
}

void
LedgerTxnRoot::Impl::deleteTrustLine(LedgerKey const& key)
{
    auto const& tl = key.trustLine();

    std::string actIDStrKey = KeyUtils::toStrKey(tl.accountID);
    std::string issuerStr, assetCode;
    if (tl.asset.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        issuerStr = KeyUtils::toStrKey(tl.asset.alphaNum4().issuer);
        assetCodeToStr(tl.asset.alphaNum4().assetCode, assetCode);
    }
    else if (tl.asset.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        issuerStr = KeyUtils::toStrKey(tl.asset.alphaNum12().issuer);
        assetCodeToStr(tl.asset.alphaNum12().assetCode, assetCode);
    }
    if (actIDStrKey == issuerStr)
    {
        throw std::runtime_error("Issuer's own trustline should not be used "
                                 "outside of OperationFrame");
    }

    auto prep = mDatabase.getPreparedStatement(
        "DELETE FROM trustlines "
        "WHERE accountid=:v1 AND issuer=:v2 AND assetcode=:v3");
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey));
    st.exchange(soci::use(issuerStr));
    st.exchange(soci::use(assetCode));
    st.define_and_bind();
    {
        auto timer = mDatabase.getDeleteTimer("trust");
        st.execute(true);
    }
    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }
}

void
LedgerTxnRoot::Impl::dropTrustLines()
{
    throwIfChild();
    mEntryCache.clear();
    mBestOffersCache.clear();

    mDatabase.getSession() << "DROP TABLE IF EXISTS trustlines;";
    mDatabase.getSession()
        << "CREATE TABLE trustlines"
           "("
           "accountid    VARCHAR(56)     NOT NULL,"
           "assettype    INT             NOT NULL,"
           "issuer       VARCHAR(56)     NOT NULL,"
           "assetcode    VARCHAR(12)     NOT NULL,"
           "tlimit       BIGINT          NOT NULL CHECK (tlimit > 0),"
           "balance      BIGINT          NOT NULL CHECK (balance >= 0),"
           "flags        INT             NOT NULL,"
           "lastmodified INT             NOT NULL,"
           "PRIMARY KEY  (accountid, issuer, assetcode)"
           ");";
}
}
