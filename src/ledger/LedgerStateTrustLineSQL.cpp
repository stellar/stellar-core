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
using xdr::operator<;

LedgerState::StateEntry
LedgerState::loadTrustLineFromDatabase(LedgerKey const& key)
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

    auto& db = mRoot->getDatabase();

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

    LedgerEntry le;
    le.data.type(TRUSTLINE);
    TrustLineEntry& tl = le.data.trustLine();

    auto prep =
        db.getPreparedStatement("SELECT tlimit, balance, flags, lastmodified "
                                "FROM trustlines "
                                "WHERE accountid= :id AND issuer= :issuer "
                                "AND assetcode= :asset");
    auto& st = prep.statement();
    st.exchange(soci::into(tl.limit));
    st.exchange(soci::into(tl.balance));
    st.exchange(soci::into(tl.flags));
    st.exchange(soci::into(le.lastModifiedLedgerSeq));
    st.exchange(soci::use(actIDStrKey));
    st.exchange(soci::use(issuerStr));
    st.exchange(soci::use(assetStr));
    st.define_and_bind();
    {
        auto timer = db.getSelectTimer("trust");
        st.execute(true);
    }
    if (!st.got_data())
    {
        return nullptr;
    }

    tl.accountID = key.trustLine().accountID;
    tl.asset = key.trustLine().asset;

    auto entry = std::make_shared<LedgerEntry>(std::move(le));
    return getLeafLedgerState().makeStateEntry(entry, entry);
}

void
LedgerState::storeTrustLineInDatabase(StateEntry const& state)
{
    bool isInsert = !state->ignoreInvalid().previousEntry();
    auto& db = mRoot->getDatabase();

    auto const& entry = *state->ignoreInvalid().entry();
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

    std::string sql;
    if (isInsert)
    {
        sql = "INSERT INTO trustlines "
              "(accountid, assettype, issuer, assetcode, balance, tlimit, "
              "flags, lastmodified) "
              "VALUES (:id, :at, :iss, :ac, :b, :tl, :f, :lm)";
    }
    else
    {
        sql = "UPDATE trustlines "
              "SET balance=:b, tlimit=:tl, flags=:f, lastmodified=:lm "
              "WHERE accountid=:id AND issuer=:iss AND assetcode=:ac";
    }
    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey, "id"));
    st.exchange(soci::use(assetType, "at"));
    st.exchange(soci::use(issuerStr, "iss"));
    st.exchange(soci::use(assetCode, "ac"));
    st.exchange(soci::use(tl.balance, "b"));
    st.exchange(soci::use(tl.limit, "tl"));
    st.exchange(soci::use(tl.flags, "f"));
    st.exchange(soci::use(entry.lastModifiedLedgerSeq, "lm"));
    st.define_and_bind();
    {
        auto timer =
            isInsert ? db.getInsertTimer("trust") : db.getUpdateTimer("trust");
        st.execute(true);
    }
    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }
}

void
LedgerState::deleteTrustLineFromDatabase(StateEntry const& state)
{
    auto& db = mRoot->getDatabase();

    auto const& entry = *state->ignoreInvalid().previousEntry();
    auto const& tl = entry.data.trustLine();

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

    {
        auto timer = db.getDeleteTimer("trust");
        auto prep = db.getPreparedStatement(
            "DELETE FROM trustlines "
            "WHERE accountid=:v1 AND issuer=:v2 AND assetcode=:v3");
        auto& st = prep.statement();
        st.exchange(soci::use(actIDStrKey));
        st.exchange(soci::use(issuerStr));
        st.exchange(soci::use(assetCode));
        st.define_and_bind();
        st.execute(true);
    }
}
}
