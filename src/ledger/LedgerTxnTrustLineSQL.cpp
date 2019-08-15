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

static void
getTrustLineStrings(AccountID const& accountID, Asset const& asset,
                    std::string& accountIDStr, std::string& issuerStr,
                    std::string& assetCodeStr)
{
    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        throw std::runtime_error("XLM TrustLine?");
    }
    else if (accountID == getIssuer(asset))
    {
        throw std::runtime_error("TrustLine accountID is issuer");
    }

    accountIDStr = KeyUtils::toStrKey(accountID);
    if (asset.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        assetCodeToStr(asset.alphaNum4().assetCode, assetCodeStr);
        issuerStr = KeyUtils::toStrKey(asset.alphaNum4().issuer);
    }
    else if (asset.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        assetCodeToStr(asset.alphaNum12().assetCode, assetCodeStr);
        issuerStr = KeyUtils::toStrKey(asset.alphaNum12().issuer);
    }
    else
    {
        throw std::runtime_error("Unknown asset type");
    }
}

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::loadTrustLine(LedgerKey const& key) const
{
    std::string accountIDStr, issuerStr, assetStr;
    getTrustLineStrings(key.trustLine().accountID, key.trustLine().asset,
                        accountIDStr, issuerStr, assetStr);

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
    st.exchange(soci::use(accountIDStr));
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

class BulkUpsertTrustLinesOperation : public DatabaseTypeSpecificOperation<void>
{
    Database& mDB;
    std::vector<std::string> mAccountIDs;
    std::vector<int32_t> mAssetTypes;
    std::vector<std::string> mIssuers;
    std::vector<std::string> mAssetCodes;
    std::vector<int64_t> mTlimits;
    std::vector<int64_t> mBalances;
    std::vector<int32_t> mFlags;
    std::vector<int32_t> mLastModifieds;
    std::vector<int64_t> mBuyingLiabilities;
    std::vector<int64_t> mSellingLiabilities;
    std::vector<soci::indicator> mLiabilitiesInds;

  public:
    BulkUpsertTrustLinesOperation(Database& DB,
                                  std::vector<EntryIterator> const& entries)
        : mDB(DB)
    {
        mAccountIDs.reserve(entries.size());
        mAssetTypes.reserve(entries.size());
        mIssuers.reserve(entries.size());
        mAssetCodes.reserve(entries.size());
        mTlimits.reserve(entries.size());
        mBalances.reserve(entries.size());
        mFlags.reserve(entries.size());
        mLastModifieds.reserve(entries.size());
        mBuyingLiabilities.reserve(entries.size());
        mSellingLiabilities.reserve(entries.size());
        mLiabilitiesInds.reserve(entries.size());

        for (auto const& e : entries)
        {
            assert(e.entryExists());
            assert(e.entry().data.type() == TRUSTLINE);
            auto const& tl = e.entry().data.trustLine();
            std::string accountIDStr, issuerStr, assetCodeStr;
            getTrustLineStrings(tl.accountID, tl.asset, accountIDStr, issuerStr,
                                assetCodeStr);

            mAccountIDs.emplace_back(accountIDStr);
            mAssetTypes.emplace_back(
                unsignedToSigned(static_cast<uint32_t>(tl.asset.type())));
            mIssuers.emplace_back(issuerStr);
            mAssetCodes.emplace_back(assetCodeStr);
            mTlimits.emplace_back(tl.limit);
            mBalances.emplace_back(tl.balance);
            mFlags.emplace_back(unsignedToSigned(tl.flags));
            mLastModifieds.emplace_back(
                unsignedToSigned(e.entry().lastModifiedLedgerSeq));

            if (tl.ext.v() >= 1)
            {
                mBuyingLiabilities.emplace_back(tl.ext.v1().liabilities.buying);
                mSellingLiabilities.emplace_back(
                    tl.ext.v1().liabilities.selling);
                mLiabilitiesInds.emplace_back(soci::i_ok);
            }
            else
            {
                mBuyingLiabilities.emplace_back(0);
                mSellingLiabilities.emplace_back(0);
                mLiabilitiesInds.emplace_back(soci::i_null);
            }
        }
    }

    void
    doSociGenericOperation()
    {
        std::string sql =
            "INSERT INTO trustlines ( "
            "accountid, assettype, issuer, assetcode,"
            "tlimit, balance, flags, lastmodified, "
            "buyingliabilities, sellingliabilities "
            ") VALUES ( "
            ":id, :v1, :v2, :v3, :v4, :v5, :v6, :v7, :v8, :v9 "
            ") ON CONFLICT (accountid, issuer, assetcode) DO UPDATE SET "
            "assettype = excluded.assettype, "
            "tlimit = excluded.tlimit, "
            "balance = excluded.balance, "
            "flags = excluded.flags, "
            "lastmodified = excluded.lastmodified, "
            "buyingliabilities = excluded.buyingliabilities, "
            "sellingliabilities = excluded.sellingliabilities ";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(mAccountIDs));
        st.exchange(soci::use(mAssetTypes));
        st.exchange(soci::use(mIssuers));
        st.exchange(soci::use(mAssetCodes));
        st.exchange(soci::use(mTlimits));
        st.exchange(soci::use(mBalances));
        st.exchange(soci::use(mFlags));
        st.exchange(soci::use(mLastModifieds));
        st.exchange(soci::use(mBuyingLiabilities, mLiabilitiesInds));
        st.exchange(soci::use(mSellingLiabilities, mLiabilitiesInds));
        st.define_and_bind();
        {
            auto timer = mDB.getUpsertTimer("trustline");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mAccountIDs.size())
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

        std::string strAccountIDs, strAssetTypes, strIssuers, strAssetCodes,
            strTlimits, strBalances, strFlags, strLastModifieds,
            strBuyingLiabilities, strSellingLiabilities;

        marshalToPGArray(conn, strAccountIDs, mAccountIDs);
        marshalToPGArray(conn, strAssetTypes, mAssetTypes);
        marshalToPGArray(conn, strIssuers, mIssuers);
        marshalToPGArray(conn, strAssetCodes, mAssetCodes);
        marshalToPGArray(conn, strTlimits, mTlimits);
        marshalToPGArray(conn, strBalances, mBalances);
        marshalToPGArray(conn, strFlags, mFlags);
        marshalToPGArray(conn, strLastModifieds, mLastModifieds);
        marshalToPGArray(conn, strBuyingLiabilities, mBuyingLiabilities,
                         &mLiabilitiesInds);
        marshalToPGArray(conn, strSellingLiabilities, mSellingLiabilities,
                         &mLiabilitiesInds);

        std::string sql =
            "WITH r AS (SELECT "
            "unnest(:ids::TEXT[]), "
            "unnest(:v1::INT[]), "
            "unnest(:v2::TEXT[]), "
            "unnest(:v3::TEXT[]), "
            "unnest(:v4::BIGINT[]), "
            "unnest(:v5::BIGINT[]), "
            "unnest(:v6::INT[]), "
            "unnest(:v7::INT[]), "
            "unnest(:v8::BIGINT[]), "
            "unnest(:v9::BIGINT[]) "
            ")"
            "INSERT INTO trustlines ( "
            "accountid, assettype, issuer, assetcode,"
            "tlimit, balance, flags, lastmodified, "
            "buyingliabilities, sellingliabilities "
            ") SELECT * from r "
            "ON CONFLICT (accountid, issuer, assetcode) DO UPDATE SET "
            "assettype = excluded.assettype, "
            "tlimit = excluded.tlimit, "
            "balance = excluded.balance, "
            "flags = excluded.flags, "
            "lastmodified = excluded.lastmodified, "
            "buyingliabilities = excluded.buyingliabilities, "
            "sellingliabilities = excluded.sellingliabilities ";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(strAccountIDs));
        st.exchange(soci::use(strAssetTypes));
        st.exchange(soci::use(strIssuers));
        st.exchange(soci::use(strAssetCodes));
        st.exchange(soci::use(strTlimits));
        st.exchange(soci::use(strBalances));
        st.exchange(soci::use(strFlags));
        st.exchange(soci::use(strLastModifieds));
        st.exchange(soci::use(strBuyingLiabilities));
        st.exchange(soci::use(strSellingLiabilities));
        st.define_and_bind();
        {
            auto timer = mDB.getUpsertTimer("trustline");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mAccountIDs.size())
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }
#endif
};

class BulkDeleteTrustLinesOperation : public DatabaseTypeSpecificOperation<void>
{
    Database& mDB;
    LedgerTxnConsistency mCons;
    std::vector<std::string> mAccountIDs;
    std::vector<std::string> mIssuers;
    std::vector<std::string> mAssetCodes;

  public:
    BulkDeleteTrustLinesOperation(Database& DB, LedgerTxnConsistency cons,
                                  std::vector<EntryIterator> const& entries)
        : mDB(DB), mCons(cons)
    {
        mAccountIDs.reserve(entries.size());
        mIssuers.reserve(entries.size());
        mAssetCodes.reserve(entries.size());
        for (auto const& e : entries)
        {
            assert(!e.entryExists());
            assert(e.key().type() == TRUSTLINE);
            auto const& tl = e.key().trustLine();
            std::string accountIDStr, issuerStr, assetCodeStr;
            getTrustLineStrings(tl.accountID, tl.asset, accountIDStr, issuerStr,
                                assetCodeStr);
            mAccountIDs.emplace_back(accountIDStr);
            mIssuers.emplace_back(issuerStr);
            mAssetCodes.emplace_back(assetCodeStr);
        }
    }

    void
    doSociGenericOperation()
    {
        std::string sql = "DELETE FROM trustlines WHERE accountid = :id "
                          "AND issuer = :v1 AND assetcode = :v2";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(mAccountIDs));
        st.exchange(soci::use(mIssuers));
        st.exchange(soci::use(mAssetCodes));
        st.define_and_bind();
        {
            auto timer = mDB.getDeleteTimer("trustline");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mAccountIDs.size() &&
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
        std::string strAccountIDs, strIssuers, strAssetCodes;
        PGconn* conn = pg->conn_;
        marshalToPGArray(conn, strAccountIDs, mAccountIDs);
        marshalToPGArray(conn, strIssuers, mIssuers);
        marshalToPGArray(conn, strAssetCodes, mAssetCodes);
        std::string sql = "WITH r AS (SELECT "
                          "unnest(:ids::TEXT[]), "
                          "unnest(:v1::TEXT[]), "
                          "unnest(:v2::TEXT[]) "
                          ") "
                          "DELETE FROM trustlines WHERE "
                          "(accountid, issuer, assetcode) IN (SELECT * FROM r)";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(strAccountIDs));
        st.exchange(soci::use(strIssuers));
        st.exchange(soci::use(strAssetCodes));
        st.define_and_bind();
        {
            auto timer = mDB.getDeleteTimer("trustline");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mAccountIDs.size() &&
            mCons == LedgerTxnConsistency::EXACT)
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }
#endif
};

void
LedgerTxnRoot::Impl::bulkUpsertTrustLines(
    std::vector<EntryIterator> const& entries)
{
    BulkUpsertTrustLinesOperation op(mDatabase, entries);
    mDatabase.doDatabaseTypeSpecificOperation(op);
}

void
LedgerTxnRoot::Impl::bulkDeleteTrustLines(
    std::vector<EntryIterator> const& entries, LedgerTxnConsistency cons)
{
    BulkDeleteTrustLinesOperation op(mDatabase, cons, entries);
    mDatabase.doDatabaseTypeSpecificOperation(op);
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
           "buyingliabilities BIGINT CHECK (buyingliabilities >= 0),"
           "sellingliabilities BIGINT CHECK (sellingliabilities >= 0),"
           "flags        INT             NOT NULL,"
           "lastmodified INT             NOT NULL,"
           "PRIMARY KEY  (accountid, issuer, assetcode)"
           ");";
}

class BulkLoadTrustLinesOperation
    : public DatabaseTypeSpecificOperation<std::vector<LedgerEntry>>
{
    Database& mDb;
    std::vector<std::string> mAccountIDs;
    std::vector<std::string> mIssuers;
    std::vector<std::string> mAssetCodes;

    std::vector<LedgerEntry>
    executeAndFetch(soci::statement& st)
    {
        std::string accountID, assetCode, issuer;
        int64_t balance, limit;
        uint32_t assetType, flags, lastModified;
        Liabilities liabilities;
        soci::indicator buyingLiabilitiesInd, sellingLiabilitiesInd;

        st.exchange(soci::into(accountID));
        st.exchange(soci::into(assetType));
        st.exchange(soci::into(assetCode));
        st.exchange(soci::into(issuer));
        st.exchange(soci::into(limit));
        st.exchange(soci::into(balance));
        st.exchange(soci::into(flags));
        st.exchange(soci::into(lastModified));
        st.exchange(soci::into(liabilities.buying, buyingLiabilitiesInd));
        st.exchange(soci::into(liabilities.selling, sellingLiabilitiesInd));
        st.define_and_bind();
        {
            auto timer = mDb.getSelectTimer("trust");
            st.execute(true);
        }

        std::vector<LedgerEntry> res;
        while (st.got_data())
        {
            res.emplace_back();
            auto& le = res.back();
            le.data.type(TRUSTLINE);
            auto& tl = le.data.trustLine();

            tl.accountID = KeyUtils::fromStrKey<PublicKey>(accountID);

            assert(assetType != ASSET_TYPE_NATIVE);
            tl.asset.type(static_cast<AssetType>(assetType));
            if (assetType == ASSET_TYPE_CREDIT_ALPHANUM4)
            {
                tl.asset.alphaNum4().issuer =
                    KeyUtils::fromStrKey<PublicKey>(issuer);
                strToAssetCode(tl.asset.alphaNum4().assetCode, assetCode);
            }
            else
            {
                tl.asset.alphaNum12().issuer =
                    KeyUtils::fromStrKey<PublicKey>(issuer);
                strToAssetCode(tl.asset.alphaNum12().assetCode, assetCode);
            }

            tl.limit = limit;
            tl.balance = balance;
            tl.flags = flags;
            le.lastModifiedLedgerSeq = lastModified;

            assert(buyingLiabilitiesInd == sellingLiabilitiesInd);
            if (buyingLiabilitiesInd == soci::i_ok)
            {
                tl.ext.v(1);
                tl.ext.v1().liabilities = liabilities;
            }

            st.fetch();
        }
        return res;
    }

  public:
    BulkLoadTrustLinesOperation(Database& db,
                                std::unordered_set<LedgerKey> const& keys)
        : mDb(db)
    {
        mAccountIDs.reserve(keys.size());
        mIssuers.reserve(keys.size());
        mAssetCodes.reserve(keys.size());
        for (auto const& k : keys)
        {
            assert(k.type() == TRUSTLINE);
            mAccountIDs.emplace_back(
                KeyUtils::toStrKey(k.trustLine().accountID));

            auto const& asset = k.trustLine().asset;
            assert(asset.type() != ASSET_TYPE_NATIVE);
            mAssetCodes.emplace_back();
            if (asset.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
            {
                assetCodeToStr(asset.alphaNum4().assetCode, mAssetCodes.back());
                mIssuers.emplace_back(
                    KeyUtils::toStrKey(asset.alphaNum4().issuer));
            }
            else if (asset.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
            {
                assetCodeToStr(asset.alphaNum12().assetCode,
                               mAssetCodes.back());
                mIssuers.emplace_back(
                    KeyUtils::toStrKey(asset.alphaNum12().issuer));
            }
        }
    }

    virtual std::vector<LedgerEntry>
    doSqliteSpecificOperation(soci::sqlite3_session_backend* sq) override
    {
        assert(mAccountIDs.size() == mIssuers.size());
        assert(mAccountIDs.size() == mAssetCodes.size());

        std::vector<char const*> cstrAccountIDs;
        std::vector<char const*> cstrIssuers;
        std::vector<char const*> cstrAssetCodes;
        cstrAccountIDs.reserve(mAccountIDs.size());
        cstrIssuers.reserve(mIssuers.size());
        cstrAssetCodes.reserve(mAssetCodes.size());
        for (size_t i = 0; i < mAccountIDs.size(); ++i)
        {
            cstrAccountIDs.emplace_back(mAccountIDs[i].c_str());
            cstrIssuers.emplace_back(mIssuers[i].c_str());
            cstrAssetCodes.emplace_back(mAssetCodes[i].c_str());
        }

        std::string sqlJoin =
            "SELECT x.value, y.value, z.value FROM "
            "(SELECT rowid, value FROM carray(?, ?, 'char*') ORDER BY rowid) "
            "AS x "
            "INNER JOIN (SELECT rowid, value FROM carray(?, ?, 'char*') ORDER "
            "BY rowid) AS y ON x.rowid = y.rowid "
            "INNER JOIN (SELECT rowid, value FROM carray(?, ?, 'char*') ORDER "
            "BY rowid) AS z ON x.rowid = z.rowid";
        std::string sql =
            "WITH r AS (" + sqlJoin +
            ") SELECT accountid, assettype, assetcode, issuer, tlimit, "
            "balance, flags, lastmodified, buyingliabilities, "
            "sellingliabilities "
            "FROM trustlines WHERE (accountid, issuer, assetcode) IN r";

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
        sqlite3_bind_pointer(st, 1, cstrAccountIDs.data(), "carray", 0);
        sqlite3_bind_int(st, 2, static_cast<int>(cstrAccountIDs.size()));
        sqlite3_bind_pointer(st, 3, cstrIssuers.data(), "carray", 0);
        sqlite3_bind_int(st, 4, static_cast<int>(cstrIssuers.size()));
        sqlite3_bind_pointer(st, 5, cstrAssetCodes.data(), "carray", 0);
        sqlite3_bind_int(st, 6, static_cast<int>(cstrAssetCodes.size()));
        return executeAndFetch(prep.statement());
    }

#ifdef USE_POSTGRES
    virtual std::vector<LedgerEntry>
    doPostgresSpecificOperation(soci::postgresql_session_backend* pg) override
    {
        assert(mAccountIDs.size() == mIssuers.size());
        assert(mAccountIDs.size() == mAssetCodes.size());

        std::string strAccountIDs;
        std::string strIssuers;
        std::string strAssetCodes;
        marshalToPGArray(pg->conn_, strAccountIDs, mAccountIDs);
        marshalToPGArray(pg->conn_, strIssuers, mIssuers);
        marshalToPGArray(pg->conn_, strAssetCodes, mAssetCodes);

        auto prep = mDb.getPreparedStatement(
            "WITH r AS (SELECT unnest(:v1::TEXT[]), unnest(:v2::TEXT[]), "
            "unnest(:v3::TEXT[])) SELECT accountid, assettype, assetcode, "
            "issuer, tlimit, balance, flags, lastmodified, buyingliabilities, "
            "sellingliabilities FROM trustlines "
            "WHERE (accountid, issuer, assetcode) IN (SELECT * FROM r)");
        auto& st = prep.statement();
        st.exchange(soci::use(strAccountIDs));
        st.exchange(soci::use(strIssuers));
        st.exchange(soci::use(strAssetCodes));
        return executeAndFetch(st);
    }
#endif
};

std::unordered_map<LedgerKey, std::shared_ptr<LedgerEntry const>>
LedgerTxnRoot::Impl::bulkLoadTrustLines(
    std::unordered_set<LedgerKey> const& keys) const
{
    if (!keys.empty())
    {
        BulkLoadTrustLinesOperation op(mDatabase, keys);
        return populateLoadedEntries(
            keys, mDatabase.doDatabaseTypeSpecificOperation(op));
    }
    else
    {
        return {};
    }
}
}
