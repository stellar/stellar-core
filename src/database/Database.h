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
#include <xdrpp/marshal.h>

namespace medida
{
class Meter;
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
    medida::Meter& mQueryMeter;
    soci::session mSession;
    std::unique_ptr<soci::connection_pool> mPool;

    std::map<std::string, std::shared_ptr<soci::statement>> mStatements;
    medida::Counter& mStatementsSize;

    std::set<std::string> mEntityTypes;

    static bool gDriversRegistered;
    static void registerDrivers();
    void applySchemaUpgrade(unsigned long vers);

    // Convert the accounts table from using explicit entries for
    // extension fields into storing the entire extension as opaque XDR.
    void convertAccountExtensionsToOpaqueXDR();
    void copyIndividualAccountExtensionFieldsToOpaqueXDR();

    std::string getOldLiabilitySelect(std::string const& table,
                                      std::string const& fields);
    void addTextColumn(std::string const& table, std::string const& column);
    void dropNullableColumn(std::string const& table,
                            std::string const& column);

    // Convert the trustlines table from using explicit entries for
    // extension fields into storing the entire extension as opaque XDR.
    void convertTrustLineExtensionsToOpaqueXDR();
    void copyIndividualTrustLineExtensionFieldsToOpaqueXDR();

  public:
    // Instantiate object and connect to app.getConfig().DATABASE;
    // if there is a connection error, this will throw.
    Database(Application& app);

    virtual ~Database()
    {
    }

    // Return a logging helper that will capture all SQL statements made
    // on the main connection while active, and will log those statements
    // to the process' log for diagnostics. For testing and perf tuning.
    std::shared_ptr<SQLLogContext> captureAndLogSQL(std::string contextName);

    // Return a helper object that borrows, from the Database, a prepared
    // statement handle for the provided query. The prepared statement handle
    // is created if necessary before borrowing, and reset (unbound from data)
    // when the statement context is destroyed.
    StatementContext getPreparedStatement(std::string const& query);

    // Purge all cached prepared statements, closing their handles with the
    // database.
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
    T doDatabaseTypeSpecificOperation(DatabaseTypeSpecificOperation<T>& op);

    // Return true if a connection pool is available for worker threads
    // to read from the database through, otherwise false.
    bool canUsePool() const;

    // Drop and recreate all tables in the database target. This is called
    // by the new-db command on stellar-core.
    void initialize();

    // Save `vers` as schema version.
    void putSchemaVersion(unsigned long vers);

    // Get current schema version in DB.
    unsigned long getDBSchemaVersion();

    // Get current schema version of running application.
    unsigned long getAppSchemaVersion();

    // Check schema version and apply any upgrades if necessary.
    void upgradeToCurrentSchema();

    // Access the underlying SOCI session object
    soci::session& getSession();

    // Access the optional SOCI connection pool available for worker
    // threads. Throws an error if !canUsePool().
    soci::connection_pool& getPool();

  protected:
    // Give clients the opportunity to perform operations on databases while
    // they're still using old schemas (prior to the upgrade that occurs either
    // immediately after database creation or after loading a version of
    // stellar-core that introduces a new schema).
    virtual void
    actBeforeDBSchemaUpgrade()
    {
    }
};

template <typename T>
T
Database::doDatabaseTypeSpecificOperation(DatabaseTypeSpecificOperation<T>& op)
{
    auto b = mSession.get_backend();
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

// Select a set of records using a client-defined query string, then map
// each record into an element of a client-defined datatype by applying a
// client-defined function (the records are accumulated in the "out"
// vector).
template <typename T>
void
selectMap(Database& db, std::string const& selectStr,
          std::function<T(soci::row const&)> makeT, std::vector<T>& out)
{
    soci::rowset<soci::row> rs = (db.getSession().prepare << selectStr);

    std::transform(rs.begin(), rs.end(), std::back_inserter(out), makeT);
}

// Map each element in the given vector of a client-defined datatype into a
// SQL update command by applying a client-defined function, then send those
// update strings to the database.
//
// The "postUpdate" function receives the number of records affected
// by the given update, as well as the element of the client-defined
// datatype which generated that update.
template <typename T>
void updateMap(Database& db, std::vector<T> const& in,
               std::string const& updateStr,
               std::function<void(soci::statement&, T const&)> prepUpdate,
               std::function<void(long long const, T const&)> postUpdate);
template <typename T>
void
updateMap(Database& db, std::vector<T> const& in, std::string const& updateStr,
          std::function<void(soci::statement&, T const&)> prepUpdate,
          std::function<void(long long const, T const&)> postUpdate)
{
    auto st_update = db.getPreparedStatement(updateStr).statement();

    for (auto& recT : in)
    {
        prepUpdate(st_update, recT);
        st_update.define_and_bind();
        st_update.execute(true);
        auto affected_rows = st_update.get_affected_rows();
        st_update.clean_up(false);
        postUpdate(affected_rows, recT);
    }
}

// The composition of updateMap() following selectMap().
//
// Returns the number of records selected by selectMap() (all of which were
// then passed through updateMap() before the selectUpdateMap() call
// returned).
template <typename T>
size_t
selectUpdateMap(Database& db, std::string const& selectStr,
                std::function<T(soci::row const&)> makeT,
                std::string const& updateStr,
                std::function<void(soci::statement&, T const&)> prepUpdate,
                std::function<void(long long const, T const&)> postUpdate)
{
    std::vector<T> vecT;

    selectMap<T>(db, selectStr, makeT, vecT);
    updateMap<T>(db, vecT, updateStr, prepUpdate, postUpdate);

    return vecT.size();
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
}
}
