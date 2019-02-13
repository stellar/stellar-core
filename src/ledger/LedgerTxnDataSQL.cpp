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

void
LedgerTxnRoot::Impl::insertOrUpdateData(LedgerEntry const& entry, bool isInsert)
{
    auto const& data = entry.data.data();
    std::string actIDStrKey = KeyUtils::toStrKey(data.accountID);
    std::string dataName = decoder::encode_b64(data.dataName);
    std::string dataValue = decoder::encode_b64(data.dataValue);

    std::string sql;
    if (isInsert)
    {
        sql = "INSERT INTO accountdata "
              "(accountid,dataname,datavalue,lastmodified)"
              " VALUES (:aid,:dn,:dv,:lm)";
    }
    else
    {
        sql = "UPDATE accountdata SET datavalue=:dv,lastmodified=:lm "
              " WHERE accountid=:aid AND dataname=:dn";
    }

    auto prep = mDatabase.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey, "aid"));
    st.exchange(soci::use(dataName, "dn"));
    st.exchange(soci::use(dataValue, "dv"));
    st.exchange(soci::use(entry.lastModifiedLedgerSeq, "lm"));
    st.define_and_bind();
    st.execute(true);
    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("could not update SQL");
    }
}

void
LedgerTxnRoot::Impl::deleteData(LedgerKey const& key, LedgerTxnConsistency cons)
{
    auto const& data = key.data();
    std::string actIDStrKey = KeyUtils::toStrKey(data.accountID);
    std::string dataName = decoder::encode_b64(data.dataName);

    auto prep = mDatabase.getPreparedStatement(
        "DELETE FROM accountdata WHERE accountid=:id AND dataname=:s");
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey));
    st.exchange(soci::use(dataName));
    st.define_and_bind();
    {
        auto timer = mDatabase.getDeleteTimer("data");
        st.execute(true);
    }
    if (st.get_affected_rows() != 1 && cons == LedgerTxnConsistency::EXACT)
    {
        throw std::runtime_error("Could not update data in SQL");
    }
}

class BulkUpsertDataOperation : public DatabaseTypeSpecificOperation
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
        std::vector<LedgerEntry> entries;
        entries.reserve(entries.size());
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
        if (st.get_affected_rows() != mAccountIDs.size())
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
        if (st.get_affected_rows() != mAccountIDs.size())
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }
#endif
};

class BulkDeleteDataOperation : public DatabaseTypeSpecificOperation
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
        if (st.get_affected_rows() != mAccountIDs.size() &&
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
        if (st.get_affected_rows() != mAccountIDs.size() &&
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

    size_t numUpdated = 0;
    for (auto const& le : dataToEncode)
    {
        insertOrUpdateData(le, true);

        if ((++numUpdated & 0xfff) == 0xfff ||
            (numUpdated == dataToEncode.size()))
        {
            CLOG(INFO, "Ledger") << "Wrote " << numUpdated << " data entries";
        }
    }
}
}
