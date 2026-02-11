---
name: subsystem-summary-of-main
description: "read this skill for a token-efficient summary of the main subsystem"
---

# Main Subsystem — Technical Summary

## Overview

The main subsystem is the central orchestration layer of stellar-core. It defines the `Application` interface and its concrete `ApplicationImpl`, which owns all other subsystem managers and coordinates the application lifecycle: construction, initialization, startup, the main event loop, graceful shutdown, and thread management. It also provides configuration parsing (`Config`), persistent state management (`PersistentState`), HTTP command handling (`CommandHandler`), query serving (`QueryServer`), CLI entry point and command routing (`CommandLine`, `main.cpp`), and various utilities (XDR dumping, diagnostics, settings upgrade helpers, maintenance).

## Key Files

- **Application.h / Application.cpp** — Abstract `Application` interface; factory method `Application::create()`.
- **ApplicationImpl.h / ApplicationImpl.cpp** — Concrete implementation; owns all subsystem managers, threads, io_contexts.
- **AppConnector.h / AppConnector.cpp** — Thread-safe accessor facade isolating subsystems from direct `Application` access.
- **Config.h / Config.cpp** — `Config` class; TOML-based configuration parsing, defaults, validation.
- **PersistentState.h / PersistentState.cpp** — Key-value persistence of node-critical state (LCL, SCP data, upgrades) across two SQL tables.
- **CommandHandler.h / CommandHandler.cpp** — HTTP admin command server routing and handler implementations.
- **QueryServer.h / QueryServer.cpp** — Multi-threaded HTTP query server for BucketListDB reads.
- **CommandLine.h / CommandLine.cpp** — CLI argument parsing via Clara; dispatches to run functions for each subcommand.
- **main.cpp** — Entry point; initializes crypto, checks XDR/version identity, delegates to `handleCommandLine()`.
- **Maintainer.h / Maintainer.cpp** — Periodic maintenance (prunes old SCP history, ledger data).
- **ApplicationUtils.h / ApplicationUtils.cpp** — Higher-level application utilities (setupApp, runApp, catchup, selfCheck, dumpLedger, etc.).
- **Diagnostics.h / Diagnostics.cpp** — Bucket statistics dumping utility.
- **dumpxdr.h / dumpxdr.cpp** — XDR file introspection, printing, and transaction signing utilities.
- **SettingsUpgradeUtils.h / SettingsUpgradeUtils.cpp** — Helpers to build Soroban settings upgrade transactions.
- **ErrorMessages.h** — Constant error message strings for common failure modes.
- **StellarCoreVersion.h** — Declares the `STELLAR_CORE_VERSION` string constant.

---

## Key Classes and Data Structures

### `Application` (abstract base class)

Defines the interface for a stellar-core application instance. Multiple instances can coexist in a single process (used in tests/simulations). Key aspects:

**State enum (`Application::State`):**
- `APP_CREATED_STATE` — Constructed but not started.
- `APP_ACQUIRING_CONSENSUS_STATE` — Out of sync with SCP peers.
- `APP_CONNECTED_STANDBY_STATE` — Tracking network but ledger subsystem still booting.
- `APP_CATCHING_UP_STATE` — Downloading/applying catchup data.
- `APP_SYNCED_STATE` — Fully synced, applying transactions.
- `APP_STOPPING_STATE` — Shutting down.

**ThreadType enum:**
- `MAIN`, `WORKER`, `EVICTION`, `OVERLAY`, `APPLY`.

**Key pure virtual methods:**
- `initialize(bool newDB, bool forceRebuild)` — Set up subsystems, DB.
- `start()` — Load last known ledger, start services.
- `gracefulStop()` / `joinAllThreads()` — Shutdown lifecycle.
- Subsystem accessors: `getLedgerManager()`, `getBucketManager()`, `getHerder()`, `getOverlayManager()`, `getDatabase()`, `getHistoryManager()`, etc.
- Thread dispatch: `postOnMainThread()`, `postOnBackgroundThread()`, `postOnOverlayThread()`, `postOnLedgerCloseThread()`, `postOnEvictionBackgroundThread()`.
- IO contexts: `getWorkerIOContext()`, `getEvictionIOContext()`, `getOverlayIOContext()`, `getLedgerCloseIOContext()`.
- Factory: `static Application::create(VirtualClock&, Config const&, ...)` — creates `ApplicationImpl`.

### `ApplicationImpl` (extends `Application`)

Concrete implementation. Central object that owns all subsystem managers and threads.

**Owned io_contexts (field order matters for construction/destruction):**
- `mWorkerIOContext` — Worker thread pool IO context (WORKER_THREADS - 1 threads).
- `mEvictionIOContext` — Single-thread IO context for eviction scans (medium priority).
- `mOverlayIOContext` — Optional single-thread IO context for background overlay processing.
- `mLedgerCloseIOContext` — Optional single-thread IO context for parallel ledger apply.

**Owned subsystem managers (all `unique_ptr`):**
- `mBucketManager`, `mDatabase`, `mOverlayManager`, `mLedgerManager`, `mHerder`, `mLedgerApplyManager`, `mHerderPersistence`, `mHistoryArchiveManager`, `mHistoryManager`, `mInvariantManager`, `mMaintainer`, `mPersistentState`, `mBanManager`, `mStatusManager`, `mLedgerTxnRoot`, `mAppConnector`, `mCommandHandler`.
- `mProcessManager`, `mWorkScheduler` — `shared_ptr`.

**Thread management:**
- `mWorkerThreads` — `vector<unique_ptr<thread>>`, run low-priority CPU-bound work.
- `mEvictionThread` — Single medium-priority thread for eviction scans.
- `mOverlayThread` — Optional thread for background overlay processing.
- `mLedgerCloseThread` — Optional thread for parallel ledger close/apply.
- `mThreadTypes` — `unordered_map<thread::id, ThreadType>`, populated at construction (read-only thereafter for thread safety).

**Key methods:**
- `initialize()` — Creates all subsystem managers in order: AppConnector → BucketManager → Database → PersistentState → OverlayManager → LedgerManager → Herder → all others. Registers invariants. Runs `newDB()` or `upgradeToCurrentSchemaAndMaybeRebuildLedger()`.
- `start()` — Loads last known ledger, enables Rust Dalek verification if protocol ≥ 25, calls `startServices()`.
- `startServices()` — Starts InvariantManager, Herder, Maintainer, OverlayManager; publishes queued history; optionally bootstraps SCP.
- `gracefulStop()` — Sets `mStopping`, calls `idempotentShutdown(true)`, schedules final IO context shutdown after a delay.
- `idempotentShutdown(forgetBuckets)` — Ordered shutdown: ledger close thread first, then CommandHandler, OverlayManager, WorkScheduler, ProcessManager, BucketManager (optionally forgets unreferenced buckets), Herder, main IO context, join all threads.
- `joinAllThreads()` — Releases work guards and joins ledger-close, worker, overlay, eviction threads.
- `getState()` — Derives application state from Herder and LedgerManager state.
- `manualClose()` — For testing: triggers manual ledger close via Herder.
- `syncOwnMetrics()` / `syncAllMetrics()` — Flushes crypto cache stats, process stats, overlay connection stats, and delegates to subsystem `syncMetrics()`.
- `postOnMainThread/BackgroundThread/OverlayThread/LedgerCloseThread()` — Posts closures to respective io_contexts with jitter injection and delay metrics.

### `AppConnector`

Thread-safe facade providing controlled access to `Application` from subsystems that may run on non-main threads.

**Design:** Holds a reference to `Application` and a **copy** of `Config` (to avoid thread-sanitizer warnings from accessing `mApp` config from background threads).

**Main-thread-only methods:** `getHerder()`, `getLedgerManager()`, `getOverlayManager()`, `getBanManager()`, `shouldYield()`, `checkOnOperationApply()`.

**Thread-safe methods:** `postOnMainThread()`, `postOnOverlayThread()`, `postOnBackgroundThread()`, `getConfig()`, `getMetrics()`, `now()`, `getOverlayMetrics()`, `isStopping()`, `getNetworkID()`, `getSorobanMetrics()`, `getModuleCache()`, `threadIsType()`, `copySearchableLiveBucketListSnapshot()`, `copySearchableHotArchiveBucketListSnapshot()`, `getOverlayThreadSnapshot()`.

### `Config`

Comprehensive configuration object parsed from TOML files. Copied locally by each `Application` at construction (immutable thereafter).

**Major configuration groups:**
- **Node identity:** `NODE_SEED` (SecretKey), `NODE_IS_VALIDATOR`, `NODE_HOME_DOMAIN`.
- **Network:** `NETWORK_PASSPHRASE`, `PEER_PORT`, peer connection limits, flood rates.
- **SCP:** `QUORUM_SET`, `FORCE_SCP`, `FAILURE_SAFETY`, `UNSAFE_QUORUM`.
- **Ledger:** `LEDGER_PROTOCOL_VERSION` (current: 25), `MAX_SLOTS_TO_REMEMBER`.
- **Database:** `DATABASE` connection string.
- **History:** `HISTORY` map of archive configurations.
- **BucketListDB:** `BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT`, `BUCKETLIST_DB_INDEX_CUTOFF`, `BUCKETLIST_DB_PERSIST_INDEX`, `BUCKETLIST_DB_MEMORY_FOR_CACHING`.
- **HTTP:** `HTTP_PORT`, `HTTP_QUERY_PORT`, `PUBLIC_HTTP_PORT`, `HTTP_MAX_CLIENT`.
- **Threading:** `WORKER_THREADS`, `QUERY_THREAD_POOL_SIZE`, `COMPILATION_THREADS`.
- **Maintenance:** `AUTOMATIC_MAINTENANCE_PERIOD` (default 359s), `AUTOMATIC_MAINTENANCE_COUNT` (default 400), `AUTOMATIC_SELF_CHECK_PERIOD` (default 3h).
- **Metadata:** `METADATA_OUTPUT_STREAM`, `METADATA_DEBUG_LEDGERS`.
- **Parallel processing:** `BACKGROUND_OVERLAY_PROCESSING`, `PARALLEL_LEDGER_APPLY`, `BACKGROUND_TX_SIG_VERIFICATION`.
- **Flow control:** `PEER_READING_CAPACITY`, `PEER_FLOOD_READING_CAPACITY`, `FLOW_CONTROL_SEND_MORE_BATCH_SIZE`, byte-based flow control params.
- **Testing-only flags:** Numerous `ARTIFICIALLY_*` and `*_FOR_TESTING` parameters (guarded at config-load time against production use).
- **Validator weights:** `VALIDATOR_WEIGHT_CONFIG` for leader election.
- **Events:** `EMIT_CLASSIC_EVENTS`, `BACKFILL_STELLAR_ASSET_EVENTS`, `EMIT_SOROBAN_TRANSACTION_META_EXT_V1`.

**Key methods:** `load(filename)`, `load(istream)`, `adjust()` (fixes connection-related settings), `logBasicInfo()`, `parallelLedgerClose()`, `setNoListen()`, `setNoPublish()`, `toShortString()`, `resolveNodeID()`.

**TestDbMode enum:** `TESTDB_DEFAULT`, `TESTDB_IN_MEMORY`, `TESTDB_POSTGRESQL`, `TESTDB_BUCKET_DB_VOLATILE`, `TESTDB_BUCKET_DB_PERSISTENT`.

### `PersistentState`

Manages critical node state persisted across restarts via two SQL tables: `storestate` (main/LCL data) and `slotstate` (SCP/consensus data).

**Entry enum (key names):**
- Main entries: `kLastClosedLedger`, `kHistoryArchiveState`, `kDatabaseSchema`, `kNetworkPassphrase`, `kRebuildLedger`.
- Misc/SCP entries: `kMiscDatabaseSchema`, `kLedgerUpgrades`, `kLastSCPDataXDR`, `kTxSet`.

**Key methods:** `getState()`, `setMainState()`, `setMiscState()`, `getSCPStateAllSlots()`, `setSCPStateV1ForSlot()`, `getTxSetsForAllSlots()`, `shouldRebuildForOfferTable()`, `hasTxSet()`, `deleteTxSets()`.

### `CommandHandler`

HTTP admin server handling operational commands. Binds to `HTTP_PORT` on the main thread's io_context.

**Routes (non-standalone):** `bans`, `connect`, `droppeer`, `peers`, `quorum`, `scp`, `surveyTopology*`, `unban`.
**Routes (always):** `info`, `ll`, `logrotate`, `manualclose`, `metrics`, `clearmetrics`, `tx`, `upgrades`, `dumpproposedsettings`, `self-check`, `maintenance`, `sorobaninfo`.
**Test-only routes:** `generateload`, `testacc`, `testtx`, `toggleoverlayonlymode`.

Also optionally creates a `QueryServer` if `HTTP_QUERY_PORT` is configured.

### `QueryServer`

Multi-threaded HTTP query server running on its own thread pool. Serves read-only queries against BucketListDB snapshots.

**Routes:** `getledgerentryraw`, `getledgerentry`.

**Threading:** Each worker thread in the server pool gets its own `SearchableSnapshotConstPtr` (both live and hot-archive). Snapshots are refreshed via `BucketSnapshotManager::maybeCopySearchableBucketListSnapshot()` on each query.

### `Maintainer`

Periodic background maintenance that prunes old data from history tables.

**Key methods:**
- `start()` — Schedules periodic maintenance based on `AUTOMATIC_MAINTENANCE_PERIOD`.
- `performMaintenance(count)` — Calculates safe deletion boundary (respects pending checkpoint publications), trims SCP history and ledger header data up to that boundary.

---

## Key Modules and Responsibilities

### Entry Point & CLI (`main.cpp`, `CommandLine.cpp`)

- `main()`: Initializes logging, crypto (libsodium), global state, validates XDR hash identity between C++ and Rust, checks stellar-core major version matches protocol version, delegates to `handleCommandLine()`.
- `handleCommandLine()`: Parses CLI via Clara library. Supports subcommands: `run`, `catchup`, `publish`, `new-db`, `new-hist`, `self-check`, `convert-id`, `dump-xdr`, `print-xdr`, `sign-transaction`, `sec-to-pub`, `gen-seed`, `http-command`, `version`, `merge-bucket-list`, `dump-ledger`, `offline-info`, `report-last-history-checkpoint`, `check-quorum-intersection`, `dump-state-archival-stats`, `calculate-asset-supply`, plus test-only commands (`test`, `fuzz`, `gen-fuzz`, `load-xdr`, `rebuild-ledger-from-buckets`, `apply-load`).

### Application Utilities (`ApplicationUtils.cpp`)

Higher-level functions used by CLI commands:
- `setupApp()` — Creates an Application, validates history config.
- `runApp()` — Starts app, runs the main event loop (`clock.crank()` until io_context stops).
- `selfCheck()` — Four-phase check: async online checks, bucket hash verification, full BL/DB consistency, crypto benchmarking.
- `catchup()` / `publish()` — Orchestrate catchup or history publication.
- `applyBucketsForLCL()` — Rebuilds ledger state from bucket list.
- `mergeBucketList()` — Merges all BL levels into single output bucket.
- `dumpLedger()` — Dumps ledger entries from BucketList with optional filtering, grouping, and aggregation using XDR query engine.
- `dumpStateArchivalStatistics()` — Reports state archival metrics (expired/evicted entries).
- `calculateAssetSupply()` — Computes total asset supply across live and hot-archive BucketLists.
- `dumpWasmBlob()` — Extracts a specific Wasm contract blob by hash.
- `minimalDBForInMemoryMode()` — Constructs minimal SQLite DB path for in-memory/captive core modes.
- `setAuthenticatedLedgerHashPair()` — Sets authenticated hash for catchup starting points.
- `getStellarCoreMajorReleaseVersion()` — Regex extracts major version from version string.

### XDR Utilities (`dumpxdr.cpp`)

- `dumpXdrStream()` — Auto-detects XDR file type (ledger, bucket, transactions, results, meta, SCP, debug-tx-set) by filename regex and streams as JSON.
- `printXdr()` — Decodes single XDR values (auto/typed) from file or stdin, outputs JSON.
- `signtxn()` / `signtxns()` — Signs transaction envelopes with secret keys (interactive password input with terminal echo suppression).
- `priv2pub()` — Converts secret key from stdin to public key.

### Settings Upgrade Utilities (`SettingsUpgradeUtils.cpp`)

Helpers for constructing Soroban settings upgrade transactions:
- `getWasmRestoreTx()` — Builds a restore-footprint TX for a Wasm contract.
- `getUploadTx()` — Builds an upload-contract-wasm TX.
- `getCreateTx()` — Builds a create-contract TX.
- `getInvokeTx()` — Builds an invoke-host-function TX that applies a `ConfigUpgradeSet`.

### Diagnostics (`Diagnostics.cpp`)

- `bucketStats()` — Reads a bucket file, computes per-entry-type counts, byte sizes, averages; optionally aggregates per account. Outputs JSON.

---

## Ownership Relationships

```
Application (abstract interface)
 └── ApplicationImpl (concrete, 1:1 with VirtualClock)
      ├── mConfig (Config, local copy, immutable)
      ├── mNetworkID (Hash, derived from NETWORK_PASSPHRASE)
      ├── mMetrics (unique_ptr<MetricsRegistry>)
      ├── mAppConnector (unique_ptr<AppConnector>)
      │
      ├── IO Contexts & Work Guards:
      │    ├── mWorkerIOContext (asio::io_context, WORKER_THREADS-1)
      │    ├── mEvictionIOContext (unique_ptr<asio::io_context>, 1 thread)
      │    ├── mOverlayIOContext (unique_ptr, conditional on BACKGROUND_OVERLAY_PROCESSING)
      │    ├── mLedgerCloseIOContext (unique_ptr, conditional on parallelLedgerClose())
      │    └── mWork, mEvictionWork, mOverlayWork, mLedgerCloseWork (io_context::work guards)
      │
      ├── Subsystem Managers:
      │    ├── mBucketManager (unique_ptr<BucketManager>)
      │    ├── mDatabase (unique_ptr<Database>)
      │    ├── mPersistentState (unique_ptr<PersistentState>)
      │    ├── mOverlayManager (unique_ptr<OverlayManager>)
      │    ├── mLedgerManager (unique_ptr<LedgerManager>) [protected]
      │    ├── mHerder (unique_ptr<Herder>) [protected]
      │    ├── mLedgerApplyManager (unique_ptr<LedgerApplyManager>)
      │    ├── mHerderPersistence (unique_ptr<HerderPersistence>)
      │    ├── mHistoryArchiveManager (unique_ptr<HistoryArchiveManager>)
      │    ├── mHistoryManager (unique_ptr<HistoryManager>)
      │    ├── mInvariantManager (unique_ptr<InvariantManager>)
      │    ├── mMaintainer (unique_ptr<Maintainer>)
      │    ├── mProcessManager (shared_ptr<ProcessManager>)
      │    ├── mWorkScheduler (shared_ptr<WorkScheduler>)
      │    ├── mBanManager (unique_ptr<BanManager>)
      │    ├── mStatusManager (unique_ptr<StatusManager>)
      │    ├── mLedgerTxnRoot (unique_ptr<AbstractLedgerTxnParent>)
      │    └── mCommandHandler (unique_ptr<CommandHandler>)
      │         └── mQueryServer (unique_ptr<QueryServer>, optional)
      │
      ├── Threads:
      │    ├── mWorkerThreads (vector<unique_ptr<thread>>)
      │    ├── mEvictionThread (unique_ptr<thread>)
      │    ├── mOverlayThread (unique_ptr<thread>, conditional)
      │    └── mLedgerCloseThread (unique_ptr<thread>, conditional)
      │
      └── mThreadTypes (unordered_map<thread::id, ThreadType>)
```

**Construction/Destruction order is critical:** IO contexts first, then managers, then threads. Destruction is reverse: threads joined first, then managers torn down.

---

## Threading Model

### Main Thread
- Runs the `VirtualClock`'s asio::io_context event loop.
- All state-modifying operations and most subsystem interactions happen here.
- Subsystem accessors in `AppConnector` assert `threadIsMain()`.

### Worker Threads (WORKER_THREADS - 1)
- Run `mWorkerIOContext`, low priority.
- Execute self-contained CPU-bound tasks (hashing, signature verification).
- Post results back to main thread via `postOnMainThread()`.

### Eviction Thread (1)
- Runs `mEvictionIOContext`, medium priority.
- Dedicated to BucketList eviction scans.

### Overlay Thread (optional, 1)
- Runs `mOverlayIOContext`, normal priority.
- Enabled by `BACKGROUND_OVERLAY_PROCESSING`.
- Handles overlay network operations (message processing, peer I/O).

### Ledger Close / Apply Thread (optional, 1)
- Runs `mLedgerCloseIOContext`.
- Enabled by `parallelLedgerClose()` (requires both `PARALLEL_LEDGER_APPLY` and `BACKGROUND_OVERLAY_PROCESSING`).
- Offloads ledger application from the main thread.

### Thread Identification
- `mThreadTypes` maps `thread::id` → `ThreadType`. Populated at construction, read-only thereafter.
- `threadIsType(type)` used for runtime assertions about which thread is executing.

---

## Key Control Loops

### Main Event Loop (`runApp()` in ApplicationUtils.cpp)
```
app->start()
asio::io_context::work mainWork(io)
while (!io.stopped()):
    app->getClock().crank()  // dispatches one batch of IO events/timers
```

### Maintenance Loop (`Maintainer`)
- Timer-driven: fires every `AUTOMATIC_MAINTENANCE_PERIOD` (default ~6 min).
- `tick()` → `performMaintenance(count)` → prunes old SCP history and ledger headers → re-schedules.

### Self-Check Loop
- Timer-driven: fires every `AUTOMATIC_SELF_CHECK_PERIOD` (default 3h).
- `scheduleSelfCheck()` → schedules `WorkSequence` (history archive report + checkpoint ledger check).
- Guards against concurrent self-checks via `mRunningSelfCheck` weak_ptr.

### Startup Sequence
1. `ApplicationImpl` constructor: creates io_contexts, spawns worker/eviction/overlay/ledger-close threads, registers signal handlers.
2. `initialize()`: creates all subsystem managers, registers invariants, initializes or upgrades DB.
3. `start()`: loads last known ledger, starts services (Herder, Maintainer, OverlayManager, history publication).
4. `runApp()`: enters main event loop.

### Graceful Shutdown Sequence
1. Signal handler or explicit call → `gracefulStop()`.
2. Sets `mStopping = true`.
3. `idempotentShutdown(true)`:
   - Shuts down ledger close thread first (while subsystems still valid).
   - Shuts down CommandHandler, OverlayManager, WorkScheduler, ProcessManager.
   - BucketManager forgets unreferenced buckets, then shuts down.
   - Herder shuts down.
   - Main IO context shuts down.
   - Joins all threads.
4. After delay: final IO context shutdown via timer.

---

## Key Data Flows

### Application State Derivation
`getState()` derives `Application::State` from:
- `mStarted` flag → `APP_CREATED_STATE` if not started.
- `mStopping` flag → `APP_STOPPING_STATE`.
- `Herder::getState()` → `APP_ACQUIRING_CONSENSUS_STATE` if not tracking.
- `LedgerManager::getState()` → `APP_CONNECTED_STANDBY_STATE`, `APP_CATCHING_UP_STATE`, or `APP_SYNCED_STATE`.

### Cross-Thread Communication
- Work posted via `postOnMainThread()`, `postOnBackgroundThread()`, etc.
- Each post wraps the closure with jitter injection and delay metrics (`LogSlowExecution`).
- `postOnLedgerCloseThread()` additionally calls `getClock().newBackgroundWork()` / `finishedBackgroundWork()` to coordinate with the VirtualClock.

### Network Passphrase Validation (`validateNetworkPassphrase()`)
- On first run: persists passphrase to `PersistentState`.
- On subsequent runs: checks stored passphrase matches config; throws if mismatch.

### CommandHandler Request Flow
1. HTTP server receives request on main thread io_context.
2. `safeRouter()` wraps handler in try/catch.
3. Handler accesses subsystems via `mApp` reference.
4. Response serialized as JSON string.

### QueryServer Request Flow
1. HTTP request arrives on one of `QUERY_THREAD_POOL_SIZE` worker threads.
2. Worker's BucketListDB snapshot is refreshed if stale.
3. Query executes against snapshot (no main-thread contention).
4. Response returned as JSON.

### Configuration Loading
1. `Config::Config()` sets all defaults.
2. `load(filename)` parses TOML via cpptoml.
3. `processConfig()` maps TOML keys to member variables, validates constraints.
4. `adjust()` fixes derived connection settings.
5. Application makes a local copy; config is immutable thereafter.
