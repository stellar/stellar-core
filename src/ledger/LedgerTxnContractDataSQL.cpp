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
throwIfNotContractData(LedgerEntryType type)
{
    if (type != CONTRACT_DATA)
    {
        throw NonSociRelatedException("LedgerEntry is not a CONTRACT_DATA");
    }
}

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::loadContractData(LedgerKey const& k) const
{
    auto contractID = toOpaqueBase64(k.contractData().contractID);
    auto key = toOpaqueBase64(k.contractData().key);
    std::string contractDataEntryStr;

    std::string sql = "SELECT ledgerentry "
                      "FROM contractdata "
                      "WHERE contractID = :contractID AND key = :key";
    auto prep = mApp.getDatabase().getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::into(contractDataEntryStr));
    st.exchange(soci::use(contractID));
    st.exchange(soci::use(key));
    st.define_and_bind();
    {
        auto timer = mApp.getDatabase().getSelectTimer("contractdata");
        st.execute(true);
    }
    if (!st.got_data())
    {
        return nullptr;
    }

    LedgerEntry le;
    fromOpaqueBase64(le, contractDataEntryStr);
    throwIfNotContractData(le.data.type());

    return std::make_shared<LedgerEntry const>(std::move(le));
}

class BulkLoadContractDataOperation
    : public DatabaseTypeSpecificOperation<std::vector<LedgerEntry>>
{
    Database& mDb;
    std::vector<std::string> mContractIDs;
    std::vector<std::string> mKeys;

    std::vector<LedgerEntry>
    executeAndFetch(soci::statement& st)
    {
        std::string contractDataEntryStr;

        st.exchange(soci::into(contractDataEntryStr));
        st.define_and_bind();
        {
            auto timer = mDb.getSelectTimer("contractdata");
            st.execute(true);
        }

        std::vector<LedgerEntry> res;
        while (st.got_data())
        {
            res.emplace_back();
            auto& le = res.back();

            fromOpaqueBase64(le, contractDataEntryStr);
            throwIfNotContractData(le.data.type());

            st.fetch();
        }
        return res;
    }

  public:
    BulkLoadContractDataOperation(Database& db,
                                  UnorderedSet<LedgerKey> const& keys)
        : mDb(db)
    {
        mContractIDs.reserve(keys.size());
        mKeys.reserve(keys.size());
        for (auto const& k : keys)
        {
            throwIfNotContractData(k.type());
            mContractIDs.emplace_back(
                toOpaqueBase64(k.contractData().contractID));
            mKeys.emplace_back(toOpaqueBase64(k.contractData().key));
        }
    }

    std::vector<LedgerEntry>
    doSqliteSpecificOperation(soci::sqlite3_session_backend* sq) override
    {
        std::vector<char const*> cStrContractIDs, cStrKeys;
        cStrContractIDs.reserve(mContractIDs.size());
        cStrKeys.reserve(cStrKeys.size());
        for (auto const& cid : mContractIDs)
        {
            cStrContractIDs.emplace_back(cid.c_str());
        }
        for (auto const& key : mKeys)
        {
            cStrKeys.emplace_back(key.c_str());
        }
        std::string sqlJoin =
            "SELECT x.value, y.value FROM "
            "(SELECT rowid, value FROM carray(?, ?, 'char*') ORDER BY rowid) "
            "AS x "
            "INNER JOIN "
            "(SELECT rowid, value FROM carray(?, ?, 'char*') ORDER BY rowid) "
            "AS y "
            "ON x.rowid = y.rowid ";

        std::string sql = "WITH r AS  (" + sqlJoin +
                          ") "
                          "SELECT ledgerentry "
                          "FROM contractdata "
                          "WHERE (contractid, key) IN r";

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
        sqlite3_bind_pointer(st, 1, (void*)cStrContractIDs.data(), "carray", 0);
        sqlite3_bind_int(st, 2, static_cast<int>(mContractIDs.size()));
        sqlite3_bind_pointer(st, 3, (void*)cStrKeys.data(), "carray", 0);
        sqlite3_bind_int(st, 4, static_cast<int>(mKeys.size()));
        return executeAndFetch(prep.statement());
    }

#ifdef USE_POSTGRES
    std::vector<LedgerEntry>
    doPostgresSpecificOperation(soci::postgresql_session_backend* pg) override
    {
        std::string strContractIDs, strKeys;
        marshalToPGArray(pg->conn_, strContractIDs, mContractIDs);
        marshalToPGArray(pg->conn_, strKeys, mKeys);

        std::string sql =
            "WITH r AS (SELECT unnest(:v1::TEXT[]), unnest(:v1::TEXT[])) "
            "SELECT ledgerentry "
            "FROM contractdata "
            "WHERE (contractid, key) IN (SELECT * from r)";

        auto prep = mDb.getPreparedStatement(sql);
        auto& st = prep.statement();
        st.exchange(soci::use(strContractIDs));
        st.exchange(soci::use(strKeys));
        return executeAndFetch(st);
    }
#endif
};

UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>>
LedgerTxnRoot::Impl::bulkLoadContractData(
    UnorderedSet<LedgerKey> const& keys) const
{
    if (!keys.empty())
    {
        BulkLoadContractDataOperation op(mApp.getDatabase(), keys);
        return populateLoadedEntries(
            keys, mApp.getDatabase().doDatabaseTypeSpecificOperation(op));
    }
    else
    {
        return {};
    }
}

class BulkDeleteContractDataOperation
    : public DatabaseTypeSpecificOperation<void>
{
    Database& mDb;
    LedgerTxnConsistency mCons;
    std::vector<std::string> mContractIDs;
    std::vector<std::string> mKeys;

  public:
    BulkDeleteContractDataOperation(Database& db, LedgerTxnConsistency cons,
                                    std::vector<EntryIterator> const& entries)
        : mDb(db), mCons(cons)
    {
        mContractIDs.reserve(entries.size());
        for (auto const& e : entries)
        {
            releaseAssert(!e.entryExists());
            throwIfNotContractData(e.key().ledgerKey().type());
            mContractIDs.emplace_back(
                toOpaqueBase64(e.key().ledgerKey().contractData().contractID));
            mKeys.emplace_back(
                toOpaqueBase64(e.key().ledgerKey().contractData().key));
        }
    }

    void
    doSociGenericOperation()
    {
        std::string sql = "DELETE FROM contractdata WHERE contractid = :id "
                          "AND key = :key";
        auto prep = mDb.getPreparedStatement(sql);
        auto& st = prep.statement();
        st.exchange(soci::use(mContractIDs));
        st.exchange(soci::use(mKeys));
        st.define_and_bind();
        {
            auto timer = mDb.getDeleteTimer("contractdata");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) !=
                mContractIDs.size() &&
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
        std::string strContractIDs, strKeys;
        marshalToPGArray(pg->conn_, strContractIDs, mContractIDs);
        marshalToPGArray(pg->conn_, strKeys, mKeys);

        std::string sql =
            "WITH r AS (SELECT unnest(:v1::TEXT[]), unnest(:v1::TEXT[])) "
            "DELETE FROM contractdata "
            "WHERE (contractid, key) IN (SELECT * FROM r)";

        auto prep = mDb.getPreparedStatement(sql);
        auto& st = prep.statement();
        st.exchange(soci::use(strContractIDs));
        st.exchange(soci::use(strKeys));
        st.define_and_bind();
        {
            auto timer = mDb.getDeleteTimer("contractdata");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) !=
                mContractIDs.size() &&
            mCons == LedgerTxnConsistency::EXACT)
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }
#endif
};

void
LedgerTxnRoot::Impl::bulkDeleteContractData(
    std::vector<EntryIterator> const& entries, LedgerTxnConsistency cons)
{
    BulkDeleteContractDataOperation op(mApp.getDatabase(), cons, entries);
    mApp.getDatabase().doDatabaseTypeSpecificOperation(op);
}

class BulkUpsertContractDataOperation
    : public DatabaseTypeSpecificOperation<void>
{
    Database& mDb;
    std::vector<std::string> mContractIDs;
    std::vector<std::string> mKeys;
    std::vector<std::string> mContractDataEntries;
    std::vector<int32_t> mLastModifieds;

    void
    accumulateEntry(LedgerEntry const& entry)
    {
        throwIfNotContractData(entry.data.type());

        mContractIDs.emplace_back(
            toOpaqueBase64(entry.data.contractData().contractID));
        mKeys.emplace_back(toOpaqueBase64(entry.data.contractData().key));
        mContractDataEntries.emplace_back(toOpaqueBase64(entry));
        mLastModifieds.emplace_back(
            unsignedToSigned(entry.lastModifiedLedgerSeq));
    }

  public:
    BulkUpsertContractDataOperation(Database& Db,
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
        std::string sql = "INSERT INTO contractData "
                          "(contractid, key, ledgerentry, lastmodified) "
                          "VALUES "
                          "( :id, :key, :v1, :v2 ) "
                          "ON CONFLICT (contractid, key) DO UPDATE SET "
                          "ledgerentry = excluded.ledgerentry, "
                          "lastmodified = excluded.lastmodified";

        auto prep = mDb.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(mContractIDs));
        st.exchange(soci::use(mKeys));
        st.exchange(soci::use(mContractDataEntries));
        st.exchange(soci::use(mLastModifieds));
        st.define_and_bind();
        {
            auto timer = mDb.getUpsertTimer("contractdata");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mContractIDs.size())
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
        std::string strContractIDs, strKeys, strContractDataEntries,
            strLastModifieds;

        PGconn* conn = pg->conn_;
        marshalToPGArray(conn, strContractIDs, mContractIDs);
        marshalToPGArray(conn, strKeys, mKeys);
        marshalToPGArray(conn, strContractDataEntries, mContractDataEntries);
        marshalToPGArray(conn, strLastModifieds, mLastModifieds);

        std::string sql = "WITH r AS "
                          "(SELECT unnest(:ids::TEXT[]), unnest(:v1::TEXT[]), "
                          "unnest(:v1::TEXT[]), unnest(:v2::INT[])) "
                          "INSERT INTO contractdata "
                          "(contractid, key, ledgerentry, lastmodified) "
                          "SELECT * FROM r "
                          "ON CONFLICT (contractid,key) DO UPDATE SET "
                          "ledgerentry = excluded.ledgerentry, "
                          "lastmodified = excluded.lastmodified";

        auto prep = mDb.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(strContractIDs));
        st.exchange(soci::use(strKeys));
        st.exchange(soci::use(strContractDataEntries));
        st.exchange(soci::use(strLastModifieds));
        st.define_and_bind();
        {
            auto timer = mDb.getUpsertTimer("contractdata");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mContractIDs.size())
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }
#endif
};

void
LedgerTxnRoot::Impl::bulkUpsertContractData(
    std::vector<EntryIterator> const& entries)
{
    BulkUpsertContractDataOperation op(mApp.getDatabase(), entries);
    mApp.getDatabase().doDatabaseTypeSpecificOperation(op);
}

void
LedgerTxnRoot::Impl::dropContractData(bool rebuild)
{
    throwIfChild();
    mEntryCache.clear();
    mBestOffers.clear();

    mApp.getDatabase().getSession() << "DROP TABLE IF EXISTS contractdata;";

    if (rebuild)
    {
        std::string coll = mApp.getDatabase().getSimpleCollationClause();
        mApp.getDatabase().getSession()
            << "CREATE TABLE contractdata ("
            << "contractid   TEXT " << coll << " NOT NULL, "
            << "key TEXT " << coll << " NOT NULL, "
            << "ledgerentry  TEXT " << coll << " NOT NULL, "
            << "lastmodified INT NOT NULL, "
            << "PRIMARY KEY  (contractid, key));";
        if (!mApp.getDatabase().isSqlite())
        {
            mApp.getDatabase().getSession() << "ALTER TABLE contractdata "
                                            << "ALTER COLUMN contractid "
                                            << "TYPE TEXT COLLATE \"C\","
                                            << "ALTER COLUMN key "
                                            << "TYPE TEXT COLLATE \"C\";";
        }
    }
}

}
#endif