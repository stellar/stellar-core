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
#include <Tracy.hpp>

namespace stellar
{

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::loadData(LedgerKey const& key) const
{
    ZoneScoped;
    std::string actIDStrKey = KeyUtils::toStrKey(key.data().accountID);
    std::string dataName = decoder::encode_b64(key.data().dataName);

    std::string dataValue;
    soci::indicator dataValueIndicator;
    std::string extensionStr;
    soci::indicator extensionInd;
    std::string ledgerExtStr;
    soci::indicator ledgerExtInd;

    LedgerEntry le;
    le.data.type(DATA);
    DataEntry& de = le.data.data();

    std::string sql = "SELECT datavalue, lastmodified, extension, "
                      "ledgerext "
                      "FROM accountdata "
                      "WHERE accountid= :id AND dataname= :dataname";
    auto prep = mDatabase.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::into(dataValue, dataValueIndicator));
    st.exchange(soci::into(le.lastModifiedLedgerSeq));
    st.exchange(soci::into(extensionStr, extensionInd));
    st.exchange(soci::into(ledgerExtStr, ledgerExtInd));
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

    decodeOpaqueXDR(extensionStr, extensionInd, de.ext);

    decodeOpaqueXDR(ledgerExtStr, ledgerExtInd, le.ext);

    return std::make_shared<LedgerEntry const>(std::move(le));
}

class BulkUpsertDataOperation : public DatabaseTypeSpecificOperation<void>
{
    Database& mDB;
    std::vector<std::string> mAccountIDs;
    std::vector<std::string> mDataNames;
    std::vector<std::string> mDataValues;
    std::vector<int32_t> mLastModifieds;
    std::vector<std::string> mExtensions;
    std::vector<std::string> mLedgerExtensions;

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
        mExtensions.emplace_back(
            decoder::encode_b64(xdr::xdr_to_opaque(data.ext)));
        mLedgerExtensions.emplace_back(
            decoder::encode_b64(xdr::xdr_to_opaque(entry.ext)));
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
            assert(e.entry().type() == InternalLedgerEntryType::LEDGER_ENTRY);
            accumulateEntry(e.entry().ledgerEntry());
        }
    }

    void
    doSociGenericOperation()
    {
        std::string sql =
            "INSERT INTO accountdata ( "
            "accountid, dataname, datavalue, lastmodified, extension, "
            "ledgerext "
            ") VALUES ( "
            ":id, :v1, :v2, :v3, :v4, :v5 "
            ") ON CONFLICT (accountid, dataname) DO UPDATE SET "
            "datavalue = excluded.datavalue, "
            "lastmodified = excluded.lastmodified, "
            "extension = excluded.extension, "
            "ledgerext = excluded.ledgerext";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(mAccountIDs));
        st.exchange(soci::use(mDataNames));
        st.exchange(soci::use(mDataValues));
        st.exchange(soci::use(mLastModifieds));
        st.exchange(soci::use(mExtensions));
        st.exchange(soci::use(mLedgerExtensions));
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
            strLastModifieds, strExtensions, strLedgerExtensions;

        PGconn* conn = pg->conn_;
        marshalToPGArray(conn, strAccountIDs, mAccountIDs);
        marshalToPGArray(conn, strDataNames, mDataNames);
        marshalToPGArray(conn, strDataValues, mDataValues);
        marshalToPGArray(conn, strLastModifieds, mLastModifieds);
        marshalToPGArray(conn, strExtensions, mExtensions);
        marshalToPGArray(conn, strLedgerExtensions, mLedgerExtensions);
        std::string sql =
            "WITH r AS (SELECT "
            "unnest(:ids::TEXT[]), "
            "unnest(:v1::TEXT[]), "
            "unnest(:v2::TEXT[]), "
            "unnest(:v3::INT[]), "
            "unnest(:v4::TEXT[]), "
            "unnest(:v5::TEXT[]) "
            ")"
            "INSERT INTO accountdata ( "
            "accountid, dataname, datavalue, lastmodified, extension, "
            "ledgerext "
            ") SELECT * FROM r "
            "ON CONFLICT (accountid, dataname) DO UPDATE SET "
            "datavalue = excluded.datavalue, "
            "lastmodified = excluded.lastmodified, "
            "extension = excluded.extension, "
            "ledgerext = excluded.ledgerext";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(strAccountIDs));
        st.exchange(soci::use(strDataNames));
        st.exchange(soci::use(strDataValues));
        st.exchange(soci::use(strLastModifieds));
        st.exchange(soci::use(strExtensions));
        st.exchange(soci::use(strLedgerExtensions));
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
            assert(e.key().type() == InternalLedgerEntryType::LEDGER_ENTRY);
            assert(e.key().ledgerKey().type() == DATA);
            auto const& data = e.key().ledgerKey().data();
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
    ZoneScoped;
    ZoneValue(static_cast<int64_t>(entries.size()));
    BulkUpsertDataOperation op(mDatabase, entries);
    mDatabase.doDatabaseTypeSpecificOperation(op);
}

void
LedgerTxnRoot::Impl::bulkDeleteAccountData(
    std::vector<EntryIterator> const& entries, LedgerTxnConsistency cons)
{
    ZoneScoped;
    ZoneValue(static_cast<int64_t>(entries.size()));
    BulkDeleteDataOperation op(mDatabase, cons, entries);
    mDatabase.doDatabaseTypeSpecificOperation(op);
}

void
LedgerTxnRoot::Impl::dropData()
{
    throwIfChild();
    mEntryCache.clear();
    mBestOffers.clear();

    std::string coll = mDatabase.getSimpleCollationClause();

    mDatabase.getSession() << "DROP TABLE IF EXISTS accountdata;";
    mDatabase.getSession() << "CREATE TABLE accountdata"
                           << "("
                           << "accountid    VARCHAR(56) " << coll
                           << " NOT NULL,"
                           << "dataname     VARCHAR(88) " << coll
                           << " NOT NULL,"
                           << "datavalue    VARCHAR(112) NOT NULL,"
                              "lastmodified INT          NOT NULL,"
                              "PRIMARY KEY  (accountid, dataname)"
                              ");";
    if (!mDatabase.isSqlite())
    {
        mDatabase.getSession() << "ALTER TABLE accountdata "
                               << "ALTER COLUMN accountid "
                               << "TYPE VARCHAR(56) COLLATE \"C\", "
                               << "ALTER COLUMN dataname "
                               << "TYPE VARCHAR(88) COLLATE \"C\"";
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
        std::string extension;
        soci::indicator extensionInd;
        std::string ledgerExtension;
        soci::indicator ledgerExtInd;

        st.exchange(soci::into(accountID));
        st.exchange(soci::into(dataName));
        st.exchange(soci::into(dataValue));
        st.exchange(soci::into(lastModified));
        st.exchange(soci::into(extension, extensionInd));
        st.exchange(soci::into(ledgerExtension, ledgerExtInd));
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

            decodeOpaqueXDR(extension, extensionInd, de.ext);

            decodeOpaqueXDR(ledgerExtension, ledgerExtInd, le.ext);

            st.fetch();
        }
        return res;
    }

  public:
    BulkLoadDataOperation(Database& db, UnorderedSet<LedgerKey> const& keys)
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
        std::string sql = "WITH r AS (" + sqlJoin +
                          ") SELECT accountid, dataname, datavalue, "
                          "lastmodified, extension, "
                          "ledgerext "
                          "FROM accountdata WHERE (accountid, dataname) IN r";

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
        sqlite3_bind_pointer(st, 3, cstrDataNames.data(), "carray", 0);
        sqlite3_bind_int(st, 4, static_cast<int>(cstrDataNames.size()));
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
            "SELECT accountid, dataname, datavalue, lastmodified, extension, "
            "ledgerext "
            "FROM accountdata WHERE (accountid, dataname) IN (SELECT * FROM r)";

        auto prep = mDb.getPreparedStatement(sql);
        auto& st = prep.statement();
        st.exchange(soci::use(strAccountIDs));
        st.exchange(soci::use(strDataNames));
        return executeAndFetch(st);
    }
#endif
};

UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>>
LedgerTxnRoot::Impl::bulkLoadData(UnorderedSet<LedgerKey> const& keys) const
{
    ZoneScoped;
    ZoneValue(static_cast<int64_t>(keys.size()));
    if (!keys.empty())
    {
        BulkLoadDataOperation op(mDatabase, keys);
        return populateLoadedEntries(
            keys, mDatabase.doDatabaseTypeSpecificOperation(op));
    }
    else
    {
        return {};
    }
}
}
