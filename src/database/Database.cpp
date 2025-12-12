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
#include "util/MetricsRegistry.h"
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
                    sqlite_api::sqlite3_api_routines const* pApi);

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

// Tables that are moved from main DB to misc DB during schema migration.
static std::vector<std::string> const kMiscTables = {
    "peers", "ban", "quoruminfo", "scpquorums", "scphistory", "slotstate"};

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

std::string
Database::getMiscDBName(std::string const& mainDB)
{
    // Find the last dot to locate file extension
    size_t lastDot = mainDB.find_last_of('.');
    if (lastDot == std::string::npos)
    {
        // No extension found, append to end
        return mainDB + "-misc.db";
    }

    // Insert "-misc" before the extension
    std::string baseName = mainDB.substr(0, lastDot);
    std::string extension = mainDB.substr(lastDot);
    return baseName + "-misc" + extension;
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
    , mMiscSession("misc")
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
    doDatabaseTypeSpecificOperation(mSession, op);

    if (canUseMiscDB())
    {
        std::string miscDB =
            Database::getMiscDBName(mApp.getConfig().DATABASE.value);
        mMiscSession.session().open(miscDB);
        DatabaseConfigureSessionOp miscOp(mMiscSession.session());
        doDatabaseTypeSpecificOperation(mMiscSession, miscOp);
    }
}

std::string
Database::getSQLiteDBLocation(soci::session& session)
{
    releaseAssert(isSqlite());
    std::string loc;
    int i;
    std::string databaseName, databaseLocation;
    soci::statement st =
        (session.prepare << "PRAGMA database_list;", soci::into(i),
         soci::into(databaseName), soci::into(databaseLocation));
    st.execute(true);
    while (st.got_data())
    {
        if (databaseName == "main")
        {
            loc = databaseLocation;
            break;
        }
    }
    return loc;
}

void
Database::populateMiscDatabase()
{
    // Step 1: Attach the source database
    auto loc = getSQLiteDBLocation(getRawSession());
    releaseAssert(!loc.empty());
    getRawMiscSession() << "ATTACH DATABASE '" + loc + "' AS source_db";

    // Step 2: Copy data from each table
    for (auto const& tableName : kMiscTables)
    {
        int sourceCount = 0;
        getRawMiscSession() << "SELECT COUNT(*) FROM source_db." + tableName,
            soci::into(sourceCount);

        std::string insertQuery = "INSERT INTO " + tableName +
                                  " SELECT * FROM source_db." + tableName;

        getRawMiscSession() << insertQuery;

        // Verify copy was successful
        int destCount = 0;
        getRawMiscSession() << "SELECT COUNT(*) FROM " + tableName,
            soci::into(destCount);

        if (destCount != sourceCount)
        {
            throw std::runtime_error(
                fmt::format("Row count mismatch for {}: source={}, dest={}",
                            tableName, sourceCount, destCount));
        }

        CLOG_INFO(Database, "Successfully copied {} rows from table {}",
                  destCount, tableName);
    }
}

void
Database::applyMiscSchemaUpgrade(unsigned long vers)
{
    clearPreparedStatementCache(mMiscSession);
    soci::transaction tx(mMiscSession.session());
    switch (vers)
    {
    case 1:
        // Create tables for the first time.
        OverlayManager::dropAll(mMiscSession);
        PersistentState::createMisc(*this);
        HerderPersistence::dropAll(mMiscSession.session());
        BanManager::dropAll(mMiscSession);
        // Copy contents from the main DB.
        populateMiscDatabase();
        break;
    default:
        throw std::runtime_error("Unknown DB schema version");
    }
    tx.commit();

    // Detach the source database _after_ commit to avoid "database is locked
    // errors". If schema version is already the most recent, DETACH is a no-op.
    getRawMiscSession() << "DETACH DATABASE source_db";
}

void
dropMiscTablesFromMain(Application& app)
{
    releaseAssert(app.getDatabase().canUseMiscDB());
    auto& db = app.getDatabase();
    for (auto const& tableName : kMiscTables)
    {
        db.getRawSession() << "DROP TABLE IF EXISTS " + tableName + ";";
    }
}

void
Database::applySchemaUpgrade(unsigned long vers)
{
    clearPreparedStatementCache(mSession);

    soci::transaction tx(mSession.session());
    switch (vers)
    {
    case 25:
        // Remove deprecated dbbackend entry from storestate table
        getRawSession() << "DELETE FROM storestate WHERE statename = "
                           "'dbbackend';";
        break;
    case 26:
        // Remove deprecated publishqueue table
        getRawSession() << "DROP TABLE IF EXISTS publishqueue;";
        if (canUseMiscDB())
        {
            // If misc database is used, drop the migrated tables from main DB
            dropMiscTablesFromMain(mApp);
        }
        break;
    default:
        throw std::runtime_error("Unknown DB schema version");
    }
    tx.commit();
}

void
validateVersion(unsigned long vers, unsigned long minVers,
                unsigned long maxVers)
{
    if (vers < minVers)
    {
        std::string s = ("DB schema version " + std::to_string(vers) +
                         " is older than minimum supported schema " +
                         std::to_string(minVers));
        throw std::runtime_error(s);
    }

    if (vers > maxVers)
    {
        std::string s =
            ("DB schema version " + std::to_string(vers) +
             " is newer than application schema " + std::to_string(maxVers));
        throw std::runtime_error(s);
    }
}

void
Database::upgradeToCurrentSchema()
{
    auto doMigration = [&](unsigned long vers, unsigned long minVers,
                           unsigned long maxVers, bool isMain) {
        validateVersion(vers, minVers, maxVers);
        while (vers < maxVers)
        {
            ++vers;
            CLOG_INFO(Database, "{}: Applying DB schema upgrade to version {}",
                      isMain ? "Main" : "Misc", vers);
            if (isMain)
            {
                applySchemaUpgrade(vers);
                putMainSchemaVersion(vers);
            }
            else if (canUseMiscDB())
            {
                applyMiscSchemaUpgrade(vers);
                putMiscSchemaVersion(vers);
            }
        }
        releaseAssert(vers == maxVers);
    };

    // First perform migration of the MISC DB
    uint32_t miscVers = 0;
    uint32_t mainVers = getMainDBSchemaVersion();
    if (canUseMiscDB())
    {
        if (mainVers >= FIRST_MAIN_VERSION_WITH_MISC)
        {
            // DB already upgraded to v26+, misc should exist
            miscVers = getMiscDBSchemaVersion();
        }
        // Always run misc migration (creates/populates if miscVers=0)
        doMigration(miscVers, MIN_MISC_SCHEMA_VERSION, MISC_SCHEMA_VERSION,
                    false);
    }
    doMigration(mainVers, MIN_SCHEMA_VERSION, SCHEMA_VERSION, true);
    CLOG_INFO(Database, "DB schema is in current version");
}

void
Database::putMainSchemaVersion(unsigned long vers)
{
    mApp.getPersistentState().setMainState(PersistentState::kDatabaseSchema,
                                           std::to_string(vers),
                                           mApp.getDatabase().getSession());
}

void
Database::putMiscSchemaVersion(unsigned long vers)
{
    releaseAssert(canUseMiscDB());
    mApp.getPersistentState().setMiscState(PersistentState::kMiscDatabaseSchema,
                                           std::to_string(vers));
}

static unsigned long
getVersion(Application& app, PersistentState::Entry const& key,
           SessionWrapper& session)
{
    std::optional<unsigned long> vers;
    try
    {
        auto vstr = app.getPersistentState().getState(key, session);
        vers = std::stoul(vstr);
    }
    catch (...)
    {
    }
    if (!vers)
    {
        throw std::runtime_error(
            "No DB schema version found, try stellar-core new-db");
    }
    return *vers;
}

unsigned long
Database::getMainDBSchemaVersion()
{
    return getVersion(mApp, PersistentState::kDatabaseSchema, getSession());
}

unsigned long
Database::getMiscDBSchemaVersion()
{
    releaseAssert(canUseMiscDB());
    return getVersion(mApp, PersistentState::kMiscDatabaseSchema,
                      getMiscSession());
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
    return mApp.getConfig().DATABASE.value != "sqlite3://:memory:";
}

bool
Database::canUseMiscDB() const
{
    return canUsePool() && isSqlite();
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
    clearPreparedStatementCache(mSession);
    if (isSqlite())
    {
        auto cleanup = [&](soci::session& sess) {
            std::string fn = getSQLiteDBLocation(sess);
            if (!fn.empty() && fs::exists(fn))
            {
                sess.close();
                std::remove(fn.c_str());
                return true;
            }
            return false;
        };
        bool shouldOpen = cleanup(mSession.session());
        if (canUseMiscDB())
        {
            releaseAssert(cleanup(mMiscSession.session()) == shouldOpen);
        }
        if (shouldOpen)
        {
            open();
        }
    }
    // normally you do not want to touch this section as
    // schema updates are done in applySchemaUpgrade

    // only time this section should be modified is when
    // consolidating changes found in applySchemaUpgrade here

    // Note: once the network is on schema version 26+, session parameter in
    // dropAll methods can be removed.
    OverlayManager::dropAll(mSession);
    PersistentState::dropAll(*this);
    LedgerHeaderUtils::dropAll(*this);
    HerderPersistence::dropAll(mSession.session());
    BanManager::dropAll(mSession);
    putMainSchemaVersion(MIN_SCHEMA_VERSION);

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

SessionWrapper&
Database::getMiscSession()
{
    // global session can only be used from the main thread
    releaseAssert(threadIsMain());
    // Use the main session if misc DB is not supported (e.g. Postgres)
    if (!canUseMiscDB())
    {
        return mSession;
    }
    return mMiscSession;
}

soci::session&
Database::getRawSession()
{
    return getSession().session();
}

soci::session&
Database::getRawMiscSession()
{
    return getMiscSession().session();
}

static soci::connection_pool&
createPool(Database const& db, Config const& cfg,
           std::unique_ptr<soci::connection_pool>& pool, std::string dbName)
{
    if (!pool)
    {
        auto const& c = cfg.DATABASE;
        if (!db.canUsePool())
        {
            std::string s("Can't create connection pool to ");
            s += removePasswordFromConnectionString(c.value);
            throw std::runtime_error(s);
        }
        size_t n = std::thread::hardware_concurrency();
        if (db.canUseMiscDB())
        {
            n = std::max<size_t>(n / 2, 1);
        }
        LOG_INFO(DEFAULT_LOG, "Establishing {}-entry connection pool to: {}", n,
                 removePasswordFromConnectionString(c.value));
        pool = std::make_unique<soci::connection_pool>(n);
        for (size_t i = 0; i < n; ++i)
        {
            LOG_DEBUG(DEFAULT_LOG, "Opening pool entry {}", i);
            soci::session& sess = pool->at(i);
            sess.open(dbName);
            DatabaseConfigureSessionOp op(sess);
            stellar::doDatabaseTypeSpecificOperation(sess, op);
        }
    }
    releaseAssert(pool);
    return *pool;
}

soci::connection_pool&
Database::getPool()
{
    return createPool(*this, mApp.getConfig(), mPool,
                      mApp.getConfig().DATABASE.value);
}

soci::connection_pool&
Database::getMiscPool()
{
    if (!canUseMiscDB())
    {
        throw std::runtime_error("Can't use misc pool");
    }
    return createPool(*this, mApp.getConfig(), mMiscPool,
                      Database::getMiscDBName(mApp.getConfig().DATABASE.value));
}

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
}
