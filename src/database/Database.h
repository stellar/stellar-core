#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <string>
#include <soci.h>
#include "generated/StellarXDR.h"
#include "ledger/AccountFrame.h"
#include "ledger/OfferFrame.h"
#include "ledger/TrustFrame.h"
#include "medida/timer_context.h"
#include "util/NonCopyable.h"

namespace medida
{
class Meter;
class Timer;
class Counter;
}

namespace stellar
{
class Application;
class SQLLogContext;

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
    soci::session mSession;
    std::unique_ptr<soci::connection_pool> mPool;

    std::map<std::string, std::shared_ptr<soci::statement>> mStatements;
    medida::Counter& mStatementsSize;

    static bool gDriversRegistered;
    static void registerDrivers();

  public:
    // Instantiate object and connect to app.getConfig().DATABASE;
    // if there is a connection error, this will throw.
    Database(Application& app);

    // Return a logging helper that will capture all SQL statements made
    // on the main connection while active, and will log those statements
    // to the process' log for diagnostics. For testing and perf tuning.
    std::shared_ptr<SQLLogContext> captureAndLogSQL(std::string contextName);

    // Return a helper object that borrows, from the Database, a prepared
    // statement handle for the provided query. The prepared statement handle
    // is ceated if necessary before borrowing, and reset (unbound from data)
    // when the statement context is destroyed.
    StatementContext getPreparedStatement(std::string const& query);

    // Return metric-gathering timers for various families of SQL operation.
    // These timers automatically count the time they are alive for,
    // so only acquire them immediately before executing an SQL statement.
    medida::TimerContext getInsertTimer(std::string const& entityName);
    medida::TimerContext getSelectTimer(std::string const& entityName);
    medida::TimerContext getDeleteTimer(std::string const& entityName);
    medida::TimerContext getUpdateTimer(std::string const& entityName);

    // Return true if the Database target is SQLite, otherwise false.
    bool isSqlite() const;

    // Return true if a connection pool is available for worker threads
    // to read from the database through, otherwise false.
    bool canUsePool() const;

    // Drop and recreate all tables in the database target. This is called
    // by the --newdb command-line flag on stellar-core.
    void initialize();

    // Access the underlying SOCI session object
    soci::session& getSession();

    // Access the optional SOCI connection pool available for worker
    // threads. Throws an error if !canUsePool().
    soci::connection_pool& getPool();
};
}
