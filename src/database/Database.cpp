// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Hex.h"
#include "database/Database.h"
#include "database/DatabaseConnectionString.h"
#include "database/DatabaseTypeSpecificOperation.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/StellarXDR.h"
#include "util/Decoder.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/types.h"
#include <error.h>
#include <fmt/format.h>

#include "bucket/BucketManager.h"
#include "herder/HerderPersistence.h"
#include "herder/Upgrades.h"
#include "history/HistoryManager.h"
#include "ledger/LedgerHeaderUtils.h"
#include "ledger/LedgerTxn.h"
#include "main/ExternalQueue.h"
#include "main/PersistentState.h"
#include "overlay/BanManager.h"
#include "overlay/OverlayManager.h"
#include "overlay/PeerManager.h"
#include "transactions/TransactionSQL.h"

#include "medida/counter.h"
#include "medida/metrics_registry.h"
#include "medida/timer.h"

#include <lib/soci/src/backends/sqlite3/soci-sqlite3.h>
#include <string>
#ifdef USE_POSTGRES
#include <lib/soci/src/backends/postgresql/soci-postgresql.h>
#endif
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

extern "C" int
sqlite3_carray_init(sqlite_api::sqlite3* db, char** pzErrMsg,
                    const sqlite_api::sqlite3_api_routines* pApi);

// NOTE: soci will just crash and not throw
//  if you misname a column in a query. yay!

namespace stellar
{

using namespace soci;
using namespace std;

bool Database::gDriversRegistered = false;

// smallest schema version supported
static unsigned long const MIN_SCHEMA_VERSION = 9;
static unsigned long const SCHEMA_VERSION = 13;

// These should always match our compiled version precisely, since we are
// using a bundled version to get access to carray(). But in case someone
// overrides that or our build configuration changes, it's nicer to get a
// more-precise version-mismatch error message than a runtime crash due
// to using SQLite features that aren't supported on an old version.
static int const MIN_SQLITE_MAJOR_VERSION = 3;
static int const MIN_SQLITE_MINOR_VERSION = 26;
static int const MIN_SQLITE_VERSION =
    (1000000 * MIN_SQLITE_MAJOR_VERSION) + (1000 * MIN_SQLITE_MINOR_VERSION);

// PostgreSQL pre-10.0 actually used its "minor number" as a major one
// (meaning: 9.4 and 9.5 were considered different major releases, with
// compatibility differences and so forth). After 10.0 they started doing
// what everyone else does, where 10.0 and 10.1 were only "minor". Either
// way though, we have a minimum minor version.
static int const MIN_POSTGRESQL_MAJOR_VERSION = 9;
static int const MIN_POSTGRESQL_MINOR_VERSION = 5;
static int const MIN_POSTGRESQL_VERSION =
    (10000 * MIN_POSTGRESQL_MAJOR_VERSION) +
    (100 * MIN_POSTGRESQL_MINOR_VERSION);

#ifdef USE_POSTGRES
static std::string
badPgVersion(int vers)
{
    std::ostringstream msg;
    int maj = (vers / 10000);
    int min = (vers - (maj * 10000)) / 100;
    msg << "PostgreSQL version " << maj << '.' << min
        << " is too old, must use at least " << MIN_POSTGRESQL_MAJOR_VERSION
        << '.' << MIN_POSTGRESQL_MINOR_VERSION;
    return msg.str();
}
#endif

static std::string
badSqliteVersion(int vers)
{
    std::ostringstream msg;
    int maj = (vers / 1000000);
    int min = (vers - (maj * 1000000)) / 1000;
    msg << "SQLite version " << maj << '.' << min
        << " is too old, must use at least " << MIN_SQLITE_MAJOR_VERSION << '.'
        << MIN_SQLITE_MINOR_VERSION;
    return msg.str();
}

void
Database::registerDrivers()
{
    if (!gDriversRegistered)
    {
        register_factory_sqlite3();
#ifdef USE_POSTGRES
        register_factory_postgresql();
#endif
        gDriversRegistered = true;
    }
}

// Helper class that confirms that we're running on a new-enough version
// of each database type and tweaks some per-backend settings.
class DatabaseConfigureSessionOp : public DatabaseTypeSpecificOperation<void>
{
    soci::session& mSession;

  public:
    DatabaseConfigureSessionOp(soci::session& sess) : mSession(sess)
    {
    }
    void
    doSqliteSpecificOperation(soci::sqlite3_session_backend* sq) override
    {
        int vers = sqlite_api::sqlite3_libversion_number();
        if (vers < MIN_SQLITE_VERSION)
        {
            throw std::runtime_error(badSqliteVersion(vers));
        }

        mSession << "PRAGMA journal_mode = WAL";
        // FULL is needed as to ensure durability
        // NORMAL is enough for non validating nodes
        // mSession << "PRAGMA synchronous = NORMAL";

        // number of pages in WAL file
        mSession << "PRAGMA wal_autocheckpoint=10000";

        // busy_timeout gives room for external processes
        // that may lock the database for some time
        mSession << "PRAGMA busy_timeout = 10000";

        // adjust caches
        // 20000 pages
        mSession << "PRAGMA cache_size=-20000";
        // 100 MB map
        mSession << "PRAGMA mmap_size=104857600";

        // Register the sqlite carray() extension we use for bulk operations.
        sqlite3_carray_init(sq->conn_, nullptr, nullptr);
    }
#ifdef USE_POSTGRES
    void
    doPostgresSpecificOperation(soci::postgresql_session_backend* pg) override
    {
        int vers = PQserverVersion(pg->conn_);
        if (vers < MIN_POSTGRESQL_VERSION)
        {
            throw std::runtime_error(badPgVersion(vers));
        }
        mSession
            << "SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL "
               "SERIALIZABLE";
    }
#endif
};

Database::Database(Application& app)
    : mApp(app)
    , mQueryMeter(
          app.getMetrics().NewMeter({"database", "query", "exec"}, "query"))
    , mStatementsSize(
          app.getMetrics().NewCounter({"database", "memory", "statements"}))
    , mExcludedQueryTime(0)
    , mExcludedTotalTime(0)
    , mLastIdleQueryTime(0)
    , mLastIdleTotalTime(app.getClock().now())
{
    registerDrivers();

    CLOG(INFO, "Database") << "Connecting to: "
                           << removePasswordFromConnectionString(
                                  app.getConfig().DATABASE.value);
    mSession.open(app.getConfig().DATABASE.value);
    DatabaseConfigureSessionOp op(mSession);
    doDatabaseTypeSpecificOperation(op);
}

std::string const Database::ledgerExtName = "ledgerext";

void
Database::applySchemaUpgrade(unsigned long vers)
{
    clearPreparedStatementCache();

    soci::transaction tx(mSession);
    switch (vers)
    {
    case 10:
        // add tracking table information
        mApp.getHerderPersistence().createQuorumTrackingTable(mSession);
        break;
    case 11:
        if (!mApp.getConfig().MODE_USES_IN_MEMORY_LEDGER)
        {
            mSession << "DROP INDEX IF EXISTS bestofferindex;";
            mSession << "CREATE INDEX bestofferindex ON offers "
                        "(sellingasset,buyingasset,price,offerid);";
        }
        break;
    case 12:
        if (!isSqlite())
        {
            // Set column collations to "C" if postgres; sqlite doesn't support
            // altering them at all (and the defaults are correct anyways).
            mSession << "ALTER TABLE accounts "
                     << "ALTER COLUMN accountid "
                     << "TYPE VARCHAR(56) COLLATE \"C\"";

            mSession << "ALTER TABLE accountdata "
                     << "ALTER COLUMN accountid "
                     << "TYPE VARCHAR(56) COLLATE \"C\", "
                     << "ALTER COLUMN dataname "
                     << "TYPE VARCHAR(88) COLLATE \"C\"";

            mSession << "ALTER TABLE offers "
                     << "ALTER COLUMN sellerid "
                     << "TYPE VARCHAR(56) COLLATE \"C\", "
                     << "ALTER COLUMN buyingasset "
                     << "TYPE TEXT COLLATE \"C\", "
                     << "ALTER COLUMN sellingasset "
                     << "TYPE TEXT COLLATE \"C\"";

            mSession << "ALTER TABLE trustlines "
                     << "ALTER COLUMN accountid "
                     << "TYPE VARCHAR(56) COLLATE \"C\", "
                     << "ALTER COLUMN issuer "
                     << "TYPE VARCHAR(56) COLLATE \"C\", "
                     << "ALTER COLUMN assetcode "
                     << "TYPE VARCHAR(12) COLLATE \"C\"";
        }

        // With inflation disabled, it's not worth keeping
        // the accountbalances index around.
        mSession << "DROP INDEX IF EXISTS accountbalances";
        break;
    case 13:
        if (!mApp.getConfig().MODE_USES_IN_MEMORY_LEDGER)
        {
            // Add columns for the LedgerEntry extension to each of
            // the tables that stores a type of ledger entry.
            addTextColumnWithDefault(
                "accounts", ledgerExtName,
                decoder::encode_b64(xdr::xdr_to_opaque(LedgerEntry::_ext_t())));
            addTextColumnWithDefault(
                "trustlines", ledgerExtName,
                decoder::encode_b64(xdr::xdr_to_opaque(LedgerEntry::_ext_t())));
            addTextColumnWithDefault(
                "accountdata", ledgerExtName,
                decoder::encode_b64(xdr::xdr_to_opaque(LedgerEntry::_ext_t())));
            addTextColumnWithDefault(
                "offers", ledgerExtName,
                decoder::encode_b64(xdr::xdr_to_opaque(LedgerEntry::_ext_t())));
            // Absorb the explicit columns of the extension fields of
            // AccountEntry and TrustLineEntry into single opaque
            // blobs of XDR each of which represents an entire extension.
            convertAccountExtensionsToOpaqueXDR();
            convertTrustLineExtensionsToOpaqueXDR();
            // Neither earlier schema versions nor the one that we're upgrading
            // to now had any extension columns in the offers or accountdata
            // tables, but we add columns in this version, even though we're not
            // going to use them for anything other than writing out opaque
            // base64-encoded empty v0 XDR extensions, so that, as with the
            // other LedgerEntry extensions, we'll be able to add such
            // extensions in the future without bumping the database schema
            // version, writing any upgrade code, or changing the SQL that reads
            // and writes those tables.
            addTextColumnWithDefault(
                "offers", "extension",
                decoder::encode_b64(xdr::xdr_to_opaque(OfferEntry::_ext_t())));
            addTextColumnWithDefault(
                "accountdata", "extension",
                decoder::encode_b64(xdr::xdr_to_opaque(DataEntry::_ext_t())));
        }
        break;
    default:
        throw std::runtime_error("Unknown DB schema version");
    }
    tx.commit();
}

void
Database::upgradeToCurrentSchema()
{
    auto vers = getDBSchemaVersion();
    if (vers < MIN_SCHEMA_VERSION)
    {
        std::string s = ("DB schema version " + std::to_string(vers) +
                         " is older than minimum supported schema " +
                         std::to_string(MIN_SCHEMA_VERSION));
        throw std::runtime_error(s);
    }

    if (vers > SCHEMA_VERSION)
    {
        std::string s = ("DB schema version " + std::to_string(vers) +
                         " is newer than application schema " +
                         std::to_string(SCHEMA_VERSION));
        throw std::runtime_error(s);
    }
    mApp.actBeforeDBSchemaUpgrade();
    while (vers < SCHEMA_VERSION)
    {
        ++vers;
        CLOG(INFO, "Database")
            << "Applying DB schema upgrade to version " << vers;
        applySchemaUpgrade(vers);
        putSchemaVersion(vers);
    }
    CLOG(INFO, "Database") << "DB schema is in current version";
    assert(vers == SCHEMA_VERSION);
}

void
Database::addTextColumn(std::string const& table, std::string const& column)
{
    std::string addColumnStr("ALTER TABLE " + table + " ADD " + column +
                             " TEXT;");
    CLOG(INFO, "Database") << "Adding column with string '" << addColumnStr
                           << "'";
    mSession << addColumnStr;
}

void
Database::addTextColumnWithDefault(std::string const& table,
                                   std::string const& column,
                                   std::string const& defaultVal)
{
    addTextColumn(table, column);
    std::string defaultColumnStr("UPDATE " + table + " SET " + column +
                                 " = :v1");
    auto st = getPreparedStatement(defaultColumnStr).statement();
    st.exchange(soci::use(defaultVal));
    CLOG(INFO, "Database") << "Setting all cells of new column '" << column
                           << "' in table '" << table << "' to '" << defaultVal
                           << "' with string '" << defaultColumnStr << "'";
    st.define_and_bind();
    st.execute(true);
}

void
Database::dropTextColumn(std::string const& table, std::string const& column)
{
    // SQLite doesn't give us a way of dropping a column with a single
    // SQL command.  If we need it in production, we could re-create the
    // table without the column and drop the old one.  Since we currently
    // use SQLite only for testing and PostgreSQL in production, we simply
    // leave the unused columm around in SQLite at the moment, and NULL
    // out all of the cells in that column.
    if (!isSqlite())
    {
        std::string dropColumnStr("ALTER TABLE " + table + " DROP COLUMN " +
                                  column);
        CLOG(INFO, "Database")
            << "Dropping column '" << column << "' with string '"
            << dropColumnStr << "' from table '" << table << "'";

        mSession << dropColumnStr;
    }
    else
    {
        std::string nullColumnStr("UPDATE " + table + " SET " + column +
                                  " = NULL");
        CLOG(INFO, "Database") << "SQLite does not support dropping column '"
                               << column << "' from table '" << table
                               << "'; setting all cells to NULL with string '"
                               << nullColumnStr << "'";

        mSession << nullColumnStr;
    }
}

std::string
Database::getOldLiabilitySelect(std::string const& table,
                                std::string const& fields)
{
    return "SELECT " + fields + "," +
           "buyingliabilities, sellingliabilities "
           "FROM " +
           table +
           " WHERE "
           "buyingliabilities IS NOT NULL"
           " OR "
           "sellingliabilities IS NOT NULL";
}

void
Database::convertAccountExtensionsToOpaqueXDR()
{
    try
    {
        addTextColumn("accounts", "extension");
        copyIndividualAccountExtensionFieldsToOpaqueXDR();
        dropTextColumn("accounts", "buyingliabilities");
        dropTextColumn("accounts", "sellingliabilities");
    }
    catch (std::exception& e)
    {
        CLOG(FATAL, "Database") << __func__ << ": exception " << e.what()
                                << " while upgrading account extensions";
        throw;
    }
}

void
Database::convertTrustLineExtensionsToOpaqueXDR()
{
    try
    {
        addTextColumn("trustlines", "extension");
        copyIndividualTrustLineExtensionFieldsToOpaqueXDR();
        dropTextColumn("trustlines", "buyingliabilities");
        dropTextColumn("trustlines", "sellingliabilities");
    }
    catch (std::exception& e)
    {
        CLOG(FATAL, "Database") << __func__ << ": exception " << e.what()
                                << " while upgrading trustline extensions";
        throw;
    }
}

template <typename T>
void
Database::selectMap(std::string const& selectStr,
                    std::function<T(soci::row const&)> makeT,
                    std::vector<T>& out)
{
    soci::rowset<soci::row> rs = (mSession.prepare << selectStr);

    std::transform(rs.begin(), rs.end(), std::back_inserter(out), makeT);
}

template <typename T>
void
Database::updateMap(std::vector<T> const& in, std::string const& updateStr,
                    std::function<void(soci::statement&, T const&)> prepUpdate,
                    std::function<void(long long const, T const&)> postUpdate)
{
    auto st_update = getPreparedStatement(updateStr).statement();

    for (auto recT : in)
    {
        prepUpdate(st_update, recT);
        st_update.define_and_bind();
        st_update.execute(true);
        auto affected_rows = st_update.get_affected_rows();
        st_update.clean_up(false);
        postUpdate(affected_rows, recT);
    }
}

template <typename T>
size_t
Database::selectUpdateMap(
    std::string const& selectStr, std::function<T(soci::row const&)> makeT,
    std::string const& updateStr,
    std::function<void(soci::statement&, T const&)> prepUpdate,
    std::function<void(long long const, T const&)> postUpdate)
{
    std::vector<T> vecT;

    selectMap<T>(selectStr, makeT, vecT);
    updateMap<T>(vecT, updateStr, prepUpdate, postUpdate);

    return vecT.size();
}

void
Database::copyIndividualAccountExtensionFieldsToOpaqueXDR()
{
    std::string const tableStr = "accounts";

    CLOG(INFO, "Database") << __func__ << ": updating extension schema for "
                           << tableStr;

    // <accountID, extension>
    using T = std::tuple<std::string, std::string>;

    std::string const fieldsStr = "accountid";
    std::string const selectStr = getOldLiabilitySelect(tableStr, fieldsStr);
    auto makeT = [](soci::row const& row) {
        AccountEntry::_ext_t::_v1_t extension;
        // getOldLiabilitySelect() places the buying and selling extension
        // column names after the key field in the SQL select string.
        extension.liabilities.buying = row.get<long long>(1);
        extension.liabilities.selling = row.get<long long>(2);
        return std::make_tuple(
            row.get<std::string>(0), // account ID
            decoder::encode_b64(xdr::xdr_to_opaque(extension)));
    };

    std::string const updateStr =
        "UPDATE accounts SET extension = :ext WHERE accountID = :id";
    auto prepUpdate = [](soci::statement& st_update, T const& data) {
        st_update.exchange(soci::use(std::get<1>(data))); // extension
        st_update.exchange(soci::use(std::get<0>(data))); // account ID
    };

    auto postUpdate = [](long long const affected_rows, T const& data) {
        if (affected_rows != 1)
        {
            throw std::runtime_error(fmt::format(
                "{}: updating account with account ID {} affected {} row(s) ",
                __func__, std::get<0>(data), affected_rows));
        }
    };

    size_t numUpdated =
        selectUpdateMap<T>(selectStr, makeT, updateStr, prepUpdate, postUpdate);

    CLOG(INFO, "Database") << __func__ << ": updated " << numUpdated
                           << " records(s) with liabilities in " << tableStr
                           << " table";
}

void
Database::copyIndividualTrustLineExtensionFieldsToOpaqueXDR()
{
    std::string const tableStr = "trustlines";

    CLOG(INFO, "Database") << __func__ << ": updating extension schema for "
                           << tableStr;

    // <accountID, issuer_id, asset_id, extension>
    using T = std::tuple<std::string, std::string, std::string, std::string>;

    std::string const fieldsStr = "accountid, issuer, assetcode";
    std::string const selectStr = getOldLiabilitySelect(tableStr, fieldsStr);
    auto makeT = [](soci::row const& row) {
        TrustLineEntry::_ext_t::_v1_t extension;
        // getOldLiabilitySelect() places the buying and selling extension
        // column names after the three key fields in the SQL select string.
        extension.liabilities.buying = row.get<long long>(3);
        extension.liabilities.selling = row.get<long long>(4);
        return std::make_tuple(
            row.get<std::string>(0), // account ID
            row.get<std::string>(1), // issuer ID
            row.get<std::string>(2), // asset ID
            decoder::encode_b64(xdr::xdr_to_opaque(extension)));
    };

    std::string const updateStr =
        "UPDATE trustlines SET extension = :ext WHERE accountID = :id "
        "AND issuer = :issuer_id AND assetcode = :asset_id";
    auto prepUpdate = [](soci::statement& st_update, T const& data) {
        st_update.exchange(soci::use(std::get<3>(data))); // extension
        st_update.exchange(soci::use(std::get<0>(data))); // account ID
        st_update.exchange(soci::use(std::get<1>(data))); // issuer ID
        st_update.exchange(soci::use(std::get<2>(data))); // asset ID
    };

    auto postUpdate = [](long long const affected_rows, T const& data) {
        if (affected_rows != 1)
        {
            throw std::runtime_error(fmt::format(
                "{}: updating trustline with account ID {}, issuer {}, and "
                "asset {} affected {} row(s)",
                __func__, std::get<0>(data), std::get<1>(data),
                std::get<2>(data), affected_rows));
        }
    };

    size_t numUpdated =
        selectUpdateMap<T>(selectStr, makeT, updateStr, prepUpdate, postUpdate);

    CLOG(INFO, "Database") << __func__ << ": updated " << numUpdated
                           << " records(s) with liabilities in " << tableStr
                           << " table";
}

void
Database::putSchemaVersion(unsigned long vers)
{
    mApp.getPersistentState().setState(PersistentState::kDatabaseSchema,
                                       std::to_string(vers));
}

unsigned long
Database::getDBSchemaVersion()
{
    unsigned long vers = 0;
    try
    {
        auto vstr = mApp.getPersistentState().getState(
            PersistentState::kDatabaseSchema);
        vers = std::stoul(vstr);
    }
    catch (...)
    {
    }
    if (vers == 0)
    {
        throw std::runtime_error(
            "No DB schema version found, try stellar-core new-db");
    }
    return vers;
}

unsigned long
Database::getAppSchemaVersion()
{
    return SCHEMA_VERSION;
}

medida::TimerContext
Database::getInsertTimer(std::string const& entityName)
{
    mEntityTypes.insert(entityName);
    mQueryMeter.Mark();
    return mApp.getMetrics()
        .NewTimer({"database", "insert", entityName})
        .TimeScope();
}

medida::TimerContext
Database::getSelectTimer(std::string const& entityName)
{
    mEntityTypes.insert(entityName);
    mQueryMeter.Mark();
    return mApp.getMetrics()
        .NewTimer({"database", "select", entityName})
        .TimeScope();
}

medida::TimerContext
Database::getDeleteTimer(std::string const& entityName)
{
    mEntityTypes.insert(entityName);
    mQueryMeter.Mark();
    return mApp.getMetrics()
        .NewTimer({"database", "delete", entityName})
        .TimeScope();
}

medida::TimerContext
Database::getUpdateTimer(std::string const& entityName)
{
    mEntityTypes.insert(entityName);
    mQueryMeter.Mark();
    return mApp.getMetrics()
        .NewTimer({"database", "update", entityName})
        .TimeScope();
}

medida::TimerContext
Database::getUpsertTimer(std::string const& entityName)
{
    mEntityTypes.insert(entityName);
    mQueryMeter.Mark();
    return mApp.getMetrics()
        .NewTimer({"database", "upsert", entityName})
        .TimeScope();
}

void
Database::setCurrentTransactionReadOnly()
{
    if (!isSqlite())
    {
        auto prep = getPreparedStatement("SET TRANSACTION READ ONLY");
        auto& st = prep.statement();
        st.define_and_bind();
        st.execute(false);
    }
}

bool
Database::isSqlite() const
{
    return mApp.getConfig().DATABASE.value.find("sqlite3:") !=
           std::string::npos;
}

std::string
Database::getSimpleCollationClause() const
{
    if (isSqlite())
    {
        return "";
    }
    else
    {
        return " COLLATE \"C\" ";
    }
}

bool
Database::canUsePool() const
{
    return !(mApp.getConfig().DATABASE.value == ("sqlite3://:memory:"));
}

void
Database::clearPreparedStatementCache()
{
    // Flush all prepared statements; in sqlite they represent open cursors
    // and will conflict with any DROP TABLE commands issued below
    for (auto st : mStatements)
    {
        st.second->clean_up(true);
    }
    mStatements.clear();
    mStatementsSize.set_count(mStatements.size());
}

void
Database::initialize()
{
    clearPreparedStatementCache();
    // normally you do not want to touch this section as
    // schema updates are done in applySchemaUpgrade

    // only time this section should be modified is when
    // consolidating changes found in applySchemaUpgrade here
    Upgrades::dropAll(*this);
    if (!mApp.getConfig().MODE_USES_IN_MEMORY_LEDGER)
    {
        mApp.getLedgerTxnRoot().dropAccounts();
        mApp.getLedgerTxnRoot().dropOffers();
        mApp.getLedgerTxnRoot().dropTrustLines();
        mApp.getLedgerTxnRoot().dropData();
    }
    OverlayManager::dropAll(*this);
    PersistentState::dropAll(*this);
    ExternalQueue::dropAll(*this);
    LedgerHeaderUtils::dropAll(*this);
    dropTransactionHistory(*this);
    HistoryManager::dropAll(*this);
    HerderPersistence::dropAll(*this);
    BanManager::dropAll(*this);
    putSchemaVersion(MIN_SCHEMA_VERSION);

    LOG(INFO) << "* ";
    LOG(INFO) << "* The database has been initialized";
    LOG(INFO) << "* ";
}

soci::session&
Database::getSession()
{
    // global session can only be used from the main thread
    assertThreadIsMain();
    return mSession;
}

soci::connection_pool&
Database::getPool()
{
    if (!mPool)
    {
        auto const& c = mApp.getConfig().DATABASE;
        if (!canUsePool())
        {
            std::string s("Can't create connection pool to ");
            s += removePasswordFromConnectionString(c.value);
            throw std::runtime_error(s);
        }
        size_t n = std::thread::hardware_concurrency();
        LOG(INFO) << "Establishing " << n << "-entry connection pool to: "
                  << removePasswordFromConnectionString(c.value);
        mPool = std::make_unique<soci::connection_pool>(n);
        for (size_t i = 0; i < n; ++i)
        {
            LOG(DEBUG) << "Opening pool entry " << i;
            soci::session& sess = mPool->at(i);
            sess.open(c.value);
            DatabaseConfigureSessionOp op(sess);
            doDatabaseTypeSpecificOperation(op);
        }
    }
    assert(mPool);
    return *mPool;
}

class SQLLogContext : NonCopyable
{
    std::string mName;
    soci::session& mSess;
    std::ostringstream mCapture;

  public:
    SQLLogContext(std::string const& name, soci::session& sess)
        : mName(name), mSess(sess)
    {
        mSess.set_log_stream(&mCapture);
    }
    ~SQLLogContext()
    {
        mSess.set_log_stream(nullptr);
        std::string captured = mCapture.str();
        std::istringstream rd(captured);
        std::string buf;
        CLOG(INFO, "Database") << "";
        CLOG(INFO, "Database") << "";
        CLOG(INFO, "Database") << "[SQL] -----------------------";
        CLOG(INFO, "Database") << "[SQL] begin capture: " << mName;
        CLOG(INFO, "Database") << "[SQL] -----------------------";
        while (std::getline(rd, buf))
        {
            CLOG(INFO, "Database") << "[SQL:" << mName << "] " << buf;
            buf.clear();
        }
        CLOG(INFO, "Database") << "[SQL] -----------------------";
        CLOG(INFO, "Database") << "[SQL] end capture: " << mName;
        CLOG(INFO, "Database") << "[SQL] -----------------------";
        CLOG(INFO, "Database") << "";
        CLOG(INFO, "Database") << "";
    }
};

StatementContext
Database::getPreparedStatement(std::string const& query)
{
    auto i = mStatements.find(query);
    std::shared_ptr<soci::statement> p;
    if (i == mStatements.end())
    {
        p = std::make_shared<soci::statement>(mSession);
        p->alloc();
        p->prepare(query);
        mStatements.insert(std::make_pair(query, p));
        mStatementsSize.set_count(mStatements.size());
    }
    else
    {
        p = i->second;
    }
    StatementContext sc(p);
    return sc;
}

std::shared_ptr<SQLLogContext>
Database::captureAndLogSQL(std::string contextName)
{
    return make_shared<SQLLogContext>(contextName, mSession);
}

medida::Meter&
Database::getQueryMeter()
{
    return mQueryMeter;
}

std::chrono::nanoseconds
Database::totalQueryTime() const
{
    std::vector<std::string> qtypes = {"insert", "delete", "select", "update"};
    std::chrono::nanoseconds nsq(0);
    for (auto const& q : qtypes)
    {
        for (auto const& e : mEntityTypes)
        {
            auto& timer = mApp.getMetrics().NewTimer({"database", q, e});
            uint64_t sumns = static_cast<uint64_t>(
                timer.sum() *
                static_cast<double>(timer.duration_unit().count()));
            nsq += std::chrono::nanoseconds(sumns);
        }
    }
    return nsq;
}

void
Database::excludeTime(std::chrono::nanoseconds const& queryTime,
                      std::chrono::nanoseconds const& totalTime)
{
    mExcludedQueryTime += queryTime;
    mExcludedTotalTime += totalTime;
}

uint32_t
Database::recentIdleDbPercent()
{
    std::chrono::nanoseconds query = totalQueryTime();
    query -= mLastIdleQueryTime;
    query -= mExcludedQueryTime;

    std::chrono::nanoseconds total = mApp.getClock().now() - mLastIdleTotalTime;
    total -= mExcludedTotalTime;

    if (total == std::chrono::nanoseconds::zero())
    {
        return 100;
    }

    uint32_t queryPercent =
        static_cast<uint32_t>((100 * query.count()) / total.count());
    uint32_t idlePercent = 100 - queryPercent;
    if (idlePercent > 100)
    {
        // This should never happen, but clocks are not perfectly well behaved.
        CLOG(WARNING, "Database") << "DB idle percent (" << idlePercent
                                  << ") over 100, limiting to 100";
        idlePercent = 100;
    }

    CLOG(DEBUG, "Database") << "Estimated DB idle: " << idlePercent << "%"
                            << " (query=" << query.count() << "ns"
                            << ", total=" << total.count() << "ns)";

    mLastIdleQueryTime = totalQueryTime();
    mLastIdleTotalTime = mApp.getClock().now();
    mExcludedQueryTime = std::chrono::nanoseconds(0);
    mExcludedTotalTime = std::chrono::nanoseconds(0);
    return idlePercent;
}

DBTimeExcluder::DBTimeExcluder(Application& app)
    : mApp(app)
    , mStartQueryTime(app.getDatabase().totalQueryTime())
    , mStartTotalTime(app.getClock().now())
{
}

DBTimeExcluder::~DBTimeExcluder()
{
    auto deltaQ = mApp.getDatabase().totalQueryTime() - mStartQueryTime;
    auto deltaT = mApp.getClock().now() - mStartTotalTime;
    mApp.getDatabase().excludeTime(deltaQ, deltaT);
}
}
