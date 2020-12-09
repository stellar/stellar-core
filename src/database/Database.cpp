// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "database/Database.h"
#include "crypto/Hex.h"
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
#include "xdr/Stellar-ledger-entries.h"

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
static unsigned long const MIN_SCHEMA_VERSION = 12;
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
{
    registerDrivers();

    CLOG_INFO(
        Database, "Connecting to: {}",
        removePasswordFromConnectionString(app.getConfig().DATABASE.value));
    mSession.open(app.getConfig().DATABASE.value);
    DatabaseConfigureSessionOp op(mSession);
    doDatabaseTypeSpecificOperation(op);
}

void
Database::applySchemaUpgrade(unsigned long vers)
{
    clearPreparedStatementCache();

    soci::transaction tx(mSession);
    switch (vers)
    {
    case 13:
        if (!mApp.getConfig().MODE_USES_IN_MEMORY_LEDGER)
        {
            // Add columns for the LedgerEntry extension to each of
            // the tables that stores a type of ledger entry.
            addTextColumn("accounts", "ledgerext");
            addTextColumn("trustlines", "ledgerext");
            addTextColumn("accountdata", "ledgerext");
            addTextColumn("offers", "ledgerext");
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
            addTextColumn("offers", "extension");
            addTextColumn("accountdata", "extension");

            mApp.getLedgerTxnRoot().dropClaimableBalances();
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
    actBeforeDBSchemaUpgrade();
    while (vers < SCHEMA_VERSION)
    {
        ++vers;
        CLOG_INFO(Database, "Applying DB schema upgrade to version {}", vers);
        applySchemaUpgrade(vers);
        putSchemaVersion(vers);
    }
    CLOG_INFO(Database, "DB schema is in current version");
    assert(vers == SCHEMA_VERSION);
}

void
Database::addTextColumn(std::string const& table, std::string const& column)
{
    std::string addColumnStr("ALTER TABLE " + table + " ADD " + column +
                             " TEXT;");
    CLOG_INFO(Database, "Adding column '{}' to table '{}'", column, table);
    mSession << addColumnStr;
}

void
Database::dropNullableColumn(std::string const& table,
                             std::string const& column)
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
        CLOG_INFO(Database, "Dropping column '{}' from table '{}'", column,
                  table);

        mSession << dropColumnStr;
    }
    else
    {
        std::string nullColumnStr("UPDATE " + table + " SET " + column +
                                  " = NULL");
        CLOG_INFO(Database,
                  "Setting all cells of column '{}' in table '{}' to NULL",
                  column, table);

        mSession << nullColumnStr;
    }
}

std::string
Database::getOldLiabilitySelect(std::string const& table,
                                std::string const& fields)
{
    return fmt::format("SELECT {}, "
                       "buyingliabilities, sellingliabilities FROM {} WHERE "
                       "buyingliabilities IS NOT NULL OR "
                       "sellingliabilities IS NOT NULL",
                       fields, table);
}

void
Database::convertAccountExtensionsToOpaqueXDR()
{
    addTextColumn("accounts", "extension");
    copyIndividualAccountExtensionFieldsToOpaqueXDR();
    dropNullableColumn("accounts", "buyingliabilities");
    dropNullableColumn("accounts", "sellingliabilities");
}

void
Database::convertTrustLineExtensionsToOpaqueXDR()
{
    addTextColumn("trustlines", "extension");
    copyIndividualTrustLineExtensionFieldsToOpaqueXDR();
    dropNullableColumn("trustlines", "buyingliabilities");
    dropNullableColumn("trustlines", "sellingliabilities");
}

void
Database::copyIndividualAccountExtensionFieldsToOpaqueXDR()
{
    std::string const tableStr = "accounts";

    CLOG_INFO(Database, "Updating extension schema for {}", tableStr);

    // <accountID, extension>
    struct Fields
    {
        std::string mAccountID;
        std::string mExtension;
    };

    std::string const fieldsStr = "accountid";
    std::string const selectStr = getOldLiabilitySelect(tableStr, fieldsStr);
    auto makeFields = [](soci::row const& row) {
        AccountEntry::_ext_t extension;
        // getOldLiabilitySelect() places the buying and selling extension
        // column names after the key field in the SQL select string.
        extension.v(1);
        extension.v1().liabilities.buying = row.get<long long>(1);
        extension.v1().liabilities.selling = row.get<long long>(2);
        return Fields{row.get<std::string>(0),
                      decoder::encode_b64(xdr::xdr_to_opaque(extension))};
    };

    std::string const updateStr =
        "UPDATE accounts SET extension = :ext WHERE accountID = :id";
    auto prepUpdate = [](soci::statement& st_update, Fields const& data) {
        st_update.exchange(soci::use(data.mExtension)),
            st_update.exchange(soci::use(data.mAccountID));
    };

    auto postUpdate = [](long long const affected_rows, Fields const& data) {
        if (affected_rows != 1)
        {
            throw std::runtime_error(fmt::format(
                "{}: updating account with account ID {} affected {} row(s) ",
                __func__, data.mAccountID, affected_rows));
        }
    };

    size_t numUpdated = selectUpdateMap<Fields>(
        *this, selectStr, makeFields, updateStr, prepUpdate, postUpdate);

    CLOG_INFO(Database,
              "{}: updated {} records(s) with liabilities in {} table",
              __func__, numUpdated, tableStr);
}

void
Database::copyIndividualTrustLineExtensionFieldsToOpaqueXDR()
{
    std::string const tableStr = "trustlines";

    CLOG_INFO(Database, "{}: updating extension schema for {}", __func__,
              tableStr);

    // <accountID, issuer_id, asset_id, extension>
    struct Fields
    {
        std::string mAccountID;
        std::string mIssuerID;
        std::string mAssetID;
        std::string mExtension;
    };

    std::string const fieldsStr = "accountid, issuer, assetcode";
    std::string const selectStr = getOldLiabilitySelect(tableStr, fieldsStr);
    auto makeFields = [](soci::row const& row) {
        TrustLineEntry::_ext_t extension;
        // getOldLiabilitySelect() places the buying and selling extension
        // column names after the three key fields in the SQL select string.
        extension.v(1);
        extension.v1().liabilities.buying = row.get<long long>(3);
        extension.v1().liabilities.selling = row.get<long long>(4);
        return Fields{row.get<std::string>(0), row.get<std::string>(1),
                      row.get<std::string>(2),
                      decoder::encode_b64(xdr::xdr_to_opaque(extension))};
    };

    std::string const updateStr =
        "UPDATE trustlines SET extension = :ext WHERE accountID = :id "
        "AND issuer = :issuer_id AND assetcode = :asset_id";
    auto prepUpdate = [](soci::statement& st_update, Fields const& data) {
        st_update.exchange(soci::use(data.mExtension));
        st_update.exchange(soci::use(data.mAccountID));
        st_update.exchange(soci::use(data.mIssuerID));
        st_update.exchange(soci::use(data.mAssetID));
    };

    auto postUpdate = [](long long const affected_rows, Fields const& data) {
        if (affected_rows != 1)
        {
            throw std::runtime_error(fmt::format(
                "{}: updating trustline with account ID {}, issuer {}, and "
                "asset {} affected {} row(s)",
                __func__, data.mAccountID, data.mIssuerID, data.mAssetID,
                affected_rows));
        }
    };

    size_t numUpdated = selectUpdateMap<Fields>(
        *this, selectStr, makeFields, updateStr, prepUpdate, postUpdate);

    CLOG_INFO(Database,
              "{}: updated {} records(s) with liabilities in {} table",
              __func__, numUpdated, tableStr);
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
        mApp.getLedgerTxnRoot().dropClaimableBalances();
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
    mApp.getHerderPersistence().createQuorumTrackingTable(mSession);

    LOG_INFO(DEFAULT_LOG, "* ");
    LOG_INFO(DEFAULT_LOG, "* The database has been initialized");
    LOG_INFO(DEFAULT_LOG, "* ");
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
        LOG_INFO(DEFAULT_LOG, "Establishing {}-entry connection pool to: {}", n,
                 removePasswordFromConnectionString(c.value));
        mPool = std::make_unique<soci::connection_pool>(n);
        for (size_t i = 0; i < n; ++i)
        {
            LOG_DEBUG(DEFAULT_LOG, "Opening pool entry {}", i);
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
        CLOG_INFO(Database, "");
        CLOG_INFO(Database, "");
        CLOG_INFO(Database, "[SQL] -----------------------");
        CLOG_INFO(Database, "[SQL] begin capture: {}", mName);
        CLOG_INFO(Database, "[SQL] -----------------------");
        while (std::getline(rd, buf))
        {
            CLOG_INFO(Database, "[SQL:{}] {}", mName, buf);
            buf.clear();
        }
        CLOG_INFO(Database, "[SQL] -----------------------");
        CLOG_INFO(Database, "[SQL] end capture: {}", mName);
        CLOG_INFO(Database, "[SQL] -----------------------");
        CLOG_INFO(Database, "");
        CLOG_INFO(Database, "");
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
}
