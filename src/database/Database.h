#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "database/DatabaseTypeSpecificOperation.h"
#include "medida/timer_context.h"
#include "overlay/StellarXDR.h"
#include "util/Decoder.h"
#include "util/NonCopyable.h"
#include "util/Timer.h"
#include <functional>
#include <set>
#include <soci.h>
#include <string>
#include <unordered_map>
#include <xdrpp/marshal.h>

namespace medida
{
class Meter;
class Counter;
}

namespace stellar
{
class Application;
using PreparedStatementCache =
    std::map<std::string, std::shared_ptr<soci::statement>>;

// smallest schema version supported
static constexpr unsigned long MIN_SCHEMA_VERSION = 21;
static constexpr unsigned long SCHEMA_VERSION = 24;

/**
 * Helper class for borrowing a SOCI prepared statement handle into a local
 * scope and cleaning it up once done with it. Returned by
 * Database::getPreparedStatement below.
 */
class StatementContext : NonCopyable
{
    std::shared_ptr<soci::statement> mStmt;

  public:
    StatementContext(std::shared_ptr<soci::statement> stmt) : mStmt(stmt)
    {
        mStmt->clean_up(false);
    }
    StatementContext(StatementContext&& other)
    {
        mStmt = other.mStmt;
        other.mStmt.reset();
    }
    ~StatementContext()
    {
        if (mStmt)
        {
            mStmt->clean_up(false);
        }
    }
    soci::statement&
    statement()
    {
        return *mStmt;
    }
};

class SessionWrapper : NonCopyable
{
    soci::session mSession;
    std::string const mSessionName;

  public:
    SessionWrapper(std::string sessionName)
        : mSessionName(std::move(sessionName))
    {
    }
    SessionWrapper(std::string sessionName, soci::connection_pool& pool)
        : mSession(pool), mSessionName(std::move(sessionName))
    {
    }

    soci::session&
    session()
    {
        return mSession;
    }
    std::string const&
    getSessionName() const
    {
        return mSessionName;
    }
};

/**
 * Object that owns the database connection(s) that an application
 * uses to store the current ledger and other persistent state in.
 *
 * This may represent an in-memory SQLite instance (for testing), an on-disk
 * SQLite instance (for running a minimal, self-contained server) or a
 * connection to a local Postgresql database, that the node operator must have
 * set up on their own.
 *
 * Database connects, on construction, to the target specified by the
 * application Config object's Config::DATABASE value; this originates from the
 * config-file's DATABASE string. The default is "sqlite3://:memory:". This
 * "main connection" is where most SQL statements -- and all write-statements --
 * are executed.
 *
 * Database may establish additional connections for worker threads to read
 * data, from a separate connection pool, if worker threads request them. The
 * pool will connect to the same target and only one connection will be made per
 * worker thread.
 *
 * All database connections and transactions are set to snapshot isolation level
 * (SQL isolation level 'SERIALIZABLE' in Postgresql and Sqlite, neither of
 * which provide true serializability).
 */

class Database : NonMovableOrCopyable
{
    Application& mApp;
    medida::Meter& mQueryMeter;
    SessionWrapper mSession;

    std::unique_ptr<soci::connection_pool> mPool;

    // Cache key -> session name <> query
    using PreparedStatementCache =
        std::unordered_map<std::string, std::shared_ptr<soci::statement>>;
    std::unordered_map<std::string, PreparedStatementCache> mCaches;

    medida::Counter& mStatementsSize;

    static bool gDriversRegistered;
    static void registerDrivers();
    void applySchemaUpgrade(unsigned long vers);
    void open();
    // Save `vers` as schema version.
    void putSchemaVersion(unsigned long vers);

    // Prepared statements cache may be accessed by multiple threads (each using
    // a different session), so use a mutex to synchronize access.
    std::mutex mutable mStatementsMutex;

  public:
    // Instantiate object and connect to app.getConfig().DATABASE;
    // if there is a connection error, this will throw.
    Database(Application& app);

    virtual ~Database()
    {
    }

    // Return a helper object that borrows, from the Database, a prepared
    // statement handle for the provided query. The prepared statement handle
    // is created if necessary before borrowing, and reset (unbound from data)
    // when the statement context is destroyed. Prepared statements caches are
    // per DB session.
    StatementContext getPreparedStatement(std::string const& query,
                                          SessionWrapper& session);

    // Purge all cached prepared statements, closing their handles with the
    // database.
    void clearPreparedStatementCache(SessionWrapper& session);
    void clearPreparedStatementCache();

    // Return metric-gathering timers for various families of SQL operation.
    // These timers automatically count the time they are alive for,
    // so only acquire them immediately before executing an SQL statement.
    medida::TimerContext getInsertTimer(std::string const& entityName);
    medida::TimerContext getSelectTimer(std::string const& entityName);
    medida::TimerContext getDeleteTimer(std::string const& entityName);
    medida::TimerContext getUpdateTimer(std::string const& entityName);
    medida::TimerContext getUpsertTimer(std::string const& entityName);

    // If possible (i.e. "on postgres") issue an SQL pragma that marks
    // the current transaction as read-only. The effects of this last
    // only as long as the current SQL transaction.
    void setCurrentTransactionReadOnly();

    // Return true if the Database target is SQLite, otherwise false.
    bool isSqlite() const;

    // Return an optional SQL COLLATION clause to use for text-typed columns in
    // this database, in order to ensure they're compared "simply" using
    // byte-value comparisons, i.e. in a non-language-sensitive fashion.  For
    // Postgresql this will be 'COLLATE "C"' and for SQLite, nothing (its
    // defaults are correct already).
    std::string getSimpleCollationClause() const;

    // Call `op` back with the specific database backend subtype in use.
    template <typename T>
    T doDatabaseTypeSpecificOperation(DatabaseTypeSpecificOperation<T>& op,
                                      SessionWrapper& session);

    // Return true if a connection pool is available for worker threads
    // to read from the database through, otherwise false.
    bool canUsePool() const;

    // Drop and recreate all tables in the database target. This is called
    // by the new-db command on stellar-core.
    void initialize();

    // Get current schema version in DB.
    unsigned long getDBSchemaVersion();

    // Check schema version and apply any upgrades if necessary.
    void upgradeToCurrentSchema();

    void dropTxMetaIfExists();
    void maybeUpgradeToBucketListDB();

    // Soci named session wrapper
    SessionWrapper& getSession();
    // Access the underlying SOCI session object
    // Use these to directly access the soci session object
    soci::session& getRawSession();

    // Access the optional SOCI connection pool available for worker
    // threads. Throws an error if !canUsePool().
    soci::connection_pool& getPool();
};

template <typename T>
T
doDatabaseTypeSpecificOperation(soci::session& session,
                                DatabaseTypeSpecificOperation<T>& op)
{
    auto b = session.get_backend();
    if (auto sq = dynamic_cast<soci::sqlite3_session_backend*>(b))
    {
        return op.doSqliteSpecificOperation(sq);
#ifdef USE_POSTGRES
    }
    else if (auto pg = dynamic_cast<soci::postgresql_session_backend*>(b))
    {
        return op.doPostgresSpecificOperation(pg);
#endif
    }
    else
    {
        // Extend this with other cases if we support more databases.
        abort();
    }
}

template <typename T>
T
Database::doDatabaseTypeSpecificOperation(DatabaseTypeSpecificOperation<T>& op,
                                          SessionWrapper& session)
{
    return stellar::doDatabaseTypeSpecificOperation(session.session(), op);
}

template <typename T>
void
decodeOpaqueXDR(std::string const& in, T& out)
{
    std::vector<uint8_t> opaque;
    decoder::decode_b64(in, opaque);
    xdr::xdr_from_opaque(opaque, out);
}

template <typename T>
void
decodeOpaqueXDR(std::string const& in, soci::indicator const& ind, T& out)
{
    if (ind == soci::i_ok)
    {
        decodeOpaqueXDR(in, out);
    }
    else
    {
        out = T{};
    }
}
}
