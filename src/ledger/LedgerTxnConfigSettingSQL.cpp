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
throwIfNotConfigSetting(LedgerEntryType type)
{
    if (type != CONFIG_SETTING)
    {
        throw NonSociRelatedException("LedgerEntry is not a CONFIG_SETTING");
    }
}

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::loadConfigSetting(LedgerKey const& key) const
{
    int32_t configSettingID = key.configSetting().configSettingID;
    std::string configSettingEntryStr;

    std::string sql = "SELECT ledgerentry "
                      "FROM configsettings "
                      "WHERE configsettingid = :configsettingid";
    auto prep = mApp.getDatabase().getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::into(configSettingEntryStr));
    st.exchange(soci::use(configSettingID));
    st.define_and_bind();
    {
        auto timer = mApp.getDatabase().getSelectTimer("configsetting");
        st.execute(true);
    }
    if (!st.got_data())
    {
        return nullptr;
    }

    LedgerEntry le;
    fromOpaqueBase64(le, configSettingEntryStr);
    throwIfNotConfigSetting(le.data.type());

    return std::make_shared<LedgerEntry const>(std::move(le));
}

class bulkLoadConfigSettingsOperation
    : public DatabaseTypeSpecificOperation<std::vector<LedgerEntry>>
{
    Database& mDb;
    std::vector<int32_t> mConfigSettingIDs;

    std::vector<LedgerEntry>
    executeAndFetch(soci::statement& st)
    {
        std::string configSettingEntryStr;

        st.exchange(soci::into(configSettingEntryStr));
        st.define_and_bind();
        {
            auto timer = mDb.getSelectTimer("configsetting");
            st.execute(true);
        }

        std::vector<LedgerEntry> res;
        while (st.got_data())
        {
            res.emplace_back();
            auto& le = res.back();

            fromOpaqueBase64(le, configSettingEntryStr);
            throwIfNotConfigSetting(le.data.type());

            st.fetch();
        }
        return res;
    }

  public:
    bulkLoadConfigSettingsOperation(Database& db,
                                    UnorderedSet<LedgerKey> const& keys)
        : mDb(db)
    {
        mConfigSettingIDs.reserve(keys.size());
        for (auto const& k : keys)
        {
            throwIfNotConfigSetting(k.type());
            mConfigSettingIDs.emplace_back(k.configSetting().configSettingID);
        }
    }

    std::vector<LedgerEntry>
    doSqliteSpecificOperation(soci::sqlite3_session_backend* sq) override
    {
        std::string sql = "WITH r AS (SELECT value FROM carray(?, ?, 'int32')) "
                          "SELECT ledgerentry "
                          "FROM configsettings "
                          "WHERE configsettingid IN r";

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
        sqlite3_bind_pointer(st, 1, (void*)mConfigSettingIDs.data(), "carray",
                             0);
        sqlite3_bind_int(st, 2, static_cast<int>(mConfigSettingIDs.size()));
        return executeAndFetch(prep.statement());
    }

#ifdef USE_POSTGRES
    std::vector<LedgerEntry>
    doPostgresSpecificOperation(soci::postgresql_session_backend* pg) override
    {
        std::string strConfigSettingIDs;
        marshalToPGArray(pg->conn_, strConfigSettingIDs, mConfigSettingIDs);

        std::string sql = "WITH r AS (SELECT unnest(:v1::INT[])) "
                          "SELECT ledgerentry "
                          "FROM configsettings "
                          "WHERE configsettingid IN (SELECT * from r)";

        auto prep = mDb.getPreparedStatement(sql);
        auto& st = prep.statement();
        st.exchange(soci::use(strConfigSettingIDs));
        return executeAndFetch(st);
    }
#endif
};

UnorderedMap<LedgerKey, std::shared_ptr<LedgerEntry const>>
LedgerTxnRoot::Impl::bulkLoadConfigSettings(
    UnorderedSet<LedgerKey> const& keys) const
{
    if (!keys.empty())
    {
        bulkLoadConfigSettingsOperation op(mApp.getDatabase(), keys);
        return populateLoadedEntries(
            keys, mApp.getDatabase().doDatabaseTypeSpecificOperation(op));
    }
    else
    {
        return {};
    }
}

class bulkDeleteConfigSettingsOperation
    : public DatabaseTypeSpecificOperation<void>
{
    Database& mDb;
    LedgerTxnConsistency mCons;
    std::vector<int32_t> mConfigSettingIDs;

  public:
    bulkDeleteConfigSettingsOperation(Database& db, LedgerTxnConsistency cons,
                                      std::vector<EntryIterator> const& entries)
        : mDb(db), mCons(cons)
    {
        mConfigSettingIDs.reserve(entries.size());
        for (auto const& e : entries)
        {
            releaseAssert(!e.entryExists());
            throwIfNotConfigSetting(e.key().ledgerKey().type());
            mConfigSettingIDs.emplace_back(
                e.key().ledgerKey().configSetting().configSettingID);
        }
    }

    void
    doSociGenericOperation()
    {
        std::string sql =
            "DELETE FROM configsettings WHERE configsettingid = :id";
        auto prep = mDb.getPreparedStatement(sql);
        auto& st = prep.statement();
        st.exchange(soci::use(mConfigSettingIDs));
        st.define_and_bind();
        {
            auto timer = mDb.getDeleteTimer("configsetting");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) !=
                mConfigSettingIDs.size() &&
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
        std::string strConfigSettingIDs;
        marshalToPGArray(pg->conn_, strConfigSettingIDs, mConfigSettingIDs);

        std::string sql = "WITH r AS (SELECT unnest(:v1::INT[])) "
                          "DELETE FROM configsettings "
                          "WHERE configsettingid IN (SELECT * FROM r)";

        auto prep = mDb.getPreparedStatement(sql);
        auto& st = prep.statement();
        st.exchange(soci::use(strConfigSettingIDs));
        st.define_and_bind();
        {
            auto timer = mDb.getDeleteTimer("configsetting");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) !=
                mConfigSettingIDs.size() &&
            mCons == LedgerTxnConsistency::EXACT)
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }
#endif
};

void
LedgerTxnRoot::Impl::bulkDeleteConfigSettings(
    std::vector<EntryIterator> const& entries, LedgerTxnConsistency cons)
{
    bulkDeleteConfigSettingsOperation op(mApp.getDatabase(), cons, entries);
    mApp.getDatabase().doDatabaseTypeSpecificOperation(op);
}

class bulkUpsertConfigSettingsOperation
    : public DatabaseTypeSpecificOperation<void>
{
    Database& mDb;
    std::vector<int32_t> mConfigSettingIDs;
    std::vector<std::string> mConfigSettingEntries;
    std::vector<int32_t> mLastModifieds;

    void
    accumulateEntry(LedgerEntry const& entry)
    {
        throwIfNotConfigSetting(entry.data.type());

        mConfigSettingIDs.emplace_back(
            entry.data.configSetting().configSettingID);
        mConfigSettingEntries.emplace_back(toOpaqueBase64(entry));
        mLastModifieds.emplace_back(
            unsignedToSigned(entry.lastModifiedLedgerSeq));
    }

  public:
    bulkUpsertConfigSettingsOperation(
        Database& Db, std::vector<EntryIterator> const& entryIter)
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
        std::string sql = "INSERT INTO configsettings "
                          "(configsettingid, ledgerentry, lastmodified) "
                          "VALUES "
                          "( :id, :v1, :v2 ) "
                          "ON CONFLICT (configsettingid) DO UPDATE SET "
                          "ledgerentry = excluded.ledgerentry, "
                          "lastmodified = excluded.lastmodified";

        auto prep = mDb.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(mConfigSettingIDs));
        st.exchange(soci::use(mConfigSettingEntries));
        st.exchange(soci::use(mLastModifieds));
        st.define_and_bind();
        {
            auto timer = mDb.getUpsertTimer("configsetting");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) !=
            mConfigSettingIDs.size())
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
        std::string strConfigSettingIDs, strConfigSettingEntries,
            strLastModifieds;

        PGconn* conn = pg->conn_;
        marshalToPGArray(conn, strConfigSettingIDs, mConfigSettingIDs);
        marshalToPGArray(conn, strConfigSettingEntries, mConfigSettingEntries);
        marshalToPGArray(conn, strLastModifieds, mLastModifieds);

        std::string sql = "WITH r AS "
                          "(SELECT unnest(:ids::INT[]), unnest(:v1::TEXT[]), "
                          "unnest(:v2::INT[])) "
                          "INSERT INTO configsettings "
                          "(configsettingid, ledgerentry, lastmodified) "
                          "SELECT * FROM r "
                          "ON CONFLICT (configsettingid) DO UPDATE SET "
                          "ledgerentry = excluded.ledgerentry, "
                          "lastmodified = excluded.lastmodified";

        auto prep = mDb.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(strConfigSettingIDs));
        st.exchange(soci::use(strConfigSettingEntries));
        st.exchange(soci::use(strLastModifieds));
        st.define_and_bind();
        {
            auto timer = mDb.getUpsertTimer("configsetting");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) !=
            mConfigSettingIDs.size())
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }
#endif
};

void
LedgerTxnRoot::Impl::bulkUpsertConfigSettings(
    std::vector<EntryIterator> const& entries)
{
    bulkUpsertConfigSettingsOperation op(mApp.getDatabase(), entries);
    mApp.getDatabase().doDatabaseTypeSpecificOperation(op);
}

void
LedgerTxnRoot::Impl::dropConfigSettings(bool rebuild)
{
    throwIfChild();
    mEntryCache.clear();
    mBestOffers.clear();

    mApp.getDatabase().getSession() << "DROP TABLE IF EXISTS configsettings;";

    if (rebuild)
    {
        std::string coll = mApp.getDatabase().getSimpleCollationClause();
        mApp.getDatabase().getSession()
            << "CREATE TABLE configsettings ("
            << "configsettingid INT PRIMARY KEY, "
            << "ledgerentry  TEXT " << coll << " NOT NULL, "
            << "lastmodified INT NOT NULL);";
    }
}
}
#endif