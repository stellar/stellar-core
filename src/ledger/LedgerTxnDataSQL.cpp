// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "database/DatabaseTypeSpecificOperation.h"
#include "ledger/LedgerTxnImpl.h"
#include "util/Decoder.h"
#include "util/Logging.h"
#include "util/types.h"

namespace stellar
{

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::loadData(LedgerKey const& key) const
{
    std::string actIDStrKey = KeyUtils::toStrKey(key.data().accountID);
    std::string dataName = decoder::encode_b64(key.data().dataName);

    std::string dataValue;
    soci::indicator dataValueIndicator;

    LedgerEntry le;
    le.data.type(DATA);
    DataEntry& de = le.data.data();

    std::string sql = "SELECT datavalue, lastmodified "
                      "FROM accountdata "
                      "WHERE accountid= :id AND dataname= :dataname";
    auto prep = mDatabase.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::into(dataValue, dataValueIndicator));
    st.exchange(soci::into(le.lastModifiedLedgerSeq));
    st.exchange(soci::use(actIDStrKey));
    st.exchange(soci::use(dataName));
    st.define_and_bind();
    st.execute(true);
    if (!st.got_data())
    {
        return nullptr;
    }

    de.accountID = key.data().accountID;
    de.dataName = key.data().dataName;

    if (dataValueIndicator != soci::i_ok)
    {
        throw std::runtime_error("bad database state");
    }
    decoder::decode_b64(dataValue, de.dataValue);

    return std::make_shared<LedgerEntry const>(std::move(le));
}

class BulkUpsertDataOperation : public DatabaseTypeSpecificOperation<void>
{
    Database& mDB;
    std::vector<std::string> mAccountIDs;
    std::vector<std::string> mDataNames;
    std::vector<std::string> mDataValues;
    std::vector<int32_t> mLastModifieds;

    void
    accumulateEntry(LedgerEntry const& entry)
    {
        assert(entry.data.type() == DATA);
        DataEntry const& data = entry.data.data();
        mAccountIDs.emplace_back(KeyUtils::toStrKey(data.accountID));
        mDataNames.emplace_back(decoder::encode_b64(data.dataName));
        mDataValues.emplace_back(decoder::encode_b64(data.dataValue));
        mLastModifieds.emplace_back(
            unsignedToSigned(entry.lastModifiedLedgerSeq));
    }

  public:
    BulkUpsertDataOperation(Database& DB,
                            std::vector<LedgerEntry> const& entries)
        : mDB(DB)
    {
        for (auto const& e : entries)
        {
            accumulateEntry(e);
        }
    }

    BulkUpsertDataOperation(Database& DB,
                            std::vector<EntryIterator> const& entryIter)
        : mDB(DB)
    {
        for (auto const& e : entryIter)
        {
            assert(e.entryExists());
            accumulateEntry(e.entry());
        }
    }

    void
    doSociGenericOperation()
    {
        std::string sql = "INSERT INTO accountdata ( "
                          "accountid, dataname, datavalue, lastmodified "
                          ") VALUES ( "
                          ":id, :v1, :v2, :v3 "
                          ") ON CONFLICT (accountid, dataname) DO UPDATE SET "
                          "datavalue = excluded.datavalue, "
                          "lastmodified = excluded.lastmodified ";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(mAccountIDs));
        st.exchange(soci::use(mDataNames));
        st.exchange(soci::use(mDataValues));
        st.exchange(soci::use(mLastModifieds));
        st.define_and_bind();
        {
            auto timer = mDB.getUpsertTimer("data");
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
        std::string strAccountIDs, strDataNames, strDataValues,
            strLastModifieds;

        PGconn* conn = pg->conn_;
        marshalToPGArray(conn, strAccountIDs, mAccountIDs);
        marshalToPGArray(conn, strDataNames, mDataNames);
        marshalToPGArray(conn, strDataValues, mDataValues);
        marshalToPGArray(conn, strLastModifieds, mLastModifieds);
        std::string sql = "WITH r AS (SELECT "
                          "unnest(:ids::TEXT[]), "
                          "unnest(:v1::TEXT[]), "
                          "unnest(:v2::TEXT[]), "
                          "unnest(:v3::INT[]) "
                          ")"
                          "INSERT INTO accountdata ( "
                          "accountid, dataname, datavalue, lastmodified "
                          ") SELECT * FROM r "
                          "ON CONFLICT (accountid, dataname) DO UPDATE SET "
                          "datavalue = excluded.datavalue, "
                          "lastmodified = excluded.lastmodified ";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(strAccountIDs));
        st.exchange(soci::use(strDataNames));
        st.exchange(soci::use(strDataValues));
        st.exchange(soci::use(strLastModifieds));
        st.define_and_bind();
        {
            auto timer = mDB.getUpsertTimer("data");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mAccountIDs.size())
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }
#endif
};

class BulkDeleteDataOperation : public DatabaseTypeSpecificOperation<void>
{
    Database& mDB;
    LedgerTxnConsistency mCons;
    std::vector<std::string> mAccountIDs;
    std::vector<std::string> mDataNames;

  public:
    BulkDeleteDataOperation(Database& DB, LedgerTxnConsistency cons,
                            std::vector<EntryIterator> const& entries)
        : mDB(DB), mCons(cons)
    {
        for (auto const& e : entries)
        {
            assert(!e.entryExists());
            assert(e.key().type() == DATA);
            auto const& data = e.key().data();
            mAccountIDs.emplace_back(KeyUtils::toStrKey(data.accountID));
            mDataNames.emplace_back(decoder::encode_b64(data.dataName));
        }
    }

    void
    doSociGenericOperation()
    {
        std::string sql = "DELETE FROM accountdata WHERE accountid = :id AND "
                          " dataname = :v1 ";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(mAccountIDs));
        st.exchange(soci::use(mDataNames));
        st.define_and_bind();
        {
            auto timer = mDB.getDeleteTimer("data");
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
        std::string strAccountIDs;
        std::string strDataNames;
        PGconn* conn = pg->conn_;
        marshalToPGArray(conn, strAccountIDs, mAccountIDs);
        marshalToPGArray(conn, strDataNames, mDataNames);
        std::string sql =
            "WITH r AS ( SELECT "
            "unnest(:ids::TEXT[]),"
            "unnest(:v1::TEXT[])"
            " ) "
            "DELETE FROM accountdata WHERE (accountid, dataname) IN "
            "(SELECT * FROM r)";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(strAccountIDs));
        st.exchange(soci::use(strDataNames));
        st.define_and_bind();
        {
            auto timer = mDB.getDeleteTimer("data");
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
LedgerTxnRoot::Impl::bulkUpsertAccountData(
    std::vector<EntryIterator> const& entries)
{
    BulkUpsertDataOperation op(mDatabase, entries);
    mDatabase.doDatabaseTypeSpecificOperation(op);
}

void
LedgerTxnRoot::Impl::bulkDeleteAccountData(
    std::vector<EntryIterator> const& entries, LedgerTxnConsistency cons)
{
    BulkDeleteDataOperation op(mDatabase, cons, entries);
    mDatabase.doDatabaseTypeSpecificOperation(op);
}

void
LedgerTxnRoot::Impl::dropData()
{
    throwIfChild();
    mEntryCache.clear();
    mBestOffersCache.clear();

    mDatabase.getSession() << "DROP TABLE IF EXISTS accountdata;";
    mDatabase.getSession() << "CREATE TABLE accountdata"
                              "("
                              "accountid    VARCHAR(56)  NOT NULL,"
                              "dataname     VARCHAR(64)  NOT NULL,"
                              "datavalue    VARCHAR(112) NOT NULL,"
                              "lastmodified INT          NOT NULL,"
                              "PRIMARY KEY  (accountid, dataname)"
                              ");";
}

static std::vector<LedgerEntry>
loadDataToEncode(Database& db)
{
    std::string accountID, dataName, dataValue;
    uint32_t lastModified;

    std::string sql = "SELECT accountid, dataname, datavalue, lastmodified "
                      "FROM accountdata";

    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::into(accountID));
    st.exchange(soci::into(dataName));
    st.exchange(soci::into(dataValue));
    st.exchange(soci::into(lastModified));
    st.define_and_bind();
    st.execute(true);

    std::vector<LedgerEntry> res;
    while (st.got_data())
    {
        res.emplace_back();
        auto& le = res.back();
        le.data.type(DATA);

        auto& de = le.data.data();
        de.accountID = KeyUtils::fromStrKey<PublicKey>(accountID);
        de.dataName = dataName;
        decoder::decode_b64(dataValue, de.dataValue);
        le.lastModifiedLedgerSeq = lastModified;

        st.fetch();
    }
    return res;
}

void
LedgerTxnRoot::Impl::encodeDataNamesBase64()
{
    throwIfChild();
    mEntryCache.clear();
    mBestOffersCache.clear();

    CLOG(INFO, "Ledger")
        << "Loading all data entries from the accountdata table";
    auto dataToEncode = loadDataToEncode(mDatabase);

    // Note: The table must be recreated since dataname is part of the primary
    // key, so there could be a collision when updating it
    dropData();
    if (!mDatabase.isSqlite())
    {
        auto& session = mDatabase.getSession();
        session << "ALTER TABLE accountdata ALTER COLUMN dataname "
                   "SET DATA TYPE VARCHAR(88)";
    }
    if (!dataToEncode.empty())
    {
        BulkUpsertDataOperation op(mDatabase, dataToEncode);
        mDatabase.doDatabaseTypeSpecificOperation(op);
        CLOG(INFO, "Ledger")
            << "Wrote " << dataToEncode.size() << " data entries";
    }
}

class BulkLoadDataOperation
    : public DatabaseTypeSpecificOperation<std::vector<LedgerEntry>>
{
    Database& mDb;
    std::vector<std::string> mAccountIDs;
    std::vector<std::string> mDataNames;

    std::vector<LedgerEntry>
    executeAndFetch(soci::statement& st)
    {
        std::string accountID, dataName, dataValue;
        uint32_t lastModified;

        st.exchange(soci::into(accountID));
        st.exchange(soci::into(dataName));
        st.exchange(soci::into(dataValue));
        st.exchange(soci::into(lastModified));
        st.define_and_bind();
        {
            auto timer = mDb.getSelectTimer("data");
            st.execute(true);
        }

        std::vector<LedgerEntry> res;
        while (st.got_data())
        {
            res.emplace_back();
            auto& le = res.back();
            le.data.type(DATA);
            auto& de = le.data.data();

            de.accountID = KeyUtils::fromStrKey<PublicKey>(accountID);
            decoder::decode_b64(dataName, de.dataName);
            decoder::decode_b64(dataValue, de.dataValue);
            le.lastModifiedLedgerSeq = lastModified;

            st.fetch();
        }
        return res;
    }

  public:
    BulkLoadDataOperation(Database& db,
                          std::unordered_set<LedgerKey> const& keys)
        : mDb(db)
    {
        mAccountIDs.reserve(keys.size());
        mDataNames.reserve(keys.size());
        for (auto const& k : keys)
        {
            assert(k.type() == DATA);
            mAccountIDs.emplace_back(KeyUtils::toStrKey(k.data().accountID));
            mDataNames.emplace_back(decoder::encode_b64(k.data().dataName));
        }
    }

    virtual std::vector<LedgerEntry>
    doSqliteSpecificOperation(soci::sqlite3_session_backend* sq) override
    {
        assert(mAccountIDs.size() == mDataNames.size());

        std::vector<char const*> cstrAccountIDs;
        std::vector<char const*> cstrDataNames;
        cstrAccountIDs.reserve(mAccountIDs.size());
        cstrDataNames.reserve(mDataNames.size());
        for (size_t i = 0; i < mAccountIDs.size(); ++i)
        {
            cstrAccountIDs.emplace_back(mAccountIDs[i].c_str());
            cstrDataNames.emplace_back(mDataNames[i].c_str());
        }

        std::string sqlJoin =
            "SELECT x.value, y.value FROM "
            "(SELECT rowid, value FROM carray(?, ?, 'char*') ORDER BY rowid) "
            "AS x "
            "INNER JOIN (SELECT rowid, value FROM carray(?, ?, 'char*') ORDER "
            "BY rowid) AS y ON x.rowid = y.rowid";
        std::string sql =
            "WITH r AS (" + sqlJoin +
            ") SELECT accountid, dataname, datavalue, lastmodified "
            "FROM accountdata WHERE (accountid, dataname) IN r";

        auto prep = mDb.getPreparedStatement(sql);
        auto sqliteStatement = dynamic_cast<soci::sqlite3_statement_backend*>(
            prep.statement().get_backend());
        auto st = sqliteStatement->stmt_;

        sqlite3_reset(st);
        sqlite3_bind_pointer(st, 1, cstrAccountIDs.data(), "carray", 0);
        sqlite3_bind_int(st, 2, cstrAccountIDs.size());
        sqlite3_bind_pointer(st, 3, cstrDataNames.data(), "carray", 0);
        sqlite3_bind_int(st, 4, cstrDataNames.size());
        return executeAndFetch(prep.statement());
    }

#ifdef USE_POSTGRES
    std::vector<LedgerEntry>
    doPostgresSpecificOperation(soci::postgresql_session_backend* pg) override
    {
        assert(mAccountIDs.size() == mDataNames.size());

        std::string strAccountIDs;
        std::string strDataNames;
        marshalToPGArray(pg->conn_, strAccountIDs, mAccountIDs);
        marshalToPGArray(pg->conn_, strDataNames, mDataNames);

        std::string sql =
            "WITH r AS (SELECT unnest(:v1::TEXT[]), unnest(:v2::TEXT[])) "
            "SELECT accountid, dataname, datavalue, lastmodified "
            "FROM accountdata WHERE (accountid, dataname) IN (SELECT * FROM r)";

        auto prep = mDb.getPreparedStatement(sql);
        auto& st = prep.statement();
        st.exchange(soci::use(strAccountIDs));
        st.exchange(soci::use(strDataNames));
        return executeAndFetch(st);
    }
#endif
};

std::unordered_map<LedgerKey, std::shared_ptr<LedgerEntry const>>
LedgerTxnRoot::Impl::bulkLoadData(
    std::unordered_set<LedgerKey> const& keys) const
{
    BulkLoadDataOperation op(mDatabase, keys);
    return populateLoadedEntries(keys,
                                 mDatabase.doDatabaseTypeSpecificOperation(op));
}
}
