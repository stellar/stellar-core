// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
#include "ledger/LedgerTxnImpl.h"
#include "ledger/NonSociRelatedException.h"
#include "main/Application.h"
#include "util/GlobalChecks.h"
#include "util/types.h"

namespace stellar
{

static void
throwIfNotContractCode(LedgerEntryType type)
{
    if (type != CONTRACT_CODE)
    {
        throw NonSociRelatedException("LedgerEntry is not a CONTRACT_CODE");
    }
}

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::loadContractCode(LedgerKey const& k) const
{
    auto hash = toOpaqueBase64(k.contractCode().hash);
    std::string contractCodeEntryStr;

    std::string sql = "SELECT ledgerentry "
                      "FROM contractcode "
                      "WHERE hash = :hash";
    auto prep = mApp.getDatabase().getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::into(contractCodeEntryStr));
    st.exchange(soci::use(hash));
    st.define_and_bind();
    {
        auto timer = mApp.getDatabase().getSelectTimer("contractcode");
        st.execute(true);
    }
    if (!st.got_data())
    {
        return nullptr;
    }

    LedgerEntry le;
    fromOpaqueBase64(le, contractCodeEntryStr);
    throwIfNotContractCode(le.data.type());

    return std::make_shared<LedgerEntry const>(std::move(le));
}

class BulkLoadContractCodeOperation
    : public DatabaseTypeSpecificOperation<std::vector<LedgerEntry>>
{
    Database& mDb;
    std::vector<std::string> mHashes;

    std::vector<LedgerEntry>
    executeAndFetch(soci::statement& st)
    {
        std::string contractCodeEntryStr;

        st.exchange(soci::into(contractCodeEntryStr));
        st.define_and_bind();
        {
            auto timer = mDb.getSelectTimer("contractcode");
            st.execute(true);
        }

        std::vector<LedgerEntry> res;
        while (st.got_data())
        {
            res.emplace_back();
            auto& le = res.back();

            fromOpaqueBase64(le, contractCodeEntryStr);
            throwIfNotContractCode(le.data.type());

            st.fetch();
        }
        return res;
    }

  public:
    BulkLoadContractCodeOperation(Database& db,
                                  UnorderedSet<LedgerKey> const& keys)
        : mDb(db)
    {
        mHashes.reserve(keys.size());
        for (auto const& k : keys)
        {
            throwIfNotContractCode(k.type());
            mHashes.emplace_back(toOpaqueBase64(k.contractCode().hash));
        }
    }

    std::vector<LedgerEntry>
    doSqliteSpecificOperation(soci::sqlite3_session_backend* sq) override
    {
        std::vector<char const*> cStrHashes;
        cStrHashes.reserve(mHashes.size());
        for (auto const& h : mHashes)
        {
            cStrHashes.emplace_back(h.c_str());
        }
        std::string sql = "SELECT ledgerentry "
                          "FROM contractcode "
                          "WHERE hash IN carray(?, ?, 'char*')";

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
        sqlite3_bind_pointer(st, 1, (void*)cStrHashes.data(), "carray", 0);
        sqlite3_bind_int(st, 2, static_cast<int>(cStrHashes.size()));
        return executeAndFetch(prep.statement());
    }

#ifdef USE_POSTGRES
    std::vector<LedgerEntry>
    doPostgresSpecificOperation(soci::postgresql_session_backend* pg) override
    {
        std::string strHashes;
        marshalToPGArray(pg->conn_, strHashes, mHashes);

        std::string sql = "WITH r AS (SELECT unnest(:v1::TEXT[])) "
                          "SELECT ledgerentry "
                          "FROM contractcode "
                          "WHERE (hash) IN (SELECT * from r)";

        auto prep = mDb.getPreparedStatement(sql);
        auto& st = prep.statement();
        st.exchange(soci::use(strHashes));
        return executeAndFetch(st);
    }
#endif
};

UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>>
LedgerTxnRoot::Impl::bulkLoadContractCode(
    UnorderedSet<LedgerKey> const& keys) const
{
    if (!keys.empty())
    {
        BulkLoadContractCodeOperation op(mApp.getDatabase(), keys);
        return populateLoadedEntries(
            keys, mApp.getDatabase().doDatabaseTypeSpecificOperation(op));
    }
    else
    {
        return {};
    }
}

class BulkDeleteContractCodeOperation
    : public DatabaseTypeSpecificOperation<void>
{
    Database& mDb;
    LedgerTxnConsistency mCons;
    std::vector<std::string> mHashes;

  public:
    BulkDeleteContractCodeOperation(Database& db, LedgerTxnConsistency cons,
                                    std::vector<EntryIterator> const& entries)
        : mDb(db), mCons(cons)
    {
        mHashes.reserve(entries.size());
        for (auto const& e : entries)
        {
            releaseAssert(!e.entryExists());
            throwIfNotContractCode(e.key().ledgerKey().type());
            mHashes.emplace_back(
                toOpaqueBase64(e.key().ledgerKey().contractCode().hash));
        }
    }

    void
    doSociGenericOperation()
    {
        std::string sql = "DELETE FROM contractcode WHERE hash = :id";
        auto prep = mDb.getPreparedStatement(sql);
        auto& st = prep.statement();
        st.exchange(soci::use(mHashes));
        st.define_and_bind();
        {
            auto timer = mDb.getDeleteTimer("contractcode");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mHashes.size() &&
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
        std::string strHashes;
        marshalToPGArray(pg->conn_, strHashes, mHashes);

        std::string sql = "WITH r AS (SELECT unnest(:v1::TEXT[])) "
                          "DELETE FROM contractcode "
                          "WHERE hash IN (SELECT * FROM r)";

        auto prep = mDb.getPreparedStatement(sql);
        auto& st = prep.statement();
        st.exchange(soci::use(strHashes));
        st.define_and_bind();
        {
            auto timer = mDb.getDeleteTimer("contractcode");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mHashes.size() &&
            mCons == LedgerTxnConsistency::EXACT)
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }
#endif
};

void
LedgerTxnRoot::Impl::bulkDeleteContractCode(
    std::vector<EntryIterator> const& entries, LedgerTxnConsistency cons)
{
    BulkDeleteContractCodeOperation op(mApp.getDatabase(), cons, entries);
    mApp.getDatabase().doDatabaseTypeSpecificOperation(op);
}

class BulkUpsertContractCodeOperation
    : public DatabaseTypeSpecificOperation<void>
{
    Database& mDb;
    std::vector<std::string> mHashes;
    std::vector<std::string> mContractCodeEntries;
    std::vector<int32_t> mLastModifieds;

    void
    accumulateEntry(LedgerEntry const& entry)
    {
        throwIfNotContractCode(entry.data.type());

        mHashes.emplace_back(toOpaqueBase64(entry.data.contractCode().hash));
        mContractCodeEntries.emplace_back(toOpaqueBase64(entry));
        mLastModifieds.emplace_back(
            unsignedToSigned(entry.lastModifiedLedgerSeq));
    }

  public:
    BulkUpsertContractCodeOperation(Database& Db,
                                    std::vector<EntryIterator> const& entryIter)
        : mDb(Db)
    {
        for (auto const& e : entryIter)
        {
            releaseAssert(e.entryExists());
            accumulateEntry(e.entry().ledgerEntry());
        }
    }

    void
    doSociGenericOperation()
    {
        std::string sql = "INSERT INTO contractCode "
                          "(hash, ledgerentry, lastmodified) "
                          "VALUES "
                          "( :hash, :v1, :v2 ) "
                          "ON CONFLICT (hash) DO UPDATE SET "
                          "ledgerentry = excluded.ledgerentry, "
                          "lastmodified = excluded.lastmodified";

        auto prep = mDb.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(mHashes));
        st.exchange(soci::use(mContractCodeEntries));
        st.exchange(soci::use(mLastModifieds));
        st.define_and_bind();
        {
            auto timer = mDb.getUpsertTimer("contractcode");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mHashes.size())
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
        std::string strHashes, strContractCodeEntries, strLastModifieds;

        PGconn* conn = pg->conn_;
        marshalToPGArray(conn, strHashes, mHashes);
        marshalToPGArray(conn, strContractCodeEntries, mContractCodeEntries);
        marshalToPGArray(conn, strLastModifieds, mLastModifieds);

        std::string sql = "WITH r AS "
                          "(SELECT unnest(:v1::TEXT[]), "
                          "unnest(:v1::TEXT[]), unnest(:v2::INT[])) "
                          "INSERT INTO contractcode "
                          "(hash, ledgerentry, lastmodified) "
                          "SELECT * FROM r "
                          "ON CONFLICT (hash) DO UPDATE SET "
                          "ledgerentry = excluded.ledgerentry, "
                          "lastmodified = excluded.lastmodified";

        auto prep = mDb.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(strHashes));
        st.exchange(soci::use(strContractCodeEntries));
        st.exchange(soci::use(strLastModifieds));
        st.define_and_bind();
        {
            auto timer = mDb.getUpsertTimer("contractcode");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mHashes.size())
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }
#endif
};

void
LedgerTxnRoot::Impl::bulkUpsertContractCode(
    std::vector<EntryIterator> const& entries)
{
    BulkUpsertContractCodeOperation op(mApp.getDatabase(), entries);
    mApp.getDatabase().doDatabaseTypeSpecificOperation(op);
}

void
LedgerTxnRoot::Impl::dropContractCode(bool rebuild)
{
    throwIfChild();
    mEntryCache.clear();
    mBestOffers.clear();

    std::string coll = mApp.getDatabase().getSimpleCollationClause();

    mApp.getDatabase().getSession() << "DROP TABLE IF EXISTS contractcode;";
    mApp.getDatabase().getSession()
        << "CREATE TABLE contractcode ("
        << "hash   TEXT " << coll << " NOT NULL, "
        << "ledgerentry  TEXT " << coll << " NOT NULL, "
        << "lastmodified INT NOT NULL, "
        << "PRIMARY KEY (hash));";
    if (!mApp.getDatabase().isSqlite())
    {
        mApp.getDatabase().getSession() << "ALTER TABLE contractcode "
                                        << "ALTER COLUMN hash "
                                        << "TYPE TEXT COLLATE \"C\";";
    }
}

}
#endif
