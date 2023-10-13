
// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxnImpl.h"
#include "ledger/NonSociRelatedException.h"
#include "main/Application.h"
#include "util/GlobalChecks.h"
#include "util/types.h"

namespace stellar
{

static void
throwIfNotTTL(LedgerEntryType type)
{
    if (type != TTL)
    {
        throw NonSociRelatedException("LedgerEntry is not TTL");
    }
}

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::loadTTL(LedgerKey const& key) const
{
    auto keyHash = toOpaqueBase64(key.ttl().keyHash);
    std::string ttlEntryStr;

    std::string sql = "SELECT ledgerentry "
                      "FROM ttl "
                      "WHERE keyhash = :keyHash";
    auto prep = mApp.getDatabase().getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::into(ttlEntryStr));
    st.exchange(soci::use(keyHash));
    st.define_and_bind();
    {
        auto timer = mApp.getDatabase().getSelectTimer("ttl");
        st.execute(true);
    }
    if (!st.got_data())
    {
        return nullptr;
    }

    LedgerEntry le;
    fromOpaqueBase64(le, ttlEntryStr);
    throwIfNotTTL(le.data.type());

    return std::make_shared<LedgerEntry const>(std::move(le));
}
class BulkLoadTTLOperation
    : public DatabaseTypeSpecificOperation<std::vector<LedgerEntry>>
{
    Database& mDb;
    std::vector<std::string> mKeyHashes;

    std::vector<LedgerEntry>
    executeAndFetch(soci::statement& st)
    {
        std::string ttlEntryStr;

        st.exchange(soci::into(ttlEntryStr));
        st.define_and_bind();
        {
            auto timer = mDb.getSelectTimer("ttl");
            st.execute(true);
        }

        std::vector<LedgerEntry> res;
        while (st.got_data())
        {
            res.emplace_back();
            auto& le = res.back();

            fromOpaqueBase64(le, ttlEntryStr);
            throwIfNotTTL(le.data.type());

            st.fetch();
        }
        return res;
    }

  public:
    BulkLoadTTLOperation(Database& db, UnorderedSet<LedgerKey> const& keys)
        : mDb(db)
    {
        mKeyHashes.reserve(keys.size());
        for (auto const& k : keys)
        {
            throwIfNotTTL(k.type());
            mKeyHashes.emplace_back(toOpaqueBase64(k.ttl().keyHash));
        }
    }

    std::vector<LedgerEntry>
    doSqliteSpecificOperation(soci::sqlite3_session_backend* sq) override
    {
        std::vector<char const*> cStrKeyHashes;
        cStrKeyHashes.reserve(mKeyHashes.size());
        for (auto const& h : mKeyHashes)
        {
            cStrKeyHashes.emplace_back(h.c_str());
        }
        std::string sql = "SELECT ledgerentry "
                          "FROM ttl "
                          "WHERE keyhash IN carray(?, ?, 'char*')";

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
        sqlite3_bind_pointer(st, 1, (void*)cStrKeyHashes.data(), "carray", 0);
        sqlite3_bind_int(st, 2, static_cast<int>(cStrKeyHashes.size()));
        return executeAndFetch(prep.statement());
    }

#ifdef USE_POSTGRES
    std::vector<LedgerEntry>
    doPostgresSpecificOperation(soci::postgresql_session_backend* pg) override
    {
        std::string strKeyHashes;
        marshalToPGArray(pg->conn_, strKeyHashes, mKeyHashes);

        std::string sql = "WITH r AS (SELECT unnest(:v1::TEXT[])) "
                          "SELECT ledgerentry "
                          "FROM ttl "
                          "WHERE (keyHash) IN (SELECT * from r)";

        auto prep = mDb.getPreparedStatement(sql);
        auto& st = prep.statement();
        st.exchange(soci::use(strKeyHashes));
        return executeAndFetch(st);
    }
#endif
};

UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>>
LedgerTxnRoot::Impl::bulkLoadTTL(UnorderedSet<LedgerKey> const& keys) const
{
    if (!keys.empty())
    {
        BulkLoadTTLOperation op(mApp.getDatabase(), keys);
        return populateLoadedEntries(
            keys, mApp.getDatabase().doDatabaseTypeSpecificOperation(op));
    }
    else
    {
        return {};
    }
}

class BulkDeleteTTLOperation : public DatabaseTypeSpecificOperation<void>
{
    Database& mDb;
    LedgerTxnConsistency mCons;
    std::vector<std::string> mKeyHashes;

  public:
    BulkDeleteTTLOperation(Database& db, LedgerTxnConsistency cons,
                           std::vector<EntryIterator> const& entries)
        : mDb(db), mCons(cons)
    {
        mKeyHashes.reserve(entries.size());
        for (auto const& e : entries)
        {
            releaseAssertOrThrow(!e.entryExists());
            throwIfNotTTL(e.key().ledgerKey().type());
            mKeyHashes.emplace_back(
                toOpaqueBase64(e.key().ledgerKey().ttl().keyHash));
        }
    }

    void
    doSociGenericOperation()
    {
        std::string sql = "DELETE FROM ttl WHERE keyhash = :id";
        auto prep = mDb.getPreparedStatement(sql);
        auto& st = prep.statement();
        st.exchange(soci::use(mKeyHashes));
        st.define_and_bind();
        {
            auto timer = mDb.getDeleteTimer("ttl");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mKeyHashes.size() &&
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
        std::string strKeyHashes;
        marshalToPGArray(pg->conn_, strKeyHashes, mKeyHashes);

        std::string sql = "WITH r AS (SELECT unnest(:v1::TEXT[])) "
                          "DELETE FROM ttl "
                          "WHERE keyHash IN (SELECT * FROM r)";

        auto prep = mDb.getPreparedStatement(sql);
        auto& st = prep.statement();
        st.exchange(soci::use(strKeyHashes));
        st.define_and_bind();
        {
            auto timer = mDb.getDeleteTimer("ttl");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mKeyHashes.size() &&
            mCons == LedgerTxnConsistency::EXACT)
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }
#endif
};

void
LedgerTxnRoot::Impl::bulkDeleteTTL(std::vector<EntryIterator> const& entries,
                                   LedgerTxnConsistency cons)
{
    BulkDeleteTTLOperation op(mApp.getDatabase(), cons, entries);
    mApp.getDatabase().doDatabaseTypeSpecificOperation(op);
}

class BulkUpsertTTLOperation : public DatabaseTypeSpecificOperation<void>
{
    Database& mDb;
    std::vector<std::string> mKeyHashes;
    std::vector<std::string> mTTLEntries;
    std::vector<int32_t> mLastModifieds;

    void
    accumulateEntry(LedgerEntry const& entry)
    {
        throwIfNotTTL(entry.data.type());

        mKeyHashes.emplace_back(toOpaqueBase64(entry.data.ttl().keyHash));
        mTTLEntries.emplace_back(toOpaqueBase64(entry));
        mLastModifieds.emplace_back(
            unsignedToSigned(entry.lastModifiedLedgerSeq));
    }

  public:
    BulkUpsertTTLOperation(Database& Db,
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
        std::string sql = "INSERT INTO ttl "
                          "(keyhash, ledgerentry, lastmodified) "
                          "VALUES "
                          "( :keyHash, :v1, :v2 ) "
                          "ON CONFLICT (keyhash) DO UPDATE SET "
                          "ledgerentry = excluded.ledgerentry, "
                          "lastmodified = excluded.lastmodified";

        auto prep = mDb.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(mKeyHashes));
        st.exchange(soci::use(mTTLEntries));
        st.exchange(soci::use(mLastModifieds));
        st.define_and_bind();
        {
            auto timer = mDb.getUpsertTimer("ttl");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mKeyHashes.size())
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
        std::string strKeyHashes, strTTLEntries, strLastModifieds;

        PGconn* conn = pg->conn_;
        marshalToPGArray(conn, strKeyHashes, mKeyHashes);
        marshalToPGArray(conn, strTTLEntries, mTTLEntries);
        marshalToPGArray(conn, strLastModifieds, mLastModifieds);

        std::string sql = "WITH r AS "
                          "(SELECT unnest(:v1::TEXT[]), "
                          "unnest(:v2::TEXT[]), unnest(:v3::INT[])) "
                          "INSERT INTO ttl "
                          "(keyHash, ledgerentry, lastmodified) "
                          "SELECT * FROM r "
                          "ON CONFLICT (keyhash) DO UPDATE SET "
                          "ledgerentry = excluded.ledgerentry, "
                          "lastmodified = excluded.lastmodified";

        auto prep = mDb.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(strKeyHashes));
        st.exchange(soci::use(strTTLEntries));
        st.exchange(soci::use(strLastModifieds));
        st.define_and_bind();
        {
            auto timer = mDb.getUpsertTimer("ttl");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mKeyHashes.size())
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }
#endif
};

void
LedgerTxnRoot::Impl::bulkUpsertTTL(std::vector<EntryIterator> const& entries)
{
    BulkUpsertTTLOperation op(mApp.getDatabase(), entries);
    mApp.getDatabase().doDatabaseTypeSpecificOperation(op);
}

void
LedgerTxnRoot::Impl::dropTTL(bool rebuild)
{
    throwIfChild();
    mEntryCache.clear();
    mBestOffers.clear();

    std::string coll = mApp.getDatabase().getSimpleCollationClause();

    mApp.getDatabase().getSession() << "DROP TABLE IF EXISTS ttl;";
    mApp.getDatabase().getSession()
        << "CREATE TABLE ttl ("
        << "keyhash   TEXT " << coll << " NOT NULL, "
        << "ledgerentry  TEXT " << coll << " NOT NULL, "
        << "lastmodified INT NOT NULL, "
        << "PRIMARY KEY (keyhash));";
    if (!mApp.getDatabase().isSqlite())
    {
        mApp.getDatabase().getSession() << "ALTER TABLE ttl "
                                        << "ALTER COLUMN keyhash "
                                        << "TYPE TEXT COLLATE \"C\";";
    }
}

}