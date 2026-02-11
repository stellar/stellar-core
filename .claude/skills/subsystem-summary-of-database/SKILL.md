---
name: subsystem-summary-of-database
description: "read this skill for a token-efficient summary of the database subsystem"
---

# Database Subsystem — Technical Summary

## Overview

The database subsystem provides the persistence layer for stellar-core, wrapping the SOCI C++ database-access library to manage connections to SQLite or PostgreSQL backends. It handles schema versioning/migration, connection pooling for worker threads, metrics collection, and a dual-database architecture (main + misc) for SQLite to avoid write-lock contention.

## Key Files

- **Database.h / Database.cpp** — Core `Database` class; connection management, schema migration, pooling, metrics.
- **DatabaseTypeSpecificOperation.h** — Visitor pattern for backend-specific (SQLite vs PostgreSQL) code paths.
- **DatabaseConnectionString.h / .cpp** — Utility to redact passwords from connection strings for logging.
- **DatabaseUtils.h / .cpp** — Helper for batch-deleting old ledger entries from tables.

---

## Key Classes and Data Structures

### `Database` (inherits `NonMovableOrCopyable`)

The central class that owns all database connections for an `Application` instance. One `Database` exists per application.

**Members:**
- `mApp` (`Application&`) — Back-reference to the owning application.
- `mQueryMeter` (`medida::Meter&`) — Metrics meter counting all SQL query executions.
- `mSession` (`SessionWrapper`, name="main") — Primary SOCI session for ledger state; used for all writes on the main DB.
- `mMiscSession` (`SessionWrapper`, name="misc") — Secondary SOCI session for miscellaneous/consensus data (SQLite only).
- `mPool` / `mMiscPool` (`unique_ptr<soci::connection_pool>`) — Lazily-created connection pools for worker-thread read access.
- `gDriversRegistered` (static `bool`) — Ensures SOCI backend drivers (sqlite3, postgresql) are registered exactly once.

### `SessionWrapper` (inherits `NonCopyable`)

A thin RAII wrapper around `soci::session` that carries a human-readable session name (e.g., "main" or "misc"). Two constructors: one for standalone sessions, one that borrows from a `connection_pool`.

### `StatementContext` (inherits `NonCopyable`)

RAII handle for borrowing a SOCI prepared statement. On construction it calls `clean_up(false)` to unbind any prior data; on destruction it does the same cleanup. Returned by `Database::getPreparedStatement()`. Supports move semantics.

### `DatabaseTypeSpecificOperation<T>` (template, abstract)

A visitor/strategy pattern that allows callers to write code specific to the database backend without switching on backend type everywhere. Has two pure virtual methods:
- `doSqliteSpecificOperation(soci::sqlite3_session_backend* sq)` — SQLite path.
- `doPostgresSpecificOperation(soci::postgresql_session_backend* pg)` — PostgreSQL path (conditionally compiled under `USE_POSTGRES`).

Used via `Database::doDatabaseTypeSpecificOperation()` or the free-function overload that takes a raw `soci::session&`.

### `DatabaseConfigureSessionOp` (local to Database.cpp)

A concrete `DatabaseTypeSpecificOperation<void>` used internally during connection setup. Performs:
- **SQLite:** Checks minimum version (3.45+), sets WAL journal mode, autocheckpoint=10000, busy_timeout=10000ms, cache_size=20000 pages, mmap_size=100MB, and registers the `carray()` extension.
- **PostgreSQL:** Checks minimum version (9.5+), sets session transaction isolation to SERIALIZABLE.

---

## Schema Versioning

Two independent schema version tracks are maintained:

### Main DB Schema
- Constants: `MIN_SCHEMA_VERSION = 25`, `SCHEMA_VERSION = 26`.
- Version stored via `PersistentState::kDatabaseSchema` in the `storestate` table.
- `applySchemaUpgrade(vers)` applies incremental migrations in a SOCI transaction:
  - **v25→v26:** Drops deprecated `publishqueue` table; drops misc tables from main DB if misc DB is active.
  - **v24→v25:** Removes deprecated `dbbackend` entry from `storestate`.

### Misc DB Schema
- Constants: `MIN_MISC_SCHEMA_VERSION = 0`, `MISC_SCHEMA_VERSION = 1`.
- Version stored via `PersistentState::kMiscDatabaseSchema` in the misc DB's own storestate.
- `applyMiscSchemaUpgrade(vers)` applies incremental migrations:
  - **v0→v1:** Creates overlay/peer tables, persistent state, herder persistence, ban manager tables in the misc DB, then copies data from main via `populateMiscDatabase()`.

### Migration Flow (`upgradeToCurrentSchema()`)
1. Migrate misc DB first (if applicable): determines current misc version, runs `doMigration()`.
2. Migrate main DB: determines current main version, runs `doMigration()`.
3. `doMigration()` validates version bounds, then loops applying upgrades one version at a time, persisting the new version after each step.

---

## Dual-Database Architecture (Main + Misc)

SQLite locks the entire database file during writes, blocking parallelism between ledger apply and consensus/overlay operations. To mitigate this, the subsystem splits data across two SQLite files:

- **Main DB:** Ledger state (ledger headers, transaction history, ledger entries). Touched at startup and during apply.
- **Misc DB:** Consensus data (SCP quorums, SCP history, slot state), overlay data (peers, bans), upgrades. Tables migrated: `peers`, `ban`, `quoruminfo`, `scpquorums`, `scphistory`, `slotstate` (defined in `kMiscTables`).

**Applicability:** Only for on-disk SQLite (`canUseMiscDB()` returns `true` when `canUsePool() && isSqlite()`). PostgreSQL handles concurrent writes natively, so it uses a single database with `mMiscSession` falling back to `mSession`.

**Misc DB naming:** `getMiscDBName()` inserts "-misc" before the file extension of the main DB path (e.g., `stellar.db` → `stellar-misc.db`).

**Data migration (`populateMiscDatabase()`):**
1. Attaches main DB as `source_db` in the misc session.
2. For each table in `kMiscTables`, copies all rows via `INSERT INTO ... SELECT * FROM source_db.<table>`.
3. Verifies row counts match.
4. Detaches `source_db` after transaction commit to avoid lock contention.

---

## Connection Pooling

Pools are lazily created on first call to `getPool()` or `getMiscPool()`.

**Pool size:** `std::thread::hardware_concurrency()` entries. If misc DB is active, each pool gets half (min 1).

**Pool creation (`createPool()`):**
1. Allocates `soci::connection_pool` of size `n`.
2. Opens each session in the pool to the target DB.
3. Configures each session via `DatabaseConfigureSessionOp`.

**Access constraints:**
- `getSession()` and `getMiscSession()` assert `threadIsMain()` — direct session access is restricted to the main thread.
- Worker threads must use the pool (`getPool()` / `getMiscPool()`).

---

## Key Functions

### `Database` Public API

| Function | Purpose |
|----------|---------|
| `Database(Application&)` | Constructor: registers drivers, logs connection string (password-redacted), calls `open()`. |
| `open()` | Opens main session, configures it; opens misc session if `canUseMiscDB()`. |
| `initialize()` | Drops and recreates all tables (used by `new-db` command). For SQLite, deletes DB files first. Creates overlay, persistent state, ledger header, herder persistence, ban tables. |
| `upgradeToCurrentSchema()` | Runs schema migrations for both misc and main DBs. |
| `getPreparedStatement(query, session)` | Allocates and prepares a SOCI statement, returns it wrapped in `StatementContext`. |
| `getInsertTimer/getSelectTimer/getDeleteTimer/getUpdateTimer/getUpsertTimer(entityName)` | Returns a `medida::TimerContext` for timing and counting SQL operations, grouped by entity name. |
| `setCurrentTransactionReadOnly()` | On PostgreSQL, issues `SET TRANSACTION READ ONLY`. No-op on SQLite. |
| `isSqlite()` | Returns true if connection string contains `"sqlite3://"`. |
| `canUseMiscDB()` | True for on-disk SQLite only. |
| `canUsePool()` | True unless using in-memory SQLite (`sqlite3://:memory:`). |
| `getSimpleCollationClause()` | Returns `COLLATE "C"` for PostgreSQL (byte-value comparison), empty for SQLite. |
| `getSession() / getMiscSession()` | Returns main/misc `SessionWrapper`; asserts main thread. `getMiscSession()` falls back to main session if misc DB unavailable. |
| `getRawSession() / getRawMiscSession()` | Convenience accessors returning `soci::session&`. |
| `getPool() / getMiscPool()` | Returns (lazily-created) connection pools for worker threads. |
| `getMainDBSchemaVersion() / getMiscDBSchemaVersion()` | Reads schema version from persistent state. |
| `doDatabaseTypeSpecificOperation(session, op)` | Dispatches to the correct backend-specific method on `op` via `dynamic_cast` on the SOCI backend. |

### Free Functions

| Function | Purpose |
|----------|---------|
| `doDatabaseTypeSpecificOperation<T>(soci::session&, op)` | Non-member overload operating on raw `soci::session`. |
| `decodeOpaqueXDR<T>(string, out)` | Base64-decodes a string then XDR-deserializes into `out`. |
| `decodeOpaqueXDR<T>(string, indicator, out)` | Same but handles null indicators (sets `out = T{}` if null). |
| `removePasswordFromConnectionString(string)` | Regex-replaces password values with `********` in connection strings for safe logging. |
| `dropMiscTablesFromMain(Application&)` | Drops all `kMiscTables` from the main DB (called during v26 migration). |
| `validateVersion(vers, min, max)` | Throws if schema version is outside supported range. |

### `DatabaseUtils` Namespace

| Function | Purpose |
|----------|---------|
| `deleteOldEntriesHelper(sess, ledgerSeq, count, tableName, ledgerSeqColumn)` | Batch-deletes old rows: finds MIN of the ledger-seq column, deletes rows up to `min(curMin + count, ledgerSeq)`. Used for pruning historical data from various tables. |

---

## Ownership Relationships

```
Application
 └── Database (1:1, owned by Application)
      ├── mSession (SessionWrapper, "main") — primary DB session
      │    └── soci::session — actual SOCI connection
      ├── mMiscSession (SessionWrapper, "misc") — secondary DB session (SQLite only)
      │    └── soci::session — actual SOCI connection
      ├── mPool (unique_ptr<soci::connection_pool>) — lazily created, for main DB
      │    └── N × soci::session (one per hardware thread)
      └── mMiscPool (unique_ptr<soci::connection_pool>) — lazily created, for misc DB
           └── N/2 × soci::session
```

`StatementContext` objects are transient: created by `getPreparedStatement()`, hold a `shared_ptr<soci::statement>`, cleaned up when they go out of scope.

---

## Key Data Flows

### Startup / Initialization
1. `Application` constructs `Database`, which registers SOCI drivers and opens connections.
2. `open()` configures each session (SQLite pragmas or PostgreSQL isolation level).
3. If `new-db`: `initialize()` drops/recreates all tables, sets schema version to `MIN_SCHEMA_VERSION`.
4. Otherwise: `upgradeToCurrentSchema()` reads current versions and applies incremental migrations.

### SQL Statement Execution (typical pattern used by other subsystems)
1. Caller obtains a timer via `getInsertTimer("entity")` etc. — starts timing.
2. Caller obtains session via `getSession()` or `getMiscSession()`.
3. Caller either uses raw SOCI syntax (`session << "SQL..."`) or obtains a prepared statement via `getPreparedStatement(query, session)`.
4. `StatementContext` RAII ensures cleanup.
5. Timer destructor records elapsed time in metrics.

### Worker Thread Database Access
1. Worker thread calls `getPool()` or `getMiscPool()` to get a `soci::connection_pool`.
2. Worker constructs a `SessionWrapper` from the pool (SOCI automatically checks out/returns connections).
3. Worker performs read queries. Write operations are restricted to the main thread's session.

### Schema Migration
1. `upgradeToCurrentSchema()` is called during startup.
2. Misc DB migration runs first: if main schema ≥ 26, misc schema version is read; migrations applied (v0→v1 creates tables and copies data from main).
3. Main DB migration runs second: v25→v26 drops deprecated tables and removes misc tables from main if misc DB is active.
4. Each step runs in a SOCI transaction, version is persisted after commit.

### Data Pruning
1. Subsystems call `DatabaseUtils::deleteOldEntriesHelper()` with a target ledger sequence, batch count, table name, and column name.
2. Helper finds the minimum ledger sequence in the table, computes an upper bound, and batch-deletes old rows up to that bound.

---

## Constants

| Constant | Value | Purpose |
|----------|-------|---------|
| `MIN_SCHEMA_VERSION` | 25 | Oldest main DB schema this binary can open |
| `SCHEMA_VERSION` | 26 | Current target main DB schema |
| `FIRST_MAIN_VERSION_WITH_MISC` | 26 | Main schema version at which misc DB was introduced |
| `MIN_MISC_SCHEMA_VERSION` | 0 | Oldest misc DB schema (0 = no misc table yet) |
| `MISC_SCHEMA_VERSION` | 1 | Current target misc DB schema |
| `MIN_SQLITE_VERSION` | 3.45 | Minimum SQLite version (compiled check) |
| `MIN_POSTGRESQL_VERSION` | 9.5 | Minimum PostgreSQL version |

## Threading Model

- **Main thread:** Owns `mSession` and `mMiscSession`. All write operations go through these. Access is guarded by `releaseAssert(threadIsMain())`.
- **Worker threads:** Read-only access via connection pools (`mPool`, `mMiscPool`). Each pool entry is an independently configured SOCI session.
- **SQLite concurrency:** WAL mode allows concurrent readers with a single writer. The dual-DB split (main/misc) further reduces write contention by separating consensus writes from ledger-apply writes into different files with independent locks.
- **PostgreSQL concurrency:** Single DB with SERIALIZABLE isolation; concurrent writes are handled by the database engine natively.
