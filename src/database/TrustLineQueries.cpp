// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TrustLineQueries.h"
#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "crypto/SignerKey.h"
#include "database/Database.h"
#include "ledger/TrustFrame.h"
#include "util/types.h"

namespace stellar
{

namespace
{

static const char* TRUST_LINE_COLUMN_SELECTOR =
    "SELECT "
    "accountid,assettype,issuer,assetcode,tlimit,balance,flags,lastmodified "
    "FROM trustlines";

void
getKeyFields(LedgerKey const& key, std::string& actIDStrKey,
             std::string& issuerStrKey, std::string& assetCode)
{
    actIDStrKey = KeyUtils::toStrKey(key.trustLine().accountID);
    if (key.trustLine().asset.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        issuerStrKey =
            KeyUtils::toStrKey(key.trustLine().asset.alphaNum4().issuer);
        assetCodeToStr(key.trustLine().asset.alphaNum4().assetCode, assetCode);
    }
    else if (key.trustLine().asset.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        issuerStrKey =
            KeyUtils::toStrKey(key.trustLine().asset.alphaNum12().issuer);
        assetCodeToStr(key.trustLine().asset.alphaNum12().assetCode, assetCode);
    }

    if (actIDStrKey == issuerStrKey)
        throw std::runtime_error("Issuer's own trustline should not be used "
                                 "outside of OperationFrame");
}

std::vector<LedgerEntry>
selectTrustLines(StatementContext& prep, Database& db)
{
    auto result = std::vector<LedgerEntry>{};

    auto actIDStrKey = std::string{};
    auto issuerStrKey = std::string{};
    auto assetCode = std::string{};
    auto assetType = 0u;

    auto trust = TrustFrame{};
    auto& le = trust.getEntry();
    auto& tl = le.data.trustLine();

    auto& st = prep.statement();
    st.exchange(soci::into(actIDStrKey));
    st.exchange(soci::into(assetType));
    st.exchange(soci::into(issuerStrKey));
    st.exchange(soci::into(assetCode));
    st.exchange(soci::into(tl.limit));
    st.exchange(soci::into(tl.balance));
    st.exchange(soci::into(tl.flags));
    st.exchange(soci::into(le.lastModifiedLedgerSeq));
    st.define_and_bind();

    st.execute(true);
    while (st.got_data())
    {
        tl.accountID = KeyUtils::fromStrKey<PublicKey>(actIDStrKey);
        tl.asset.type((AssetType)assetType);
        if (assetType == ASSET_TYPE_CREDIT_ALPHANUM4)
        {
            tl.asset.alphaNum4().issuer =
                KeyUtils::fromStrKey<PublicKey>(issuerStrKey);
            strToAssetCode(tl.asset.alphaNum4().assetCode, assetCode);
        }
        else if (assetType == ASSET_TYPE_CREDIT_ALPHANUM12)
        {
            tl.asset.alphaNum12().issuer =
                KeyUtils::fromStrKey<PublicKey>(issuerStrKey);
            strToAssetCode(tl.asset.alphaNum12().assetCode, assetCode);
        }

        if (!trust.isValid())
        {
            throw std::runtime_error("Invalid TrustEntry");
        }

        result.push_back(le);
        st.fetch();
    }

    return result;
}

const auto DROP_TABLE_TRUSTLINES = "DROP TABLE IF EXISTS trustlines;";

// note: the primary key omits assettype as assetcodes are non overlapping
const auto CREATE_TABLE_TRUSTLINES =
    "CREATE TABLE trustlines"
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

void
createTrustLinesTable(Database& db)
{
    db.getSession() << DROP_TABLE_TRUSTLINES;
    db.getSession() << CREATE_TABLE_TRUSTLINES;
}

optional<LedgerEntry const>
selectTrustLine(AccountID const& accountID, Asset const& asset, Database& db)
{
    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        throw std::runtime_error("XLM TrustLine?");
    }

    if (accountID == getIssuer(asset))
    {
        throw std::runtime_error(
            "TrustLineQueries does not allow for self trust lines");
    }

    std::string accStr, issuerStr, assetStr;

    accStr = KeyUtils::toStrKey(accountID);
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

    auto query = std::string{TRUST_LINE_COLUMN_SELECTOR};
    query += (" WHERE accountid = :id "
              " AND issuer = :issuer "
              " AND assetcode = :asset");
    auto prep = db.getPreparedStatement(query);
    auto& st = prep.statement();
    st.exchange(soci::use(accStr));
    st.exchange(soci::use(issuerStr));
    st.exchange(soci::use(assetStr));

    auto timer = db.getSelectTimer("trust");
    auto entries = selectTrustLines(prep, db);
    if (entries.size() == 1)
    {
        return make_optional<LedgerEntry const>(entries[0]);
    }
    else
    {
        return nullopt<LedgerEntry>();
    }
}

void
insertTrustLine(LedgerEntry const& entry, Database& db)
{
    assert(entry.data.type() == TRUSTLINE);

    auto trust = TrustFrame{entry};
    if (trust.isIssuer())
    {
        throw std::runtime_error(
            "TrustLineQueries does not allow for self trust lines");
    }

    if (!trust.isValid())
    {
        throw std::runtime_error("Invalid TrustEntry");
    }

    auto& trustLine = entry.data.trustLine();

    auto key = entryKey(entry);
    std::string actIDStrKey, issuerStrKey, assetCode;
    unsigned int assetType = key.trustLine().asset.type();
    getKeyFields(key, actIDStrKey, issuerStrKey, assetCode);

    auto prep = db.getPreparedStatement(
        "INSERT INTO trustlines "
        "(accountid, assettype, issuer, assetcode, balance, tlimit, flags, "
        "lastmodified) "
        "VALUES (:v1, :v2, :v3, :v4, :v5, :v6, :v7, :v8)");
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey));
    st.exchange(soci::use(assetType));
    st.exchange(soci::use(issuerStrKey));
    st.exchange(soci::use(assetCode));
    st.exchange(soci::use(trustLine.balance));
    st.exchange(soci::use(trustLine.limit));
    st.exchange(soci::use(trustLine.flags));
    st.exchange(soci::use(entry.lastModifiedLedgerSeq));
    st.define_and_bind();
    {
        auto timer = db.getInsertTimer("trust");
        st.execute(true);
    }

    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }
}

void
updateTrustLine(LedgerEntry const& entry, Database& db)
{
    assert(entry.data.type() == TRUSTLINE);

    auto trust = TrustFrame{entry};
    if (trust.isIssuer())
    {
        throw std::runtime_error(
            "TrustLineQueries does not allow for self trust lines");
    }

    if (!trust.isValid())
    {
        throw std::runtime_error("Invalid TrustEntry");
    }

    auto& trustLine = entry.data.trustLine();

    auto key = entryKey(entry);
    std::string actIDStrKey, issuerStrKey, assetCode;
    getKeyFields(key, actIDStrKey, issuerStrKey, assetCode);

    auto prep = db.getPreparedStatement(
        "UPDATE trustlines "
        "SET balance=:b, tlimit=:tl, flags=:a, lastmodified=:lm "
        "WHERE accountid=:v1 AND issuer=:v2 AND assetcode=:v3");
    auto& st = prep.statement();
    st.exchange(soci::use(trustLine.balance));
    st.exchange(soci::use(trustLine.limit));
    st.exchange(soci::use(trustLine.flags));
    st.exchange(soci::use(entry.lastModifiedLedgerSeq));
    st.exchange(soci::use(actIDStrKey));
    st.exchange(soci::use(issuerStrKey));
    st.exchange(soci::use(assetCode));
    st.define_and_bind();
    {
        auto timer = db.getUpdateTimer("trust");
        st.execute(true);
    }
    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }
}

bool
trustLineExists(LedgerKey const& key, Database& db)
{
    assert(key.type() == TRUSTLINE);

    std::string actIDStrKey, issuerStrKey, assetCode;
    getKeyFields(key, actIDStrKey, issuerStrKey, assetCode);

    auto exists = 0;
    auto timer = db.getSelectTimer("trust-exists");
    auto prep = db.getPreparedStatement(
        "SELECT EXISTS (SELECT NULL FROM trustlines "
        "WHERE accountid=:v1 AND issuer=:v2 AND assetcode=:v3)");
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey));
    st.exchange(soci::use(issuerStrKey));
    st.exchange(soci::use(assetCode));
    st.exchange(soci::into(exists));
    st.define_and_bind();
    st.execute(true);
    return exists != 0;
}

void
deleteTrustLine(LedgerKey const& key, Database& db)
{
    assert(key.type() == TRUSTLINE);

    if (key.trustLine().accountID == getIssuer(key.trustLine().asset))
    {
        throw std::runtime_error(
            "TrustLineQueries does not allow for self trust lines");
    }

    std::string actIDStrKey, issuerStrKey, assetCode;
    getKeyFields(key, actIDStrKey, issuerStrKey, assetCode);

    auto timer = db.getDeleteTimer("trust");
    db.getSession() << "DELETE FROM trustlines "
                       "WHERE accountid=:v1 AND issuer=:v2 AND assetcode=:v3",
        soci::use(actIDStrKey), soci::use(issuerStrKey), soci::use(assetCode);
}

std::unordered_map<AccountID, int>
selectTrustLineCountPerAccount(Database& db)
{
    auto result = std::unordered_map<AccountID, int>{};
    auto count = 0;
    auto sellerId = std::string{};

    auto sql = std::string{R"(
        SELECT COUNT(*), accountid FROM trustlines GROUP BY accountid
    )"};

    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::into(count));
    st.exchange(soci::into(sellerId));
    st.define_and_bind();

    auto timer = db.getSelectTimer("trust");
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
countTrustLines(Database& db)
{
    auto query = std::string{R"(
        SELECT COUNT(*) FROM trustlines
    )"};

    auto result = 0;
    auto prep = db.getPreparedStatement(query);
    auto& st = prep.statement();
    st.exchange(soci::into(result));
    st.define_and_bind();
    st.execute(true);

    return result;
}

uint64_t
countTrustLines(AccountID const& accountID, Database& db)
{
    auto actIDStrKey = KeyUtils::toStrKey(accountID);

    auto query = std::string{R"(
        SELECT COUNT(*) FROM trustlines WHERE accountid = :id
    )"};

    auto result = 0;
    auto prep = db.getPreparedStatement(query);
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey, "id"));
    st.exchange(soci::into(result));
    st.define_and_bind();
    st.execute(true);

    return result;
}
}
