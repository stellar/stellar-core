// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "database/DatabaseTypeSpecificOperation.h"
#include "ledger/LedgerTxnImpl.h"
#include "ledger/NonSociRelatedException.h"
#include "main/Application.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "util/types.h"
#include <Tracy.hpp>

namespace stellar
{

void
validateTrustLineKey(uint32_t ledgerVersion, LedgerKey const& key)
{
    auto const& asset = key.trustLine().asset;

    if (!isAssetValid(asset, ledgerVersion))
    {
        throw NonSociRelatedException("TrustLine asset is invalid");
    }
    else if (asset.type() == ASSET_TYPE_NATIVE)
    {
        throw NonSociRelatedException("XLM TrustLine?");
    }
    else if (isIssuer(key.trustLine().accountID, asset))
    {
        throw NonSociRelatedException("TrustLine accountID is issuer");
    }
}

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::loadTrustLine(LedgerKey const& key) const
{
    ZoneScoped;

    validateTrustLineKey(mHeader->ledgerVersion, key);

    std::string accountIDStr = KeyUtils::toStrKey(key.trustLine().accountID);
    auto asset = toOpaqueBase64(key.trustLine().asset);

    std::string trustLineEntryStr;

    auto prep = mApp.getDatabase().getPreparedStatement(
        "SELECT ledgerentry "
        " FROM trustlines "
        "WHERE accountid= :id AND asset= :asset");
    auto& st = prep.statement();
    st.exchange(soci::into(trustLineEntryStr));
    st.exchange(soci::use(accountIDStr));
    st.exchange(soci::use(asset));
    st.define_and_bind();
    {
        auto timer = mApp.getDatabase().getSelectTimer("trust");
        st.execute(true);
    }
    if (!st.got_data())
    {
        return nullptr;
    }

    LedgerEntry le;
    fromOpaqueBase64(le, trustLineEntryStr);
    if (le.data.type() != TRUSTLINE)
    {
        throw NonSociRelatedException("Loaded non-trustline entry");
    }

    return std::make_shared<LedgerEntry const>(std::move(le));
}

std::vector<LedgerEntry>
LedgerTxnRoot::Impl::loadPoolShareTrustLinesByAccountAndAsset(
    AccountID const& accountID, Asset const& asset) const
{
    ZoneScoped;

    std::string accountIDStr = KeyUtils::toStrKey(accountID);
    auto assetStr = toOpaqueBase64(asset);

    std::string trustLineEntryStr;

    auto prep = mApp.getDatabase().getPreparedStatement(
        "SELECT trustlines.ledgerentry "
        "FROM trustlines "
        "INNER JOIN liquiditypool "
        "ON trustlines.asset = liquiditypool.poolasset "
        "AND trustlines.accountid = :v1 "
        "AND (liquiditypool.asseta = :v2 OR liquiditypool.assetb = :v3)");
    auto& st = prep.statement();
    st.exchange(soci::into(trustLineEntryStr));
    st.exchange(soci::use(accountIDStr));
    st.exchange(soci::use(assetStr));
    st.exchange(soci::use(assetStr));
    st.define_and_bind();
    {
        auto timer = mApp.getDatabase().getSelectTimer("trust");
        st.execute(true);
    }

    std::vector<LedgerEntry> trustLines;
    while (st.got_data())
    {
        trustLines.emplace_back();
        fromOpaqueBase64(trustLines.back(), trustLineEntryStr);
        if (trustLines.back().data.type() != TRUSTLINE)
        {
            throw NonSociRelatedException("Loaded non-trustline entry");
        }
        st.fetch();
    }
    return trustLines;
}

class BulkUpsertTrustLinesOperation : public DatabaseTypeSpecificOperation<void>
{
    Database& mDB;
    std::vector<std::string> mAccountIDs;
    std::vector<std::string> mAssets;
    std::vector<std::string> mTrustLineEntries;
    std::vector<int32_t> mLastModifieds;

  public:
    BulkUpsertTrustLinesOperation(Database& DB,
                                  std::vector<EntryIterator> const& entries,
                                  uint32_t ledgerVersion)
        : mDB(DB)
    {
        mAccountIDs.reserve(entries.size());
        mAssets.reserve(entries.size());
        mTrustLineEntries.reserve(entries.size());
        mLastModifieds.reserve(entries.size());

        for (auto const& e : entries)
        {
            releaseAssert(e.entryExists());
            releaseAssert(e.entry().type() ==
                          InternalLedgerEntryType::LEDGER_ENTRY);
            auto const& le = e.entry().ledgerEntry();
            releaseAssert(le.data.type() == TRUSTLINE);

            auto const& tl = le.data.trustLine();

            validateTrustLineKey(ledgerVersion, e.key().ledgerKey());

            mAccountIDs.emplace_back(KeyUtils::toStrKey(tl.accountID));
            mAssets.emplace_back(toOpaqueBase64(tl.asset));
            mTrustLineEntries.emplace_back(toOpaqueBase64(le));
            mLastModifieds.emplace_back(
                unsignedToSigned(le.lastModifiedLedgerSeq));
        }
    }

    void
    doSociGenericOperation()
    {
        std::string sql = "INSERT INTO trustlines ( "
                          "accountid, asset, ledgerentry, lastmodified)"
                          "VALUES ( "
                          ":id, :v1, :v2, :v3 "
                          ") ON CONFLICT (accountid, asset) DO UPDATE SET "
                          "ledgerentry = excluded.ledgerentry, "
                          "lastmodified = excluded.lastmodified";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(mAccountIDs));
        st.exchange(soci::use(mAssets));
        st.exchange(soci::use(mTrustLineEntries));
        st.exchange(soci::use(mLastModifieds));
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

        std::string strAccountIDs, strAssets, strTrustLineEntries,
            strLastModifieds;

        marshalToPGArray(conn, strAccountIDs, mAccountIDs);
        marshalToPGArray(conn, strAssets, mAssets);
        marshalToPGArray(conn, strTrustLineEntries, mTrustLineEntries);
        marshalToPGArray(conn, strLastModifieds, mLastModifieds);

        std::string sql = "WITH r AS (SELECT "
                          "unnest(:ids::TEXT[]), "
                          "unnest(:v1::TEXT[]), "
                          "unnest(:v2::TEXT[]), "
                          "unnest(:v3::INT[])) "
                          "INSERT INTO trustlines ( "
                          "accountid, asset, ledgerEntry, lastmodified"
                          ") SELECT * from r "
                          "ON CONFLICT (accountid, asset) DO UPDATE SET "
                          "ledgerentry = excluded.ledgerentry, "
                          "lastmodified = excluded.lastmodified";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(strAccountIDs));
        st.exchange(soci::use(strAssets));
        st.exchange(soci::use(strTrustLineEntries));
        st.exchange(soci::use(strLastModifieds));
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
    std::vector<std::string> mAssets;

  public:
    BulkDeleteTrustLinesOperation(Database& DB, LedgerTxnConsistency cons,
                                  std::vector<EntryIterator> const& entries,
                                  uint32_t ledgerVersion)
        : mDB(DB), mCons(cons)
    {
        mAccountIDs.reserve(entries.size());
        mAssets.reserve(entries.size());
        for (auto const& e : entries)
        {
            releaseAssert(!e.entryExists());
            releaseAssert(e.key().type() ==
                          InternalLedgerEntryType::LEDGER_ENTRY);
            releaseAssert(e.key().ledgerKey().type() == TRUSTLINE);
            auto const& tl = e.key().ledgerKey().trustLine();

            validateTrustLineKey(ledgerVersion, e.key().ledgerKey());

            mAccountIDs.emplace_back(KeyUtils::toStrKey(tl.accountID));
            mAssets.emplace_back(toOpaqueBase64(tl.asset));
        }
    }

    void
    doSociGenericOperation()
    {
        std::string sql = "DELETE FROM trustlines WHERE accountid = :id "
                          "AND asset = :v1";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(mAccountIDs));
        st.exchange(soci::use(mAssets));
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
        std::string strAccountIDs, strAssets;
        PGconn* conn = pg->conn_;
        marshalToPGArray(conn, strAccountIDs, mAccountIDs);
        marshalToPGArray(conn, strAssets, mAssets);
        std::string sql = "WITH r AS (SELECT "
                          "unnest(:ids::TEXT[]), "
                          "unnest(:v1::TEXT[])"
                          ") "
                          "DELETE FROM trustlines WHERE "
                          "(accountid, asset) IN (SELECT * FROM r)";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(strAccountIDs));
        st.exchange(soci::use(strAssets));
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
    ZoneScoped;
    ZoneValue(static_cast<int64_t>(entries.size()));
    BulkUpsertTrustLinesOperation op(mApp.getDatabase(), entries,
                                     mHeader->ledgerVersion);
    mApp.getDatabase().doDatabaseTypeSpecificOperation(op);
}

void
LedgerTxnRoot::Impl::bulkDeleteTrustLines(
    std::vector<EntryIterator> const& entries, LedgerTxnConsistency cons)
{
    ZoneScoped;
    ZoneValue(static_cast<int64_t>(entries.size()));
    BulkDeleteTrustLinesOperation op(mApp.getDatabase(), cons, entries,
                                     mHeader->ledgerVersion);
    mApp.getDatabase().doDatabaseTypeSpecificOperation(op);
}

void
LedgerTxnRoot::Impl::dropTrustLines(bool rebuild)
{
    throwIfChild();
    mEntryCache.clear();
    mBestOffers.clear();

    mApp.getDatabase().getSession() << "DROP TABLE IF EXISTS trustlines;";

    if (rebuild)
    {
        std::string coll = mApp.getDatabase().getSimpleCollationClause();
        mApp.getDatabase().getSession()
            << "CREATE TABLE trustlines"
            << "("
            << "accountid    VARCHAR(56) " << coll << " NOT NULL,"
            << "asset        TEXT " << coll << " NOT NULL,"
            << "ledgerentry  TEXT NOT NULL,"
            << "lastmodified INT  NOT NULL,"
            << "PRIMARY KEY  (accountid, asset));";
    }
}

class BulkLoadTrustLinesOperation
    : public DatabaseTypeSpecificOperation<std::vector<LedgerEntry>>
{
    Database& mDb;
    std::vector<std::string> mAccountIDs;
    std::vector<std::string> mAssets;

    std::vector<LedgerEntry>
    executeAndFetch(soci::statement& st)
    {
        std::string accountID, asset, trustLineEntryStr;

        st.exchange(soci::into(accountID));
        st.exchange(soci::into(asset));
        st.exchange(soci::into(trustLineEntryStr));
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

            fromOpaqueBase64(le, trustLineEntryStr);
            releaseAssert(le.data.type() == TRUSTLINE);
            releaseAssert(le.data.trustLine().asset.type() !=
                          ASSET_TYPE_NATIVE);

            st.fetch();
        }
        return res;
    }

  public:
    BulkLoadTrustLinesOperation(Database& db,
                                UnorderedSet<LedgerKey> const& keys)
        : mDb(db)
    {
        mAccountIDs.reserve(keys.size());
        mAssets.reserve(keys.size());

        for (auto const& k : keys)
        {
            releaseAssert(k.type() == TRUSTLINE);
            if (k.trustLine().asset.type() == ASSET_TYPE_NATIVE)
            {
                throw NonSociRelatedException(
                    "TrustLine asset can't be native");
            }

            mAccountIDs.emplace_back(
                KeyUtils::toStrKey(k.trustLine().accountID));
            mAssets.emplace_back(toOpaqueBase64(k.trustLine().asset));
        }
    }

    virtual std::vector<LedgerEntry>
    doSqliteSpecificOperation(soci::sqlite3_session_backend* sq) override
    {
        releaseAssert(mAccountIDs.size() == mAssets.size());

        std::vector<char const*> cstrAccountIDs;
        std::vector<char const*> cstrAssets;
        cstrAccountIDs.reserve(mAccountIDs.size());
        cstrAssets.reserve(mAssets.size());
        for (size_t i = 0; i < mAccountIDs.size(); ++i)
        {
            cstrAccountIDs.emplace_back(mAccountIDs[i].c_str());
            cstrAssets.emplace_back(mAssets[i].c_str());
        }

        std::string sqlJoin = "SELECT x.value, y.value FROM "
                              "(SELECT rowid, value FROM carray(?, ?, "
                              "'char*') ORDER BY rowid) "
                              "AS x "
                              "INNER JOIN (SELECT rowid, value FROM "
                              "carray(?, ?, 'char*') ORDER "
                              "BY rowid) AS y ON x.rowid = y.rowid ";
        std::string sql = "WITH r AS (" + sqlJoin +
                          ") SELECT accountid, asset, ledgerentry "
                          "FROM trustlines WHERE (accountid, asset) IN r";

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
        sqlite3_bind_pointer(st, 3, cstrAssets.data(), "carray", 0);
        sqlite3_bind_int(st, 4, static_cast<int>(cstrAssets.size()));
        return executeAndFetch(prep.statement());
    }

#ifdef USE_POSTGRES
    virtual std::vector<LedgerEntry>
    doPostgresSpecificOperation(soci::postgresql_session_backend* pg) override
    {
        releaseAssert(mAccountIDs.size() == mAssets.size());

        std::string strAccountIDs;
        std::string strAssets;
        marshalToPGArray(pg->conn_, strAccountIDs, mAccountIDs);
        marshalToPGArray(pg->conn_, strAssets, mAssets);

        auto prep = mDb.getPreparedStatement(
            "WITH r AS (SELECT unnest(:v1::TEXT[]), "
            "unnest(:v2::TEXT[])) SELECT accountid, asset, "
            "ledgerentry "
            " FROM trustlines "
            "WHERE (accountid, asset) IN (SELECT * "
            "FROM r)");
        auto& st = prep.statement();
        st.exchange(soci::use(strAccountIDs));
        st.exchange(soci::use(strAssets));
        return executeAndFetch(st);
    }
#endif
};

UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>>
LedgerTxnRoot::Impl::bulkLoadTrustLines(
    UnorderedSet<LedgerKey> const& keys) const
{
    ZoneScoped;
    ZoneValue(static_cast<int64_t>(keys.size()));
    if (!keys.empty())
    {
        BulkLoadTrustLinesOperation op(mApp.getDatabase(), keys);
        return populateLoadedEntries(
            keys, mApp.getDatabase().doDatabaseTypeSpecificOperation(op));
    }
    else
    {
        return {};
    }
}
}
