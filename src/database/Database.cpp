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
#include "util/Fs.h"
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

// These should always match our compiled version precisely, since we are
// using a bundled version to get access to carray(). But in case someone
// overrides that or our build configuration changes, it's nicer to get a
// more-precise version-mismatch error message than a runtime crash due
// to using SQLite features that aren't supported on an old version.
static int const MIN_SQLITE_MAJOR_VERSION = 3;
static int const MIN_SQLITE_MINOR_VERSION = 45;
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
    , mSession("main")
    , mStatementsSize(
          app.getMetrics().NewCounter({"database", "memory", "statements"}))
{
    registerDrivers();

    CLOG_INFO(
        Database, "Connecting to: {}",
        removePasswordFromConnectionString(app.getConfig().DATABASE.value));
    open();
}

void
Database::open()
{
    mSession.session().open(mApp.getConfig().DATABASE.value);
    DatabaseConfigureSessionOp op(mSession.session());
    doDatabaseTypeSpecificOperation(op, mSession);
}

void
Database::applySchemaUpgrade(unsigned long vers)
{
    clearPreparedStatementCache(mSession);

    soci::transaction tx(mSession.session());
    switch (vers)
    {
    case 22:
        dropSupportTransactionFeeHistory(*this);
        break;
    case 23:
        mApp.getHistoryManager().dropSQLBasedPublish();
        Upgrades::dropSupportUpgradeHistory(*this);
        break;
    case 24:
        getRawSession() << "DROP TABLE IF EXISTS pubsub;";
        mApp.getPersistentState().migrateToSlotStateTable();
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
    while (vers < SCHEMA_VERSION)
    {
        ++vers;
        CLOG_INFO(Database, "Applying DB schema upgrade to version {}", vers);
        applySchemaUpgrade(vers);
        putSchemaVersion(vers);
    }

    maybeUpgradeToBucketListDB();

    CLOG_INFO(Database, "DB schema is in current version");
    releaseAssert(vers == SCHEMA_VERSION);
}

void
Database::maybeUpgradeToBucketListDB()
{
    if (mApp.getPersistentState().getState(PersistentState::kDBBackend,
                                           getSession()) !=
        LiveBucketIndex::DB_BACKEND_STATE)
    {
        CLOG_INFO(Database, "Upgrading to BucketListDB");

        // Drop all LedgerEntry tables except for offers
        CLOG_INFO(Database, "Dropping table accounts");
        getRawSession() << "DROP TABLE IF EXISTS accounts;";

        CLOG_INFO(Database, "Dropping table signers");
        getRawSession() << "DROP TABLE IF EXISTS signers;";

        CLOG_INFO(Database, "Dropping table claimablebalance");
        getRawSession() << "DROP TABLE IF EXISTS claimablebalance;";

        CLOG_INFO(Database, "Dropping table configsettings");
        getRawSession() << "DROP TABLE IF EXISTS configsettings;";

        CLOG_INFO(Database, "Dropping table contractcode");
        getRawSession() << "DROP TABLE IF EXISTS contractcode;";

        CLOG_INFO(Database, "Dropping table contractdata");
        getRawSession() << "DROP TABLE IF EXISTS contractdata;";

        CLOG_INFO(Database, "Dropping table accountdata");
        getRawSession() << "DROP TABLE IF EXISTS accountdata;";

        CLOG_INFO(Database, "Dropping table liquiditypool");
        getRawSession() << "DROP TABLE IF EXISTS liquiditypool;";

        CLOG_INFO(Database, "Dropping table trustlines");
        getRawSession() << "DROP TABLE IF EXISTS trustlines;";

        CLOG_INFO(Database, "Dropping table ttl");
        getRawSession() << "DROP TABLE IF EXISTS ttl;";

        mApp.getPersistentState().setState(PersistentState::kDBBackend,
                                           LiveBucketIndex::DB_BACKEND_STATE,
                                           getSession());
    }
}

void
Database::putSchemaVersion(unsigned long vers)
{
    mApp.getPersistentState().setState(PersistentState::kDatabaseSchema,
                                       std::to_string(vers),
                                       mApp.getDatabase().getSession());
}

unsigned long
Database::getDBSchemaVersion()
{
    releaseAssert(threadIsMain());
    unsigned long vers = 0;
    try
    {
        auto vstr = mApp.getPersistentState().getState(
            PersistentState::kDatabaseSchema, getSession());
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

medida::TimerContext
Database::getInsertTimer(std::string const& entityName)
{
    mQueryMeter.Mark();
    return mApp.getMetrics()
        .NewTimer({"database", "insert", entityName})
        .TimeScope();
}

medida::TimerContext
Database::getSelectTimer(std::string const& entityName)
{
    mQueryMeter.Mark();
    return mApp.getMetrics()
        .NewTimer({"database", "select", entityName})
        .TimeScope();
}

medida::TimerContext
Database::getDeleteTimer(std::string const& entityName)
{
    mQueryMeter.Mark();
    return mApp.getMetrics()
        .NewTimer({"database", "delete", entityName})
        .TimeScope();
}

medida::TimerContext
Database::getUpdateTimer(std::string const& entityName)
{
    mQueryMeter.Mark();
    return mApp.getMetrics()
        .NewTimer({"database", "update", entityName})
        .TimeScope();
}

medida::TimerContext
Database::getUpsertTimer(std::string const& entityName)
{
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
        auto prep =
            getPreparedStatement("SET TRANSACTION READ ONLY", getSession());
        auto& st = prep.statement();
        st.define_and_bind();
        st.execute(false);
    }
}

bool
Database::isSqlite() const
{
    return mApp.getConfig().DATABASE.value.find("sqlite3://") !=
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
    std::lock_guard<std::mutex> lock(mStatementsMutex);
    for (auto& c : mCaches)
    {
        for (auto& st : c.second)
        {
            st.second->clean_up(true);
        }
    }
    mCaches.clear();
    mStatementsSize.set_count(0);
}

void
Database::clearPreparedStatementCache(SessionWrapper& session)
{
    std::lock_guard<std::mutex> lock(mStatementsMutex);

    // Flush all prepared statements; in sqlite they represent open cursors
    // and will conflict with any DROP TABLE commands issued below
    for (auto st : mCaches[session.getSessionName()])
    {
        st.second->clean_up(true);
        mStatementsSize.dec();
    }
    mCaches.erase(session.getSessionName());
}

void
Database::initialize()
{
    clearPreparedStatementCache();
    if (isSqlite())
    {
        // delete the sqlite file directly if possible
        std::string fn;

        {
            int i;
            std::string databaseName, databaseLocation;
            soci::statement st =
                (getRawSession().prepare << "PRAGMA database_list;",
                 soci::into(i), soci::into(databaseName),
                 soci::into(databaseLocation));
            st.execute(true);
            while (st.got_data())
            {
                if (databaseName == "main")
                {
                    fn = databaseLocation;
                    break;
                }
            }
        }
        if (!fn.empty() && fs::exists(fn))
        {
            getRawSession().close();
            std::remove(fn.c_str());
            open();
        }
    }
    // normally you do not want to touch this section as
    // schema updates are done in applySchemaUpgrade

    // only time this section should be modified is when
    // consolidating changes found in applySchemaUpgrade here
    Upgrades::dropSupportUpgradeHistory(*this);
    OverlayManager::dropAll(*this);
    PersistentState::dropAll(*this);
    LedgerHeaderUtils::dropAll(*this);
    // No need to re-create txhistory, will be dropped during
    // upgradeToCurrentSchema anyway
    dropSupportTxHistory(*this);
    HistoryManager::dropAll(*this);
    HerderPersistence::dropAll(*this);
    BanManager::dropAll(*this);
    putSchemaVersion(MIN_SCHEMA_VERSION);

    LOG_INFO(DEFAULT_LOG, "* ");
    LOG_INFO(DEFAULT_LOG, "* The database has been initialized");
    LOG_INFO(DEFAULT_LOG, "* ");
}

SessionWrapper&
Database::getSession()
{
    // global session can only be used from the main thread
    releaseAssert(threadIsMain());
    return mSession;
}

soci::session&
Database::getRawSession()
{
    return getSession().session();
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
            stellar::doDatabaseTypeSpecificOperation(sess, op);
        }
    }
    releaseAssert(mPool);
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
Database::getPreparedStatement(std::string const& query,
                               SessionWrapper& session)
{
    std::lock_guard<std::mutex> lock(mStatementsMutex);

    auto& cache = mCaches[session.getSessionName()];
    auto i = cache.find(query);
    std::shared_ptr<soci::statement> p;
    if (i == cache.end())
    {
        p = std::make_shared<soci::statement>(session.session());
        p->alloc();
        p->prepare(query);
        cache.insert(std::make_pair(query, p));
        mStatementsSize.inc();
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
    return make_shared<SQLLogContext>(contextName, mSession.session());
}
}
