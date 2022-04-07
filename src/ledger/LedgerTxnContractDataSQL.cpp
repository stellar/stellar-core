// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "ledger/LedgerTxnImpl.h"
#include "ledger/NonSociRelatedException.h"
#include "util/GlobalChecks.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"
#include <Tracy.hpp>

namespace stellar
{
std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::loadContractData(LedgerKey const& key) const
{
    ZoneScoped;
    std::string owner = KeyUtils::toStrKey(key.contractData().owner);
    int64_t contractID = key.contractData().contractID;
    std::string k = toOpaqueBase64(key.contractData().key);

    std::string val;
    LedgerEntry le;
    le.data.type(CONTRACT_DATA);
    ContractDataEntry& cde = le.data.contractData();

    std::string sql =
        "SELECT val "
        "FROM contractdata "
        "WHERE owner = :owner AND contractid = :contractid AND key = :key";
    auto prep = mDatabase.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::into(val));
    st.exchange(soci::use(owner));
    st.exchange(soci::use(contractID));
    st.exchange(soci::use(k));
    st.define_and_bind();
    st.execute(true);
    if (!st.got_data())
    {
        return nullptr;
    }
    cde.owner = KeyUtils::fromStrKey<PublicKey>(owner);
    cde.contractID = contractID;
    cde.key.activate();
    *cde.key = *key.contractData().key;

    fromOpaqueBase64(cde.val, val);
    return std::make_shared<LedgerEntry const>(std::move(le));
}

class BulkUpsertContractDataOperation
    : public DatabaseTypeSpecificOperation<void>
{
    Database& mDB;
    std::vector<std::string> mOwners;
    std::vector<int64_t> mContractIDs;
    std::vector<std::string> mContractKeys;
    std::vector<std::string> mContractVals;
    std::vector<int32_t> mLastModifieds;

    void
    accumulateEntry(LedgerEntry const& entry)
    {
        releaseAssert(entry.data.type() == CONTRACT_DATA);
        ContractDataEntry const& data = entry.data.contractData();
        mOwners.emplace_back(KeyUtils::toStrKey(data.owner));
        mContractIDs.emplace_back(data.contractID);
        mContractKeys.emplace_back(toOpaqueBase64(data.key));
        mContractVals.emplace_back(toOpaqueBase64(data.val));
        mLastModifieds.emplace_back(
            unsignedToSigned(entry.lastModifiedLedgerSeq));
    }

  public:
    BulkUpsertContractDataOperation(Database& DB,
                                    std::vector<LedgerEntry> const& entries)
        : mDB(DB)
    {
        for (auto const& e : entries)
        {
            accumulateEntry(e);
        }
    }

    BulkUpsertContractDataOperation(Database& DB,
                                    std::vector<EntryIterator> const& entryIter)
        : mDB(DB)
    {
        for (auto const& e : entryIter)
        {
            releaseAssert(e.entryExists());
            releaseAssert(e.entry().type() ==
                          InternalLedgerEntryType::LEDGER_ENTRY);
            accumulateEntry(e.entry().ledgerEntry());
        }
    }

    void
    doSociGenericOperation()
    {
        std::string sql =
            "INSERT INTO contractdata ( "
            "owner, contractid, key, val, lastmodified"
            ") VALUES ( "
            ":owner, :contractid, :key, :val, :lastmodified"
            ") ON CONFLICT (owner, contractid, key) DO UPDATE SET "
            "val = excluded.val, "
            "lastmodified = excluded.lastmodified";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(mOwners));
        st.exchange(soci::use(mContractIDs));
        st.exchange(soci::use(mContractKeys));
        st.exchange(soci::use(mContractVals));
        st.exchange(soci::use(mLastModifieds));
        st.define_and_bind();
        {
            auto timer = mDB.getUpsertTimer("contractdata");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mOwners.size())
        {
            throw std::runtime_error("Could not update contractdata in SQL");
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
        std::string strOwners, strContractIDs, strContractKeys, strContractVals,
            strLastModifieds;

        PGconn* conn = pg->conn_;
        marshalToPGArray(conn, strOwners, mOwners);
        marshalToPGArray(conn, strContractIDs, mContractIDs);
        marshalToPGArray(conn, strContractKeys, mContractKeys);
        marshalToPGArray(conn, strContractVals, mContractVals);
        marshalToPGArray(conn, strLastModifieds, mLastModifieds);
        std::string sql = "WITH r AS (SELECT "
                          "unnest(:owners::TEXT[]), "
                          "unnest(:contractids::BIGINT[]), "
                          "unnest(:contractkeys::TEXT[]), "
                          "unnest(:contractvals::TEXT[]), "
                          "unnest(:lastmodifieds::INT[]) "
                          ")"
                          "INSERT INTO contractdata ( "
                          "owner, contractid, key, val, lastmodified "
                          ") SELECT * FROM r "
                          "ON CONFLICT (owner, contractid, key) DO UPDATE SET "
                          "val = excluded.val, "
                          "lastmodified = excluded.lastmodified";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(strOwners));
        st.exchange(soci::use(strContractIDs));
        st.exchange(soci::use(strContractKeys));
        st.exchange(soci::use(strContractVals));
        st.exchange(soci::use(strLastModifieds));
        st.define_and_bind();
        {
            auto timer = mDB.getUpsertTimer("contractdata");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mOwners.size())
        {
            throw std::runtime_error("Could not update contractdata in SQL");
        }
    }
#endif
};

class BulkDeleteContractDataOperation
    : public DatabaseTypeSpecificOperation<void>
{
    Database& mDB;
    LedgerTxnConsistency mCons;
    std::vector<std::string> mOwners;
    std::vector<int64_t> mContractIDs;
    std::vector<std::string> mKeys;

  public:
    BulkDeleteContractDataOperation(Database& DB, LedgerTxnConsistency cons,
                                    std::vector<EntryIterator> const& entries)
        : mDB(DB), mCons(cons)
    {
        for (auto const& e : entries)
        {
            releaseAssert(!e.entryExists());
            releaseAssert(e.key().type() ==
                          InternalLedgerEntryType::LEDGER_ENTRY);
            releaseAssert(e.key().ledgerKey().type() == CONTRACT_DATA);
            auto const& data = e.key().ledgerKey().contractData();
            mOwners.emplace_back(KeyUtils::toStrKey(data.owner));
            mContractIDs.emplace_back(data.contractID);
            mKeys.emplace_back(toOpaqueBase64(data.key));
        }
    }

    void
    doSociGenericOperation()
    {
        std::string sql = "DELETE FROM contractdata WHERE owner = :owner AND "
                          " contractid = :contractid AND key = :key";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(mOwners));
        st.exchange(soci::use(mContractIDs));
        st.exchange(soci::use(mKeys));
        st.define_and_bind();
        {
            auto timer = mDB.getDeleteTimer("contractdata");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mOwners.size() &&
            mCons == LedgerTxnConsistency::EXACT)
        {
            throw std::runtime_error("Could not update contractdata in SQL");
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
        std::string strOwners;
        std::string strContractIDs;
        std::string strKeys;
        PGconn* conn = pg->conn_;
        marshalToPGArray(conn, strOwners, mOwners);
        marshalToPGArray(conn, strContractIDs, mContractIDs);
        marshalToPGArray(conn, strKeys, mKeys);
        std::string sql =
            "WITH r AS ( SELECT "
            "unnest(:owners::TEXT[]),"
            "unnest(:contractids::BIGINT[]),"
            "unnest(:keys::TEXT[]),"
            " ) "
            "DELETE FROM contractdata WHERE (owner, contractid, key) IN "
            "(SELECT * FROM r)";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(strOwners));
        st.exchange(soci::use(strContractIDs));
        st.exchange(soci::use(strKeys));
        st.define_and_bind();
        {
            auto timer = mDB.getDeleteTimer("contractdata");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mOwners.size() &&
            mCons == LedgerTxnConsistency::EXACT)
        {
            throw std::runtime_error("Could not update contractdata in SQL");
        }
    }
#endif
};

void
LedgerTxnRoot::Impl::bulkUpsertContractData(
    std::vector<EntryIterator> const& entries)
{
    ZoneScoped;
    ZoneValue(static_cast<int64_t>(entries.size()));
    BulkUpsertContractDataOperation op(mDatabase, entries);
    mDatabase.doDatabaseTypeSpecificOperation(op);
}

void
LedgerTxnRoot::Impl::bulkDeleteContractData(
    std::vector<EntryIterator> const& entries, LedgerTxnConsistency cons)
{
    ZoneScoped;
    ZoneValue(static_cast<int64_t>(entries.size()));
    BulkDeleteContractDataOperation op(mDatabase, cons, entries);
    mDatabase.doDatabaseTypeSpecificOperation(op);
}

class BulkLoadContractDataOperation
    : public DatabaseTypeSpecificOperation<std::vector<LedgerEntry>>
{
    Database& mDb;
    std::vector<std::string> mOwners;
    std::vector<int64_t> mContractIDs;
    std::vector<std::string> mKeys;

    std::vector<LedgerEntry>
    executeAndFetch(soci::statement& st)
    {
        std::string owner, key, val;
        int64_t contractID;
        uint32_t lastModified;

        st.exchange(soci::into(owner));
        st.exchange(soci::into(contractID));
        st.exchange(soci::into(key));
        st.exchange(soci::into(val));
        st.exchange(soci::into(lastModified));
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
            le.data.type(CONTRACT_DATA);
            auto& cc = le.data.contractData();

            cc.owner = KeyUtils::fromStrKey<PublicKey>(owner);
            cc.contractID = contractID;
            cc.key.activate();
            cc.val.activate();
            fromOpaqueBase64(*cc.key, key);
            fromOpaqueBase64(*cc.val, val);
            le.lastModifiedLedgerSeq = lastModified;
            st.fetch();
        }
        return res;
    }

  public:
    BulkLoadContractDataOperation(Database& db,
                                  UnorderedSet<LedgerKey> const& keys)
        : mDb(db)
    {
        mOwners.reserve(keys.size());
        mContractIDs.reserve(keys.size());
        mKeys.reserve(keys.size());
        for (auto const& k : keys)
        {
            releaseAssert(k.type() == CONTRACT_DATA);
            mOwners.emplace_back(KeyUtils::toStrKey(k.contractData().owner));
            mContractIDs.emplace_back(k.contractData().contractID);
            mKeys.emplace_back(toOpaqueBase64(k.contractData().key));
        }
    }

    virtual std::vector<LedgerEntry>
    doSqliteSpecificOperation(soci::sqlite3_session_backend* sq) override
    {
        releaseAssert(mOwners.size() == mContractIDs.size());
        releaseAssert(mOwners.size() == mKeys.size());

        std::vector<char const*> cstrOwners;
        cstrOwners.reserve(mOwners.size());
        for (size_t i = 0; i < mOwners.size(); ++i)
        {
            cstrOwners.emplace_back(mOwners[i].c_str());
        }

        std::string sqlJoin = "SELECT x.value, y.value, z.value FROM "
                              "(SELECT rowid, value FROM carray(?, ?, 'char*') "
                              "ORDER BY rowid) AS x "
                              "INNER JOIN "
                              "(SELECT rowid, value FROM carray(?, ?, 'int64') "
                              "ORDER BY rowid) AS y "
                              "ON x.rowid = y.rowid "
                              "INNER JOIN "
                              "(SELECT rowid, value FROM carray(?, ?, 'char*') "
                              "ORDER BY rowid) AS z "
                              "ON x.rowid = z.rowid ";
        std::string sql =
            "WITH r AS (" + sqlJoin +
            ") SELECT owner, contractid, key, val, "
            "lastmodified "
            "FROM contractdata WHERE (owner, contractid, key) IN r";

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
        sqlite3_bind_pointer(st, 1, cstrOwners.data(), "carray", 0);
        sqlite3_bind_int(st, 2, static_cast<int>(cstrOwners.size()));
        sqlite3_bind_pointer(st, 3, mContractIDs.data(), "carray", 0);
        sqlite3_bind_int(st, 4, static_cast<int>(mContractIDs.size()));
        sqlite3_bind_pointer(st, 5, mKeys.data(), "carray", 0);
        sqlite3_bind_int(st, 6, static_cast<int>(mKeys.size()));
        return executeAndFetch(prep.statement());
    }

#ifdef USE_POSTGRES
    std::vector<LedgerEntry>
    doPostgresSpecificOperation(soci::postgresql_session_backend* pg) override
    {
        releaseAssert(mOwners.size() == mContractIDs.size());

        std::string strOwners;
        std::string strContractIDs;
        std::string strKeys;
        marshalToPGArray(pg->conn_, strOwners, mOwners);
        marshalToPGArray(pg->conn_, strContractIDs, mContractIDs);
        marshalToPGArray(pg->conn_, strKeys, mKeys);

        std::string sql = "WITH r AS (SELECT unnest(:owners::TEXT[]), "
                          "unnest(:contractids::BIGINT[])) "
                          "unnest(:keys::TEXT[])) "
                          "SELECT owner, contractid, key, val, lastmodified "
                          "FROM contractdata WHERE (owner, contractid, key) IN "
                          "(SELECT * FROM r)";

        auto prep = mDb.getPreparedStatement(sql);
        auto& st = prep.statement();
        st.exchange(soci::use(strOwners));
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
    ZoneScoped;
    ZoneValue(static_cast<int64_t>(keys.size()));
    if (!keys.empty())
    {
        BulkLoadContractDataOperation op(mDatabase, keys);
        return populateLoadedEntries(
            keys, mDatabase.doDatabaseTypeSpecificOperation(op));
    }
    else
    {
        return {};
    }
}

void
LedgerTxnRoot::Impl::dropContractData()
{
    throwIfChild();
    mEntryCache.clear();

    std::string coll = mDatabase.getSimpleCollationClause();

    mDatabase.getSession() << "DROP TABLE IF EXISTS contractdata;";
    mDatabase.getSession() << "CREATE TABLE contractdata"
                           << "("
                           << "owner    VARCHAR(56) " << coll << " NOT NULL,"
                           << "contractid   BIGINT       NOT NULL,"
                           << "key          TEXT         NOT NULL,"
                           << "val          TEXT         NOT NULL,"
                              "lastmodified INT          NOT NULL,"
                              "PRIMARY KEY  (owner, contractid, key)"
                              ");";
    if (!mDatabase.isSqlite())
    {
        mDatabase.getSession() << "ALTER TABLE contractdata "
                               << "ALTER COLUMN owner "
                               << "TYPE VARCHAR(56) COLLATE \"C\"";
    }
}
}
