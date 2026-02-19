```skill
---
name: "stellar-core whole-system summary"
description: "Good initial context for any broad-scope task working on stellar-core. Covers architecture, subsystems, threading, data flows, ownership, Soroban integration, testing, and design patterns."
---

# stellar-core — Whole-System Technical Summary

## 1. System Overview

stellar-core is the C++ reference implementation of the Stellar network consensus node. It validates transactions, participates in the Stellar Consensus Protocol (SCP), maintains the canonical ledger state, publishes history to archives, and (since protocol v20) hosts the Soroban smart-contract runtime via a Rust FFI bridge.

**Language mix:** ~95% C++17 (core), ~5% Rust (Soroban host, crypto primitives, module cache). Rust is compiled into `librust_stellar_core.a` and linked via [cxx](https://cxx.rs/) FFI.

**Key external dependencies:**
- **ASIO** — async I/O and event loop (standalone, no Boost).
- **libsodium** — Ed25519, SHA-256, Curve25519 ECDH, AEAD.
- **SOCI** — database abstraction (SQLite / PostgreSQL).
- **medida** — metrics (counters, meters, timers, histograms).
- **spdlog** — structured logging with partition-independent levels.
- **cereal** — serialization (JSON for HAS, binary for bucket indexes).
- **xdrpp** — XDR code generator and runtime for all on-wire/on-disk types.
- **wasmi** — WebAssembly interpreter for Soroban Wasm contracts (inside Rust host).
- **Catch2** — test framework.

**Build system:** GNU Autotools (`configure.ac` / `Makefile.am`), with a Cargo workspace for the Rust crate.

**Repository layout (src/):**
| Directory | Purpose |
|-----------|---------|
| `main/` | Application lifecycle, Config, CLI, CommandHandler |
| `ledger/` | LedgerManager, LedgerTxn, InMemorySorobanState |
| `herder/` | SCP driver, tx queues, upgrades |
| `scp/` | Federated Byzantine Agreement implementation |
| `overlay/` | P2P networking, peer management, flood control |
| `transactions/` | Transaction/operation frames, DEX, parallel apply |
| `bucket/` | BucketList (LSM-tree), merges, indexes, eviction |
| `catchup/` | Ledger sync from history archives |
| `history/` | History archive publication, checkpoint construction |
| `historywork/` | Work units for archive I/O (download, upload, verify) |
| `crypto/` | Hashing, signatures, key encoding |
| `database/` | SOCI wrappers, schema versioning |
| `work/` | Cooperative async task framework |
| `simulation/` | Multi-node in-process simulation and load generation |
| `invariant/` | Runtime correctness checks |
| `rust/` | cxx FFI bridge to Soroban host and Rust utilities |
| `util/` | VirtualClock, Scheduler, logging, numerics, data structures |
| `test/` | Test infrastructure, helpers, fuzzing |

---

## 2. Architecture Overview

### Application as Root Object

`Application` (abstract interface) / `ApplicationImpl` (concrete) is the root of the entire object graph. It owns every subsystem manager via `unique_ptr` and orchestrates the startup/shutdown lifecycle.

Owned managers (created in `ApplicationImpl::initialize()`):
- `LedgerManager` — ledger close/apply pipeline
- `LedgerApplyManager` — catchup coordination
- `Herder` / `HerderImpl` — SCP driver, tx queues
- `BucketManager` — BucketList state, merges, snapshots
- `HistoryManager` — checkpoint construction and publication
- `HistoryArchiveManager` — archive configuration
- `OverlayManager` — P2P networking
- `InvariantManager` — runtime invariant checks
- `DatabasePool` — SQL database connections
- `WorkScheduler` — cooperative async task scheduler
- `ProcessManager` — external subprocess management
- `CommandHandler` / `QueryServer` — HTTP admin and query endpoints
- `StatusManager` — status reporting
- `PersistentState` — key-value store in the database

### Event Loop

The main event loop is driven by `VirtualClock::crank()`:

1. Dispatch expired timers (VirtualTimer callbacks).
2. Poll ASIO for I/O completions (network, file handles).
3. Run one action from the `Scheduler` (LAS/FB fair multi-queue scheduler).
4. Transfer items from the thread-safe pending queue (cross-thread posts) into the scheduler.

In `VIRTUAL_TIME` mode (tests/simulation), time advances to the next event when idle, enabling deterministic fast-forward execution.

The `Scheduler` implements Least-Attained-Service scheduling: multiple named queues each track cumulative runtime, and the queue with the lowest total runs next. Under overload (queue latency > 5s window), droppable actions are shed.

### Configuration

`Config` is a value object loaded from TOML. Key parameters include: `PEER_PORT`, `HTTP_PORT`, `DATABASE` (connection string), `BUCKET_DIR_PATH`, `HISTORY` (archive definitions), `QUORUM_SET`, `NETWORK_PASSPHRASE`, `WORKER_THREADS`, `MAX_CONCURRENT_SUBPROCESSES`, and dozens of tuning knobs. `Config::adjust()` normalizes dependent settings.

---

## 3. Core Subsystems

### 3.1 SCP — Stellar Consensus Protocol

Implementation of Federated Byzantine Agreement in `src/scp/`. Operates in two phases:

1. **Nomination** — Proposes candidate values. A node nominates its own values and accepts/confirms values from peers via federated voting (`federatedAccept` / `federatedRatify` over quorum slices). Nomination produces a composite value.

2. **Ballot** — Drives the network to agree on a single value. Uses a two-phase commit: `prepare` → `confirm` → `externalize`. Ballot numbers monotonically increase; a node may abort a ballot and try a higher one. When a ballot is externalized, the slot is decided.

Key classes:
- `SCP` — top-level interface, owns `LocalNode` and slot map.
- `SCPDriver` (abstract) — callback interface implemented by `HerderSCPDriver`.
- `Slot` — per-ledger-sequence state: nomination + ballot protocol state machines.
- `LocalNode` — this node's identity, quorum set, quorum intersection checking.
- `QuorumTracker` / `QuorumIntersectionChecker` — Tarjan SCC-based quorum analysis.

### 3.2 Herder — SCP Driver and Transaction Management

`HerderImpl` is the concrete `SCPDriver` that bridges SCP consensus to ledger management. Key responsibilities:

- **Transaction queuing**: `TransactionQueue` (classic) and `SorobanTransactionQueue` manage pending transactions with per-source-account flood limits, surge pricing, and age-based eviction.
- **Consensus value production**: `TxSetFrame` / `GeneralizedTxSetFrame` constructs valid transaction sets respecting resource limits. `ApplicableTxSetFrame` is the validated, ready-to-apply form.
- **SCP envelope handling**: `PendingEnvelopes` buffers envelopes, fetches missing tx sets/quorum sets via overlay.
- **Upgrade mechanism**: `Upgrades` / `LedgerUpgrade` proposes and applies network parameter changes (protocol version, base reserve, max tx set size, Soroban config).
- **Externalization**: When SCP externalizes a slot, `valueExternalized()` constructs a `LedgerCloseData` and passes it to `LedgerApplyManager::processLedger()`.

### 3.3 Ledger — State Management and Close Pipeline

The ledger subsystem manages the authoritative ledger state and the close/apply pipeline.

**LedgerManager** orchestrates ledger closing:
1. `closeLedger(lcd)` — receives externalized data from Herder.
2. `applyLedger()` — applies the transaction set to produce the next ledger state.
3. Inner apply sequence: `processFeesSeqNums()` → `applyTransactions()` (sequential classic + parallel Soroban) → `applyUpgrades()` → `sealLedgerTxn()` → commit to DB → `advanceLedgerState()` (advance BucketList) → trigger history checkpoint if appropriate.

**LedgerTxn** — Transactional ledger state access layer. `LedgerTxnRoot` wraps the persistent store (DB or BucketListDB). Nested `LedgerTxn` instances form a tree; commit propagates changes upward, rollback discards them. `LedgerTxnEntry` / `ConstLedgerTxnEntry` provide handle-based access with invalidation semantics.

**InMemorySorobanState** — In-memory cache of all Soroban ledger entries (ContractData, ContractCode) for fast lookup during Soroban tx execution, avoiding DB round-trips.

**SorobanNetworkConfig** — Cached Soroban network configuration (CPU/memory limits, fee params, state archival settings). Loaded from ledger entries at startup and updated on protocol upgrades.

### 3.4 BucketList — Canonical State Store

An LSM-tree data structure providing the canonical serialization of all ledger state. Two independent bucket lists:

- **LiveBucketList** — active ledger entries (accounts, trustlines, offers, contract data, etc.). 11 levels with geometrically increasing sizes (level L stores entries modified within the last `2^(2*(L+1))` ledgers).
- **HotArchiveBucketList** — recently archived/deleted Soroban entries. Same level structure.

Key components:
- **BucketManager** — owns both bucket lists, manages the bucket directory, merge scheduling, garbage collection, temp/live bucket tracking, and the `BucketSnapshotManager`.
- **FutureBucket** — represents an in-progress async merge, resolved on a worker thread. Each level's `snap` slot may have an active `FutureBucket`.
- **BucketIndex** — per-bucket hash-indexed lookup structure (range index + individual key index) for BucketListDB point queries. Supports `BinaryFuseFilter` for fast negative membership tests.
- **BucketSnapshotManager** — provides thread-safe read-only snapshots of the bucket list for queries (main thread, background eviction thread, overlay thread).
- **Eviction** — background scanning of the LiveBucketList to evict expired Soroban entries and produce eviction iterators for the HotArchiveBucketList.

### 3.5 Overlay — P2P Networking

Manages TCP connections to peers and message flooding.

- **OverlayManager** — owns the `PeerManager` (peer database), `FloodGate`, and `SurveyManager`. Optionally runs network I/O on a dedicated overlay thread.
- **Peer** / **TCPPeer** — per-connection state: HMAC-authenticated messaging over TCP, read/write queues with flow control.
- **Authentication**: ECDH key exchange (Curve25519) → shared key → HMAC-SHA256 MAC on every message. `PeerAuth` manages the handshake.
- **Flow control**: Capacity-based system where a peer advertises available capacity for flood messages and reading messages. `FlowControl` tracks per-peer capacity and throttles sends.
- **Transaction flooding**: Pull-mode (v23+): nodes send `StellarMessage::TX_ADVERT` containing tx hashes; receivers send `TX_DEMAND` for missing hashes; sender responds with full tx. The `FloodGate` manages demand tracking, deduplication, and retry.
- **SurveyManager** — network topology surveys for monitoring.

### 3.6 History — Archive Publication and Catchup

**Publication pipeline** (every 64 ledgers):
1. `CheckpointBuilder` incrementally writes ledger headers, transactions, and results to XDR streams during ledger close.
2. At checkpoint boundary: `HistoryManager::maybeQueueHistoryCheckpoint()` snapshots the BucketList into a `HistoryArchiveState` (HAS) and queues it.
3. After commit: dirty files are renamed to final names (atomic durability).
4. `takeSnapshotAndPublish()` schedules: `ResolveSnapshotWork` → `WriteSnapshotWork` (gzip) → `PutSnapshotFilesWork` (upload via shell commands).

**Catchup** (when a node falls behind):
1. `LedgerApplyManager` detects gap, buffers incoming ledgers in `mSyncingLedgers`, triggers online catchup.
2. `CatchupWork` orchestrates: fetch remote HAS → compute `CatchupRange` → download and verify ledger chain (hash-chain verification from trusted SCP hash backward) → download and apply buckets → download and replay transactions → drain buffered ledgers.
3. Key work classes: `VerifyLedgerChainWork` (backward hash-chain verification), `ApplyBucketsWork` (bucket-to-DB restore), `DownloadApplyTxsWork` (per-checkpoint download+apply with ConditionalWork sequencing), `ApplyBufferedLedgersWork`.

### 3.7 Database

SOCI-based SQL abstraction supporting SQLite and PostgreSQL.

- **DatabasePool** — pool of `Database` sessions. One primary session for main-thread use, additional sessions from the pool for worker threads.
- **Dual-database architecture** (SQLite only): main database for ledger state, misc database for historical data (SCP messages, peer records). Reduces I/O contention.
- **Schema versioning**: `PersistentState` stores `databaseschema` version; `upgradeToCurrentSchema()` runs DDL migrations on startup.
- **BucketListDB mode** (v23+): replaces SQL-based ledger entry storage with BucketList point lookups, using SQL only for offers (order book) and other specialized queries.

### 3.8 Transactions — Processing Pipeline

Complete transaction processing from parsing to application.

**Transaction frame hierarchy:**
```
TransactionFrameBase (abstract)
├── TransactionFrame         (regular V0/V1 transactions)
└── FeeBumpTransactionFrame  (fee bump wrapping inner TransactionFrame)
```

**Operation frame hierarchy** — 23+ concrete operation types:
- Classic: `CreateAccountOpFrame`, `PaymentOpFrame`, `PathPaymentStrictReceiveOpFrame`, `PathPaymentStrictSendOpFrame`, `ManageSellOfferOpFrame`, `ManageBuyOfferOpFrame`, `CreatePassiveSellOfferOpFrame`, `SetOptionsOpFrame`, `ChangeTrustOpFrame`, `AllowTrustOpFrame`, `SetTrustLineFlagsOpFrame`, `MergeOpFrame`, `InflationOpFrame`, `ManageDataOpFrame`, `BumpSequenceOpFrame`, plus claimable balance, sponsorship, clawback, and liquidity pool operations.
- Soroban: `InvokeHostFunctionOpFrame`, `ExtendFootprintTTLOpFrame`, `RestoreFootprintOpFrame`.

**Transaction lifecycle:**
1. **Deserialization**: `TransactionFrameBase::makeTransactionFromWire()` constructs frame hierarchy.
2. **Validation** (`checkValid`): XDR depth check → fee validation → signatures → sequence number → per-op validation.
3. **Fee processing** (`processFeeSeqNum`): deducts fee from source account, consumes sequence number.
4. **Application** (`apply`): per-op `doApply()` in nested LedgerTxn with commit/rollback.
5. **Post-apply**: Soroban fee refunds, result finalization.

**DEX exchange**: `exchangeV10()` computes exact crossing amounts with rounding rules. `convertWithOffersAndPools()` iterates the order book and liquidity pools.

**Parallel apply** (Soroban, v23+): Transactions grouped into `ApplyStage`s of non-overlapping `Cluster`s. Clusters run on separate threads with scoped ledger state (`GlobalParallelApplyLedgerState` → `ThreadParallelApplyLedgerState` → `TxParallelApplyLedgerState`). Compile-time scope tags prevent cross-scope reads.

### 3.9 Crypto

- **Hashing**: SHA-256 (libsodium, primary), BLAKE2 (bucket hashing for BinaryFuseFilter).
- **Signatures**: Ed25519 via libsodium (C++) and ed25519-dalek (Rust). Signature verification cache (`VerifySigCache`) avoids redundant verification during flooding.
- **Key exchange**: Curve25519 ECDH for peer authentication.
- **Key encoding**: StrKey format (base32 with version byte and CRC16 checksum) for public keys, secret keys, pre-auth tx hashes, hash-x, signed payloads, and contract IDs.
- **HMAC**: SHA-256 HMAC for authenticated peer messaging.
- **Soroban crypto** (Rust host): Ed25519, ECDSA (secp256k1/secp256r1), SHA-256, Keccak-256, BLS12-381, BN254, Poseidon hashes — all budget-metered.

### 3.10 Work — Cooperative Async Framework

The work subsystem provides a cooperative, single-threaded, FSM-based task framework for long-running operations (catchup, publication, downloads).

- **BasicWork** — base FSM with states: `PENDING`, `RUNNING`, `WAITING`, `RETRYING`, `SUCCESS`, `FAILURE`, `ABORTED`. Supports exponential-backoff retries.
- **Work** — extends BasicWork with child management (hierarchical work trees, round-robin child scheduling).
- **WorkScheduler** — top-level scheduler owned by Application; posts cranks to the ASIO event loop.
- **WorkSequence** — strict sequential execution of a vector of work items.
- **BatchWork** — parallel batched execution throttled by `MAX_CONCURRENT_SUBPROCESSES`.
- **ConditionalWork** — gates execution on a monotonic condition (used extensively in catchup for sequencing dependencies).

### 3.11 Process Management

`ProcessManager` spawns external subprocesses (gzip, gunzip, curl, archive shell commands) asynchronously. Uses `posix_spawnp` on POSIX. Each `ProcessExitEvent` carries an exit code and is delivered via ASIO timer polling. Throttled by `MAX_CONCURRENT_SUBPROCESSES`.

### 3.12 Invariants

Runtime correctness checking framework with 12+ invariant implementations. Enabled in debug/test builds. Each invariant implements `checkOnOperationApply()` and/or `checkAfterAssumeState()`.

Key invariants:
- `ConservationOfLumens` — total XLM is conserved across ledger closes.
- `LedgerEntryIsValid` — structural validity of all modified entries.
- `AccountSubEntriesCountIsValid` — sub-entry bookkeeping consistency.
- `SponsorshipCountIsValid` — sponsorship reserve accounting.
- `BucketListIsConsistentWithDatabase` — BucketList matches SQL state.
- `LiabilitiesMatchOffers` — offer liabilities match reserve tracking.
- `MinimumAccountBalance` — accounts maintain minimum reserve.
- `SorobanImpliedLedgerBoundsAreValid` — Soroban TTL/archival bounds.

### 3.13 Simulation

In-process multi-node simulation for integration testing and benchmarking.

- `Simulation` — creates multiple `Application` instances connected via `LoopbackPeer`. Supports various topologies (1-node, 2-node, core+watcher, hierarchical quorums).
- `LoadGenerator` — generates synthetic transactions (create accounts, payments, Soroban uploads/invocations, DEX operations) for stress testing.
- `ApplyLoad` — benchmarks parallel Soroban transaction application.
- `TxGenerator` — lower-level tx construction for load generation with account state tracking.

---

## 4. Threading Model

stellar-core uses a primarily single-threaded architecture with selective parallelism:

### Main Thread
All state mutations and coordination happen on the main thread. The `VirtualClock::crank()` loop drives timers, I/O, and scheduled actions. Cross-thread work is posted via `VirtualClock::postAction()` (mutex-protected pending queue → scheduler). `threadIsMain()` assertions guard critical sections.

### Worker Thread Pool
CPU-bound tasks run on a pool of `WORKER_THREADS` threads (default from config). Uses `asio::io_context` as a thread-safe work queue. Tasks include:
- BucketList merge operations (`FutureBucket` resolution).
- Bucket indexing (`IndexBucketsWork`).
- Signature verification (batch verification during flooding).
- Background hash computation.

Results are posted back to the main thread via `postOnMainThread()`.

### Eviction Thread
A dedicated thread scans the LiveBucketList for expired Soroban entries. Reads from `BucketSnapshotManager` snapshots (thread-safe). Produces eviction iterators consumed by the main thread during ledger close.

### Overlay Thread (Optional)
When `BACKGROUND_OVERLAY_PROCESSING` is enabled, TCP I/O and message processing run on a separate thread. Uses `OverlayAppConnector` to safely post results to the main thread.

### Ledger Close Thread (Optional)
When parallel ledger close is enabled, `applyLedger()` runs on a dedicated thread while the main thread continues with SCP. Coordination via `std::promise`/`std::future` and `postOnMainThread()`.

### Soroban Parallel Apply Threads (v23+)
During ledger close, Soroban transactions in non-overlapping clusters run on separate threads. The `GlobalParallelApplyLedgerState` → `ThreadParallelApplyLedgerState` → `TxParallelApplyLedgerState` hierarchy enforces ownership discipline with compile-time scope tags.

### Thread Safety Annotations
Clang thread-safety annotations (`GUARDED_BY`, `REQUIRES`, `ACQUIRE`, `RELEASE`) are used extensively. Custom mutex wrappers (`Mutex`, `SharedMutex`, `RecursiveMutex`) carry annotations. `releaseAssert(threadIsMain())` guards main-thread-only code.

---

## 5. Key Data Flows

### 5.1 Transaction Lifecycle: Submission to Finalization

```
External submission (HTTP/overlay)
  │
  ▼
OverlayManager::recvFloodedMsg() ─── pull-mode: TX_ADVERT → TX_DEMAND → TX
  │
  ▼
Herder::recvTransaction()
  ├─ TransactionFrame::checkValid() (validation, signature check)
  ├─ TransactionQueue::tryAdd() (per-source flood limits, surge pricing)
  └─ FloodGate::addRecord() → broadcast TX_ADVERT to peers
  │
  ▼
SCP Nomination Phase
  ├─ Herder::triggerNextLedger() → builds TxSetFrame from queued txs
  └─ SCPDriver::nominate(composite value with tx set hash)
  │
  ▼
SCP Ballot Phase
  ├─ prepare → confirm → externalize
  └─ valueExternalized() → builds LedgerCloseData
  │
  ▼
LedgerApplyManager::processLedger(lcd)
  ├─ If sequential → tryApplySyncingLedgers()
  └─ If behind → buffer → startOnlineCatchup()
  │
  ▼
LedgerManager::closeLedger(lcd)
  │
  ├─ processFeesSeqNums() ── deduct fees, consume seq nums
  ├─ applyTransactions()
  │   ├─ Classic: sequential apply in nested LedgerTxn
  │   └─ Soroban (v23+): parallel apply in stages/clusters
  ├─ applyUpgrades() ── network parameter changes
  ├─ sealLedgerTxn() ── finalize ledger header hash
  ├─ Commit to DB + advance BucketList
  ├─ appendLedgerHeader() + appendTransactionSet() → checkpoint streams
  └─ maybeQueueHistoryCheckpoint() → publish pipeline
```

### 5.2 BucketList Merge Cycle

```
ledgerClose → BucketList::addBatch(newEntries)
  │
  ├─ Level 0: always merge new entries into curr bucket
  ├─ Level L (L>0): on level-L spill boundary:
  │   ├─ snap(L) = old curr(L)
  │   ├─ curr(L) = merge(old snap(L), new entries from level L-1)
  │   └─ FutureBucket: async merge on worker thread
  │
  ├─ FutureBucket::startMerge() → post to worker pool
  ├─ Worker: BucketList::merge() → produce new bucket file
  └─ Main thread: resolve future before next level spill
```

### 5.3 Catchup Flow

```
Gap detected (LCL < consensus ledger)
  │
  ▼
LedgerApplyManager::startOnlineCatchup()
  │
  ▼
CatchupWork (on WorkScheduler)
  ├─ 1. Fetch remote HistoryArchiveState
  ├─ 2. Compute CatchupRange (bucket-apply? replay range?)
  ├─ 3. Download + verify ledger chain (backward hash verification)
  ├─ 4a. Download + apply buckets (if needed)
  │      └─ IndexBucketsWork → BucketApplicator → AssumeStateWork
  ├─ 4b. Download + replay transactions (per checkpoint)
  │      └─ GetAndUnzipRemoteFileWork → ApplyCheckpointWork → ApplyLedgerWork
  └─ 5. ApplyBufferedLedgersWork → drain mSyncingLedgers
```

### 5.4 History Publication Pipeline

```
Ledger close (checkpoint boundary)
  │
  ▼
CheckpointBuilder: finalize .dirty → final rename
  │
  ▼
HistoryManager::publishQueuedHistory()
  │
  ▼
StateSnapshot created
  │
  ▼
WorkSequence:
  ├─ ResolveSnapshotWork (resolve FutureBucket merges)
  ├─ WriteSnapshotWork (write SCP msgs, gzip all files)
  └─ PutSnapshotFilesWork (upload to all writable archives)
```

### 5.5 Soroban Contract Invocation

```
InvokeHostFunctionOpFrame::doApply() / doParallelApply()
  │
  ▼
Rust FFI bridge (cxx) → rust_bridge::invoke_host_function()
  │
  ▼
Host::with_frame(Frame::HostFunction)
  ├─ Resolve contract instance from storage
  ├─ Load Wasm from ModuleCache or storage
  ├─ Instantiate Vm (wasmi)
  └─ Host::with_frame(Frame::ContractVM)
       ├─ Call exported function
       ├─ Wasm instructions consume wasmi fuel (derived from CPU budget)
       ├─ Host function calls: fuel→budget, relative→absolute handles,
       │   dispatch VmCallerEnv method, absolute→relative, budget→fuel
       ├─ Sub-contract calls: recursive with_frame with rollback points
       └─ On return: pop frame, on error: rollback storage+events
  │
  ▼
Host::try_finish() → extract (Storage, Events)
  │
  ▼
Back through FFI → C++ processes ledger changes, fee refunds
```

---

## 6. Ownership Hierarchy

```
Application (ApplicationImpl)
  │
  ├── Config (value)
  ├── VirtualClock& (reference, clock owns ASIO io_context + Scheduler)
  ├── PersistentState (unique_ptr)
  │
  ├── DatabasePool (unique_ptr)
  │     ├── Database (primary session)
  │     └── Database[] (pool sessions for worker threads)
  │
  ├── LedgerManager (unique_ptr)
  │     ├── LedgerTxnRoot → Database or BucketListDB backend
  │     ├── InMemorySorobanState (unique_ptr, Soroban entry cache)
  │     └── SorobanNetworkConfig (cached network config)
  │
  ├── BucketManager (unique_ptr)
  │     ├── LiveBucketList (unique_ptr)
  │     │     └── BucketListLevel[] →  Bucket (shared_ptr), FutureBucket
  │     ├── HotArchiveBucketList (unique_ptr)
  │     ├── BucketSnapshotManager (unique_ptr, thread-safe snapshots)
  │     ├── TmpDirManager (unique_ptr, temp bucket files)
  │     └── Bucket file directory (on disk)
  │
  ├── HerderImpl (unique_ptr)
  │     ├── SCP (unique_ptr) → LocalNode, Slot map
  │     ├── HerderSCPDriver → PendingEnvelopes, SCPMetrics
  │     ├── TransactionQueue (classic tx queue)
  │     ├── SorobanTransactionQueue
  │     ├── Upgrades (upgrade tracking)
  │     └── SorobanModuleCache* (optional, shared_ptr)
  │
  ├── OverlayManager (unique_ptr)
  │     ├── PeerManager → peer database
  │     ├── FloodGate → tx/SCP flood tracking
  │     ├── Peer[] (shared_ptr per connection)
  │     │     ├── TCPPeer → ASIO socket, read/write queues
  │     │     ├── PeerAuth → ECDH, HMAC state
  │     │     └── FlowControl → capacity tracking
  │     └── SurveyManager
  │
  ├── LedgerApplyManager (unique_ptr)
  │     ├── CatchupWork (shared_ptr, active during catchup)
  │     └── mSyncingLedgers (buffered ledgers map)
  │
  ├── HistoryManager (unique_ptr)
  │     ├── CheckpointBuilder (value, XDR output streams)
  │     └── BasicWork mPublishWork (shared_ptr, current publish)
  │
  ├── HistoryArchiveManager (value)
  │     └── HistoryArchive[] (shared_ptr per configured archive)
  │
  ├── WorkScheduler (shared_ptr)
  │     └── BasicWork children (shared_ptr tree)
  │
  ├── ProcessManager (unique_ptr)
  │     └── ProcessExitEvent[] (pending subprocesses)
  │
  ├── InvariantManager (unique_ptr)
  │     └── Invariant[] (unique_ptr per invariant)
  │
  ├── CommandHandler (unique_ptr, HTTP admin)
  ├── QueryServer (unique_ptr, read-only query endpoint)
  └── StatusManager (unique_ptr)
```

---

## 7. Cross-Cutting Concerns

### 7.1 XDR Type System

All on-wire and on-disk data structures are defined in `.x` XDR files under `src/protocol-curr/xdr/`. The `xdrpp` code generator produces C++ types with serialization, comparison, and visitor support. Key type families:

- **Ledger entries**: `AccountEntry`, `TrustLineEntry`, `OfferEntry`, `DataEntry`, `ClaimableBalanceEntry`, `LiquidityPoolEntry`, `ContractDataEntry`, `ContractCodeEntry`, `ConfigSettingEntry`, `TTLEntry`
- **Transactions**: `TransactionEnvelope` (V0/V1/FeeBump), `TransactionResult`, `TransactionMeta`
- **SCP**: `SCPEnvelope`, `SCPStatement`, `SCPQuorumSet`, `SCPBallot`
- **Overlay**: `StellarMessage`, `Hello`, `Auth`, `PeerAddress`
- **Soroban**: `SorobanResources`, `SorobanTransactionData`, `InvokeContractArgs`, `SCVal`, `SCAddress`
- **BucketList**: `BucketEntry`, `HotArchiveBucketEntry`, `BucketMetadata`

### 7.2 Protocol Version Gating

Extensive version-gated code paths across all subsystems. The `ProtocolVersion` enum (`V_0` through `V_26`) with comparison helpers (`protocolVersionIsBefore`, `protocolVersionStartsFrom`) provides safe version checks. Major gates:

| Version | Feature |
|---------|---------|
| v10 | Sequence number processing moved to apply time |
| v13 | Envelope V0 deprecated, one-time signer changes |
| v18 | BinaryFuseFilter bucket indexes |
| v19 | PreconditionsV2, extra signers |
| v20 | Soroban smart contracts |
| v21 | Hot archive bucket list |
| v23 | Parallel Soroban apply, fee bump inner fee relaxation, BucketListDB |
| v25 | Soroban memo/muxed restrictions |

### 7.3 Metrics and Observability

`medida::MetricsRegistry` + `SimpleTimer` for lightweight performance tracking. Metrics exported via `/metrics` HTTP endpoint. Key metric categories: ledger close timing, SCP round duration, overlay message counts, bucket merge timing, catchup progress, tx queue sizes.

`LogSlowExecution` — RAII guard that logs warnings when a scope exceeds a time threshold.

`StatusManager` — aggregates status by category for the `/info` endpoint.

### 7.4 Numeric Safety

All financial calculations use safe arithmetic from `numeric.h`:
- `bigDivide(A, B, C, rounding)` — computes `A*B/C` via 128-bit intermediate.
- `saturatingMultiply`/`saturatingAdd` — cap at max instead of overflowing.
- `Rounding::ROUND_DOWN`/`ROUND_UP` — explicit rounding direction for all division.
- 128-bit helpers for huge intermediate values.

### 7.5 Logging

Partitioned spdlog loggers (~15 partitions: Bucket, Herder, Ledger, Overlay, Tx, SCP, etc.) with per-partition log levels. `CLOG_*` macros with compile-time format string checking. Rust-side logging integrated at `init()`. File rotation support.

### 7.6 Error Handling Patterns

- `releaseAssert(e)` — never compiled out, prints backtrace and aborts.
- `releaseAssertOrThrow(e)` — throws `runtime_error` instead of aborting (used in test contexts).
- Transaction failures: typed exceptions (`ex_PAYMENT_UNDERFUNDED`, etc.) for test verification.
- Soroban: `HostError` wraps error code + optional debug info; `ScErrorType::Contract` errors are recoverable via `try_call`, all others propagate.

---

## 8. Soroban Integration

### 8.1 Architecture

Soroban executes smart contracts inside a Rust-based runtime (`soroban-env-host`) accessed via x FFI bridge from C++.

**Rust crate**: `rust/src/` compiled to `librust_stellar_core.a`. The cxx bridge (`lib.rs`) defines shared types and extern functions callable from C++.

**Key bridge functions** (C++ → Rust):
- `invoke_host_function()` — execute InvokeHostFunction/ExtendTTL/RestoreFootprint
- `compute_transaction_resource_fee()` — fee computation
- `preflight_host_function()` — simulation/preflight for RPC
- `init_logging()` / `check_lockfile_content()` — utilities

**Key bridge types**: `CxxLedgerInfo`, `CxxLedgerEntry`, `CxxBuf`, `InvokeHostFunctionOutput`, `CxxTransactionResources`

### 8.2 Host Runtime

The `Host` (Rc<HostImpl>) is the Soroban execution environment:

- **Val** — universal 64-bit tagged value type crossing the host-guest boundary. Small values (numbers ≤56 bits, symbols ≤9 chars) are packed inline; larger values use host object handles.
- **Object system** — `Vec<HostObject>` indexed by handles. Object handles are absolute (host-side, odd low bit) or relative (per-frame, even low bit) for Wasm isolation.
- **Storage** — `FootprintMode::Recording` (preflight) or `Enforcing` (production). `StorageMap` (metered ordered map) holds entries.
- **Budget** — `Rc<RefCell<BudgetImpl>>` tracking CPU instructions and memory bytes. Per-cost-type linear models. Shadow mode for diagnostic work.
- **Authorization** — `AuthorizationManager` validates `SorobanAuthorizationEntry` trees against invocation patterns. Supports invoker-contract auth, Stellar account auth, and custom account contracts.
- **Events** — `InternalEventsBuffer` for contract/system/diagnostic events with frame-level rollback.
- **Wasm VM** — `wasmi::Instance` per contract call. `ParsedModule` caches validated modules. Fuel-based metering bridges to Budget.

### 8.3 Multi-Protocol Host Dispatch

Different protocol versions use different Soroban host versions. The Rust bridge dispatches to the appropriate host implementation:
- p21-p25 hosts handle older protocol semantics.
- p26 host (`soroban-env-host`) is the current version.
- `SorobanModuleCache` pre-parses and caches Wasm modules per protocol version.

### 8.4 Built-in Contracts

- **Stellar Asset Contract (SAC)** — wraps classic Stellar assets as Soroban contracts. Provides `transfer`, `mint`, `burn`, `allowance`, `balance`, `set_admin`, etc. Bridges classic trustline/account state with Soroban contract data entries.
- **Account Contract** — implements `__check_auth` for classic Stellar multisig accounts used as Soroban addresses.

### 8.5 Parallel Soroban Apply (v23+)

Soroban transactions declare read/write footprints upfront. The `ApplyStage` / `Cluster` structure groups non-overlapping transactions for parallel execution:

1. `GlobalParallelApplyLedgerState` collects all modified entries and sets up snapshots.
2. Per stage: clusters distributed to threads, each thread gets `ThreadParallelApplyLedgerState`.
3. Within a cluster: txs applied sequentially, each in `TxParallelApplyLedgerState`.
4. Successful changes committed thread→global; all stages merged into main LedgerTxn.

---

## 9. Testing Infrastructure

### 9.1 Framework

Tests use **Catch2** with a custom `SimpleTestReporter`. Test entry point: `runTest()` configures the session, seeds PRNGs, and runs all test cases.

`getTestConfig(instanceNumber, dbMode)` returns lazily-created cached `Config` objects with test defaults: `RUN_STANDALONE=true`, `FORCE_SCP=true`, `MANUAL_CLOSE=true`, single-node quorum, in-memory SQLite.

`TestApplication` subclasses `ApplicationImpl` with a `TestInvariantManager` that throws on invariant failure instead of aborting.

### 9.2 Protocol Version Testing

- `for_all_versions(app, f)` — run a test function for every protocol version.
- `for_versions(from, to, app, f)` — run for a version range.
- `for_versions_from(from, app, f)` / `for_versions_to(to, app, f)` — run from/to a version.
- `TEST_CASE_VERSIONS` macro — declares a test iterating over CLI-specified versions.

### 9.3 Test Utilities

**TestAccount** — high-level account wrapper with methods for all Stellar operations (`create`, `pay`, `changeTrust`, `manageOffer`, etc.) that apply transactions and assert success.

**TxTests namespace** — extensive helpers:
- `applyTx()` / `applyCheck()` — apply transactions with validation and result checking.
- `closeLedger()` / `closeLedgerOn()` — close ledgers with specific transactions, dates, upgrades.
- Operation builders: `payment()`, `createAccount()`, `changeTrust()`, `manageOffer()`, etc.
- `sorobanTransactionFrameFromOps()` — construct Soroban transactions with resources.
- `feeBump()` — construct fee bump transactions.
- Upgrade helpers: `executeUpgrade()`, `modifySorobanNetworkConfig()`.

**TestMarket** — tracks DEX offer state for verification. `requireChanges(changes, f)` executes a function and verifies offer state transitions match expectations.

**TestExceptions** — typed exception hierarchy (`ex_PAYMENT_UNDERFUNDED`, `ex_CREATE_ACCOUNT_MALFORMED`, etc.) for error verification via `REQUIRE_THROWS_AS`.

### 9.4 Transaction Metadata Recording

`recordOrCheckGlobalTestTxMetadata()` records or verifies SIPHash of normalized transaction metadata against persistent baselines. Ensures deterministic transaction processing across refactors.

### 9.5 Fuzzing

- **TransactionFuzzer** — sets up a minimal ledger state, reads fuzzed XDR operations, builds and applies transactions.
- **OverlayFuzzer** — creates a 2-node simulation, injects fuzzed `StellarMessage` into a peer connection.
- AFL persistent mode support via `__AFL_LOOP`.

### 9.6 Simulation Testing

`Simulation` creates multi-node topologies with `LoopbackPeer` connections. `LoadGenerator` produces synthetic workloads. Tests cover consensus convergence, catchup, upgrade propagation, and performance under load.

---

## 10. Key Design Patterns

### 10.1 Cooperative Single-Threaded Core

The primary design philosophy: all state mutations happen on the main thread, driven by the event loop. Long-running operations are broken into small "cranks" via the Work FSM framework. This eliminates most synchronization complexity while allowing I/O concurrency through ASIO and selective background work.

### 10.2 Work FSM Pattern

All async operations (catchup, publication, downloads) use the `BasicWork` → `Work` hierarchy. State machine transitions (PENDING → RUNNING → WAITING/RETRYING → SUCCESS/FAILURE/ABORTED) with retry logic, hierarchical composition, and notification propagation. `ConditionalWork` gates execution on monotonic conditions; `WorkSequence` enforces ordering; `BatchWork` provides throttled parallelism.

### 10.3 Transactional State Access (LedgerTxn)

All ledger state modifications go through `LedgerTxn`, which provides:
- Nested transactions with commit-up / rollback semantics.
- Handle-based entry access with invalidation on parent modification.
- Deferred commit to persistent store (SQL or BucketList).
- Automatic change tracking for transaction metadata.

### 10.4 Snapshot Isolation

Thread-safe reads use snapshot isolation:
- `BucketSnapshotManager` provides immutable snapshots of the BucketList for background threads.
- `InMemorySorobanState` provides point-in-time Soroban entry state.
- `GlobalParallelApplyLedgerState` distributes entry ownership across threads with merge-back after completion.

### 10.5 XDR as Source of Truth

All data structures crossing persistence boundaries are defined in XDR. The `xdrpp` generator produces type-safe C++ with serialization, comparison, and visitor support. This ensures wire-compatible serialization across implementations and versions.

### 10.6 Protocol Version Gating

Every behavior change is gated by protocol version checks. The `ProtocolVersion` enum and comparison functions (`protocolVersionIsBefore`, `protocolVersionStartsFrom`) provide a uniform pattern. Tests use `for_all_versions` / `for_versions` to verify behavior across versions.

### 10.7 Hierarchical Ownership

Clear ownership hierarchy rooted at `Application`. Subsystem managers are `unique_ptr`-owned. Shared state uses `shared_ptr` with documented lifetime contracts. Background work captures `weak_ptr` to avoid preventing destruction.

### 10.8 Scope-Tagged Parallelism

Parallel Soroban apply uses compile-time scope tags (`GlobalParApply`, `ThreadParApply`, `TxParApply`) on the `LedgerEntryScope` template to prevent accidental cross-scope access. This pattern enforces thread-safety at the type level rather than relying on runtime checks alone.

### 10.9 Pull-Mode Flooding

Transaction dissemination uses a bandwidth-efficient pull model (v23+): advertise hashes → demand missing → deliver. This reduces redundant transmission in well-connected networks and enables flow control via capacity-based throttling.

### 10.10 Checkpoint-Based History

History is organized in 64-ledger checkpoints with ACID-transactional construction (write-as-dirty, rename-on-commit). Publication is asynchronous via the Work framework. Catchup uses backward hash-chain verification from a trusted SCP consensus hash, providing cryptographic continuity guarantees.
```
