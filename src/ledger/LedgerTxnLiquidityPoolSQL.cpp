// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxnImpl.h"
#include "ledger/NonSociRelatedException.h"
#include "util/types.h"

namespace stellar
{

static void
throwIfNotLiquidityPool(LedgerEntryType type)
{
    if (type != LIQUIDITY_POOL)
    {
        throw NonSociRelatedException("LedgerEntry is not a LIQUIDITY_POOL");
    }
}

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::loadLiquidityPool(LedgerKey const& key) const
{
    auto poolID = toOpaqueBase64(key.liquidityPool().liquidityPoolID);

    std::string liquidityPoolEntryStr;

    std::string sql = "SELECT ledgerentry "
                      "FROM liquiditypool "
                      "WHERE poolid= :poolid";
    auto prep = mDatabase.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::into(liquidityPoolEntryStr));
    st.exchange(soci::use(poolID));
    st.define_and_bind();
    {
        auto timer = mDatabase.getSelectTimer("liquiditypool");
        st.execute(true);
    }
    if (!st.got_data())
    {
        return nullptr;
    }

    LedgerEntry le;
    fromOpaqueBase64(le, liquidityPoolEntryStr);
    throwIfNotLiquidityPool(le.data.type());

    return std::make_shared<LedgerEntry const>(std::move(le));
}

class BulkLoadLiquidityPoolOperation
    : public DatabaseTypeSpecificOperation<std::vector<LedgerEntry>>
{
    Database& mDb;
    std::vector<std::string> mPoolIDs;

    std::vector<LedgerEntry>
    executeAndFetch(soci::statement& st)
    {
        std::string liquidityPoolEntryStr;

        st.exchange(soci::into(liquidityPoolEntryStr));
        st.define_and_bind();
        {
            auto timer = mDb.getSelectTimer("liquiditypool");
            st.execute(true);
        }

        std::vector<LedgerEntry> res;
        while (st.got_data())
        {
            res.emplace_back();
            auto& le = res.back();

            fromOpaqueBase64(le, liquidityPoolEntryStr);
            throwIfNotLiquidityPool(le.data.type());

            st.fetch();
        }
        return res;
    }

  public:
    BulkLoadLiquidityPoolOperation(Database& db,
                                   UnorderedSet<LedgerKey> const& keys)
        : mDb(db)
    {
        mPoolIDs.reserve(keys.size());
        for (auto const& k : keys)
        {
            throwIfNotLiquidityPool(k.type());
            mPoolIDs.emplace_back(
                toOpaqueBase64(k.liquidityPool().liquidityPoolID));
        }
    }

    std::vector<LedgerEntry>
    doSqliteSpecificOperation(soci::sqlite3_session_backend* sq) override
    {
        std::vector<char const*> cstrPoolIDs;
        cstrPoolIDs.reserve(mPoolIDs.size());
        for (size_t i = 0; i < mPoolIDs.size(); ++i)
        {
            cstrPoolIDs.emplace_back(mPoolIDs[i].c_str());
        }

        std::string sql = "WITH r AS (SELECT value FROM carray(?, ?, 'char*')) "
                          "SELECT ledgerentry "
                          "FROM liquiditypool "
                          "WHERE poolid IN r";

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
        sqlite3_bind_pointer(st, 1, cstrPoolIDs.data(), "carray", 0);
        sqlite3_bind_int(st, 2, static_cast<int>(cstrPoolIDs.size()));
        return executeAndFetch(prep.statement());
    }

#ifdef USE_POSTGRES
    std::vector<LedgerEntry>
    doPostgresSpecificOperation(soci::postgresql_session_backend* pg) override
    {
        std::string strPoolIDs;
        marshalToPGArray(pg->conn_, strPoolIDs, mPoolIDs);

        std::string sql = "WITH r AS (SELECT unnest(:v1::TEXT[])) "
                          "SELECT ledgerentry "
                          "FROM liquiditypool "
                          "WHERE poolid IN (SELECT * from r)";

        auto prep = mDb.getPreparedStatement(sql);
        auto& st = prep.statement();
        st.exchange(soci::use(strPoolIDs));
        return executeAndFetch(st);
    }
#endif
};

UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>>
LedgerTxnRoot::Impl::bulkLoadLiquidityPool(
    UnorderedSet<LedgerKey> const& keys) const
{
    if (!keys.empty())
    {
        BulkLoadLiquidityPoolOperation op(mDatabase, keys);
        return populateLoadedEntries(
            keys, mDatabase.doDatabaseTypeSpecificOperation(op));
    }
    else
    {
        return {};
    }
}

class BulkDeleteLiquidityPoolOperation
    : public DatabaseTypeSpecificOperation<void>
{
    Database& mDb;
    LedgerTxnConsistency mCons;
    std::vector<std::string> mPoolIDs;

  public:
    BulkDeleteLiquidityPoolOperation(Database& db, LedgerTxnConsistency cons,
                                     std::vector<EntryIterator> const& entries)
        : mDb(db), mCons(cons)
    {
        mPoolIDs.reserve(entries.size());
        for (auto const& e : entries)
        {
            assert(!e.entryExists());
            throwIfNotLiquidityPool(e.key().ledgerKey().type());
            mPoolIDs.emplace_back(toOpaqueBase64(
                e.key().ledgerKey().liquidityPool().liquidityPoolID));
        }
    }

    void
    doSociGenericOperation()
    {
        std::string sql = "DELETE FROM liquiditypool WHERE poolid = :id";
        auto prep = mDb.getPreparedStatement(sql);
        auto& st = prep.statement();
        st.exchange(soci::use(mPoolIDs));
        st.define_and_bind();
        {
            auto timer = mDb.getDeleteTimer("liquiditypool");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mPoolIDs.size() &&
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
        std::string strPoolIDs;
        marshalToPGArray(pg->conn_, strPoolIDs, mPoolIDs);

        std::string sql = "WITH r AS (SELECT unnest(:v1::TEXT[])) "
                          "DELETE FROM liquiditypool "
                          "WHERE poolid IN (SELECT * FROM r)";

        auto prep = mDb.getPreparedStatement(sql);
        auto& st = prep.statement();
        st.exchange(soci::use(strPoolIDs));
        st.define_and_bind();
        {
            auto timer = mDb.getDeleteTimer("liquiditypool");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mPoolIDs.size() &&
            mCons == LedgerTxnConsistency::EXACT)
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }
#endif
};

void
LedgerTxnRoot::Impl::bulkDeleteLiquidityPool(
    std::vector<EntryIterator> const& entries, LedgerTxnConsistency cons)
{
    BulkDeleteLiquidityPoolOperation op(mDatabase, cons, entries);
    mDatabase.doDatabaseTypeSpecificOperation(op);
}

class BulkUpsertLiquidityPoolOperation
    : public DatabaseTypeSpecificOperation<void>
{
    Database& mDb;
    std::vector<std::string> mPoolIDs;
    std::vector<std::string> mAssetAs;
    std::vector<std::string> mAssetBs;
    std::vector<std::string> mLiquidityPoolEntries;
    std::vector<int32_t> mLastModifieds;

    void
    accumulateEntry(LedgerEntry const& entry)
    {
        throwIfNotLiquidityPool(entry.data.type());

        auto const& lp = entry.data.liquidityPool();
        auto const& cp = lp.body.constantProduct();
        mPoolIDs.emplace_back(toOpaqueBase64(lp.liquidityPoolID));
        mAssetAs.emplace_back(toOpaqueBase64(cp.params.assetA));
        mAssetBs.emplace_back(toOpaqueBase64(cp.params.assetB));
        mLiquidityPoolEntries.emplace_back(toOpaqueBase64(entry));
        mLastModifieds.emplace_back(
            unsignedToSigned(entry.lastModifiedLedgerSeq));
    }

  public:
    BulkUpsertLiquidityPoolOperation(
        Database& Db, std::vector<EntryIterator> const& entryIter)
        : mDb(Db)
    {
        for (auto const& e : entryIter)
        {
            assert(e.entryExists());
            accumulateEntry(e.entry().ledgerEntry());
        }
    }

    void
    doSociGenericOperation()
    {
        std::string sql = "INSERT INTO liquiditypool "
                          "(poolid, asseta, assetb, ledgerentry, lastmodified) "
                          "VALUES "
                          "( :id, :v1, :v2, :v3, :v4 ) "
                          "ON CONFLICT (poolid) DO UPDATE SET "
                          "asseta = excluded.asseta, "
                          "assetb = excluded.assetb, "
                          "ledgerentry = excluded.ledgerentry, "
                          "lastmodified = excluded.lastmodified";

        auto prep = mDb.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(mPoolIDs));
        st.exchange(soci::use(mAssetAs));
        st.exchange(soci::use(mAssetBs));
        st.exchange(soci::use(mLiquidityPoolEntries));
        st.exchange(soci::use(mLastModifieds));
        st.define_and_bind();
        {
            auto timer = mDb.getUpsertTimer("liquiditypool");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mPoolIDs.size())
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
        std::string strPoolIDs, strAssetAs, strAssetBs, strLiquidityPoolEntry,
            strLastModifieds;

        PGconn* conn = pg->conn_;
        marshalToPGArray(conn, strPoolIDs, mPoolIDs);
        marshalToPGArray(conn, strAssetAs, mAssetAs);
        marshalToPGArray(conn, strAssetBs, mAssetBs);
        marshalToPGArray(conn, strLiquidityPoolEntry, mLiquidityPoolEntries);
        marshalToPGArray(conn, strLastModifieds, mLastModifieds);

        std::string sql = "WITH r AS "
                          "(SELECT unnest(:ids::TEXT[]), unnest(:v1::TEXT[]), "
                          "unnest(:v2::TEXT[]), unnest(:v3::TEXT[]), "
                          "unnest(:v4::INT[])) "
                          "INSERT INTO liquiditypool "
                          "(poolid, asseta, assetb, ledgerentry, lastmodified) "
                          "SELECT * FROM r "
                          "ON CONFLICT (poolid) DO UPDATE SET "
                          "asseta = excluded.asseta, "
                          "assetb = excluded.assetb, "
                          "ledgerentry = excluded.ledgerentry, "
                          "lastmodified = excluded.lastmodified";

        auto prep = mDb.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(strPoolIDs));
        st.exchange(soci::use(strAssetAs));
        st.exchange(soci::use(strAssetBs));
        st.exchange(soci::use(strLiquidityPoolEntry));
        st.exchange(soci::use(strLastModifieds));
        st.define_and_bind();
        {
            auto timer = mDb.getUpsertTimer("liquiditypool");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mPoolIDs.size())
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }
#endif
};

void
LedgerTxnRoot::Impl::bulkUpsertLiquidityPool(
    std::vector<EntryIterator> const& entries)
{
    BulkUpsertLiquidityPoolOperation op(mDatabase, entries);
    mDatabase.doDatabaseTypeSpecificOperation(op);
}

void
LedgerTxnRoot::Impl::dropLiquidityPools()
{
    throwIfChild();
    mEntryCache.clear();
    mBestOffers.clear();

    std::string coll = mDatabase.getSimpleCollationClause();

    mDatabase.getSession() << "DROP TABLE IF EXISTS liquiditypool;";
    mDatabase.getSession() << "CREATE TABLE liquiditypool ("
                           << "poolid       VARCHAR(48) " << coll
                           << " PRIMARY KEY, "
                           << "asseta       TEXT " << coll << " NOT NULL, "
                           << "assetb       TEXT " << coll << " NOT NULL, "
                           << "ledgerentry  TEXT NOT NULL, "
                           << "lastmodified INT NOT NULL);";
    mDatabase.getSession() << "CREATE INDEX liquiditypoolasseta "
                           << "ON liquiditypool(asseta);";
    mDatabase.getSession() << "CREATE INDEX liquiditypoolassetb "
                           << "ON liquiditypool(assetb);";
}
}
