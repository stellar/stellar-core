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

static std::string
getMiscDBName(std::string const& mainDB)
{
    std::string miscDB = mainDB.substr(0, mainDB.length() - 3) + "-misc.db";
    return miscDB;
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
    doDatabaseTypeSpecificOperation(op, mSession);

    if (canUseMiscDB())
    {
        std::string miscDB = getMiscDBName(mApp.getConfig().DATABASE.value);
        mMiscSession.session().open(miscDB);
        DatabaseConfigureSessionOp miscOp(mMiscSession.session());
        doDatabaseTypeSpecificOperation(miscOp, mMiscSession);
    }
}

static std::string
getDeleteStateQuery(Application& app, bool isMain)
{
    auto const& ps = app.getPersistentState();
    std::vector<std::string> states;
    for (auto let : xdr::xdr_traits<LedgerEntryType>::enum_values())
    {
        states.push_back(
            ps.getStoreStateName(PersistentState::kRebuildLedger, let));
    }
    states.push_back(ps.getStoreStateName(PersistentState::kLastClosedLedger));
    states.push_back(
        ps.getStoreStateName(PersistentState::kHistoryArchiveState));
    states.push_back(ps.getStoreStateName(PersistentState::kDatabaseSchema));
    states.push_back(ps.getStoreStateName(PersistentState::kNetworkPassphrase));
    states.push_back(ps.getStoreStateName(PersistentState::kDBBackend));

    std::string query = fmt::format(
        "DELETE FROM storestate WHERE statename {} IN (", isMain ? "NOT" : "");
    bool first = true;
    for (auto const& stateName : states)
    {
        query = fmt::format("{}{}'{}'", query, first ? "" : ", ", stateName);
        first = false;
    }
    query += ")";
    return query;
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
    auto loc = getSQLiteDBLocation(getRawMiscSession());
    releaseAssert(!loc.empty());

    // Step 1: Attach the source database
    getRawMiscSession() << "ATTACH DATABASE '" + loc + "' AS source_db";

    // Step 2: Retrieve table names from the source database
    std::vector<std::string> tableNames = {
        "peers",      "ban",    "quoruminfo", "scpquorums",
        "scphistory", "pubsub", "storestate"};

    // Step 3: Copy data from each table
    for (const auto& tableName : tableNames)
    {
        std::string insertQuery = "INSERT INTO " + tableName +
                                  " SELECT * FROM source_db." + tableName;

        getRawMiscSession() << insertQuery;
        CLOG_INFO(Database, "Data from {} copied successfully!", tableName);
    }

    // Step 4: Remove data from storestate that doesn't belong there
    getRawMiscSession() << getDeleteStateQuery(mApp, /* isMain */ false);
    CLOG_INFO(Database, "Data from storestate removed successfully!");

    // Step 5: Persist misc DB schema version
    putMiscSchemaVersion(MISC_SCHEMA_VERSION);
    CLOG_INFO(Database, "Misc: Schema version set to {}", MISC_SCHEMA_VERSION);
}

void
Database::applyMiscSchemaUpgrade(unsigned long vers)
{
    clearPreparedStatementCache();
    soci::transaction tx(mMiscSession.session());
    switch (vers)
    {
    case 1:
        // Create tables for the first time.
        OverlayManager::dropAll(*this);
        PersistentState::dropMisc(*this);
        ExternalQueue::dropAll(*this);
        HerderPersistence::dropAll(*this);
        BanManager::dropAll(*this);
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
    db.getRawSession() << "DROP TABLE IF EXISTS peers;";
    db.getRawSession() << "DROP TABLE IF EXISTS ban;";
    db.getRawSession() << "DROP TABLE IF EXISTS quoruminfo;";
    db.getRawSession() << "DROP TABLE IF EXISTS scpquorums;";
    db.getRawSession() << "DROP TABLE IF EXISTS scphistory;";
    db.getRawSession() << "DROP TABLE IF EXISTS pubsub;";
    db.getRawSession() << getDeleteStateQuery(app, /* isMain */ true);
}

void
Database::applySchemaUpgrade(unsigned long vers)
{
    clearPreparedStatementCache();

    soci::transaction tx(mSession.session());
    switch (vers)
    {
    case 22:
        dropSupportTransactionFeeHistory(*this);
        break;
    case 23:
        mApp.getHistoryManager().dropSQLBasedPublish();
        Upgrades::dropSupportUpgradeHistory(*this);
        if (canUseMiscDB())
        {
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
    auto mainVers = getMainDBSchemaVersion();
    auto miscVers = 0;
    if (mainVers >= FIRST_MAIN_VERSION_WITH_MISC && canUseMiscDB())
    {
        miscVers = getMiscDBSchemaVersion();
        doMigration(miscVers, MIN_MISC_SCHEMA_VERSION, MISC_SCHEMA_VERSION,
                    false);
    }
    doMigration(mainVers, MIN_MAIN_SCHEMA_VERSION, MAIN_SCHEMA_VERSION, true);
    CLOG_INFO(Database, "DB schema is in current version");
}

void
Database::putMainSchemaVersion(unsigned long vers)
{
    mApp.getPersistentState().setState(PersistentState::kDatabaseSchema,
                                       std::to_string(vers),
                                       mApp.getDatabase().getSession());
}

void
Database::putMiscSchemaVersion(unsigned long vers)
{
    mApp.getPersistentState().setState(PersistentState::kMiscDatabaseSchema,
                                       std::to_string(vers),
                                       mApp.getDatabase().getMiscSession());
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
    return !(mApp.getConfig().DATABASE.value == ("sqlite3://:memory:"));
}

bool
Database::canUseMiscDB() const
{
    return canUsePool() && isSqlite();
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
    Upgrades::dropSupportUpgradeHistory(*this);
    OverlayManager::dropAll(*this);
    PersistentState::dropAll(*this);
    ExternalQueue::dropAll(*this);
    LedgerHeaderUtils::dropAll(*this);
    // No need to re-create txhistory, will be dropped during
    // upgradeToCurrentSchema anyway
    dropSupportTxHistory(*this);
    HistoryManager::dropAll(*this);
    HerderPersistence::dropAll(*this);
    BanManager::dropAll(*this);
    putMainSchemaVersion(MIN_MAIN_SCHEMA_VERSION);
    if (canUseMiscDB())
    {
        putMiscSchemaVersion(MIN_MISC_SCHEMA_VERSION);
    }

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

soci::connection_pool&
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
                      getMiscDBName(mApp.getConfig().DATABASE.value));
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
    auto cacheKey = PrepStatementCacheKey(session.getSessionName(), query);
    auto i = mStatements.find(cacheKey);
    std::shared_ptr<soci::statement> p;
    if (i == mStatements.end())
    {
        p = std::make_shared<soci::statement>(session.session());
        p->alloc();
        p->prepare(query);
        mStatements.insert(std::make_pair(cacheKey, p));
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
    return make_shared<SQLLogContext>(contextName, mSession.session());
}
}
