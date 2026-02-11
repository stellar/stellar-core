---
name: subsystem-summary-of-simulation
description: "read this skill for a token-efficient summary of the simulation subsystem"
---

# Simulation Subsystem — Technical Summary

## Overview

The simulation subsystem provides infrastructure for creating multi-node network simulations and generating synthetic transaction load for testing and benchmarking stellar-core. It has four major components: (1) `Simulation` — orchestrates multiple `Application` instances as virtual or TCP-connected nodes, (2) `Topologies` — factory functions producing pre-configured network shapes, (3) `LoadGenerator` — drives sustained transaction submission at configurable rates, and (4) `ApplyLoad` — a closed-loop benchmarking harness that bypasses the overlay to measure raw ledger-close performance. A shared `TxGenerator` class constructs all transaction types (classic payments, Soroban uploads, invocations, SAC transfers, upgrades).

## Key Files

- **Simulation.h / Simulation.cpp** — `Simulation` class; manages nodes, clocks, loopback/TCP connections, and event-loop cranking.
- **Topologies.h / Topologies.cpp** — Static factory methods producing `Simulation` instances with specific network topologies.
- **LoadGenerator.h / LoadGenerator.cpp** — `LoadGenerator` class; timer-driven load submission loop, account management, metrics, completion tracking.
- **TxGenerator.h / TxGenerator.cpp** — `TxGenerator` class; creates all transaction types (payments, Soroban upload/invoke/upgrade, SAC, batch transfer).
- **ApplyLoad.h / ApplyLoad.cpp** — `ApplyLoad` class; offline benchmarking: sets up contracts, populates bucket list, runs ledger-close iterations, binary-searches for maximum throughput.

---

## Key Classes and Data Structures

### `Simulation`

Orchestrates a multi-node stellar-core network within a single process. Used extensively in integration and consensus tests.

**Enums:**
- `Mode` — `OVER_LOOPBACK` (in-process, virtual clocks) or `OVER_TCP` (real sockets, real clocks).

**Type aliases:**
- `ConfigGen` — `std::function<Config(int)>`, generates per-node configs.
- `QuorumSetAdjuster` — `std::function<SCPQuorumSet(SCPQuorumSet const&)>`, post-processes quorum sets.
- `QuorumSetSpec` — `std::variant<SCPQuorumSet, std::vector<ValidatorEntry>>`, allows explicit or auto-generated quorum sets.

**Key members:**
- `mNodes` (`std::map<NodeID, Node>`) — Maps node public keys to `Node` structs. Each `Node` owns a `VirtualClock` and an `Application::pointer`.
- `mPendingConnections` (`std::vector<std::pair<NodeID, NodeID>>`) — Connections queued before `startAllNodes()`.
- `mLoopbackConnections` (`std::vector<std::shared_ptr<LoopbackPeerConnection>>`) — Active in-process connections (loopback mode only).
- `mClock` (`VirtualClock`) — The simulation-level master clock.
- `mIdleApp` (`Application::pointer`) — A "background" application used for the master clock's IO context and timers.
- `mPeerMap` (`std::unordered_map<unsigned short, std::weak_ptr<Application>>`) — Maps `PEER_PORT` to application; enables `LoopbackOverlayManager` to resolve connection targets.
- `mConfigGen` — Optional config generator; defaults to `getTestConfig()`.
- `mQuorumSetAdjuster` — Optional quorum-set postprocessor.
- `mSetupForSorobanUpgrade` (`bool`) — Flag indicating Soroban upgrade readiness.

**Key functions:**
- `addNode(SecretKey, QuorumSetSpec, Config*, bool)` — Creates a new `Application` on its own `VirtualClock`, registers it in `mNodes` and `mPeerMap`. Supports both explicit `SCPQuorumSet` and auto-generated quorum from `ValidatorEntry` vectors.
- `addPendingConnection(NodeID, NodeID)` — Queues a connection to be established when `startAllNodes()` runs.
- `fullyConnectAllPending()` — Adds pending connections between all pairs of nodes (mesh).
- `startAllNodes()` — Calls `app->start()` on each node, then establishes all pending connections.
- `stopAllNodes()` — Gracefully stops all nodes and cranks until clocks stop.
- `removeNode(NodeID)` — Gracefully stops a node, drops all its connections, removes from maps.
- `crankNode(NodeID, time_point)` — Drives one node's clock forward by up to one `quantum` (100 ms) in virtual mode, or just catches up in real-time mode. Also updates the node's survey manager.
- `crankAllNodes(int nbTicks)` — Advances the entire simulation by `nbTicks` meaningful events. Iterates all node clocks until they catch up to the master clock, then cranks the master clock.
- `crankForAtMost / crankForAtLeast / crankUntil` — Convenience wrappers that crank the simulation until a time limit or predicate is met.
- `haveAllExternalized(uint32, uint32, bool)` — Returns true if all (or validator-only) nodes have externalized at least ledger `num`. Throws if spread exceeds `maxSpread`.
- `addConnection / dropConnection` — Immediately create/drop a loopback or TCP connection.
- `metricsSummary(string)` — Dumps all metrics (or a domain) from the first node using a custom `ConsoleReporterWithSum`.

### `Simulation::Node` (inner struct)

- `mClock` (`shared_ptr<VirtualClock>`) — Per-node clock.
- `mApp` (`Application::pointer`) — The node's application. Destructor ensures app is destroyed before its clock.

### `LoopbackOverlayManager` (extends `OverlayManagerImpl`)

Overrides `connectToImpl` to resolve the target from `Simulation::mPeerMap` and create a `LoopbackPeer` pair instead of opening a TCP socket.

### `ApplicationLoopbackOverlay` (extends `TestApplication`)

A test application variant that creates `LoopbackOverlayManager` as its overlay and holds a back-reference to the owning `Simulation`.

---

### `Topologies`

A static utility class with factory methods that create fully configured `Simulation` instances with common network shapes. All methods accept optional `ConfigGen` and `QuorumSetAdjuster`.

**Factory methods:**
- `pair(mode, networkID)` — Two mutually-trusting validators, one connection.
- `cycle4(networkID)` — Four nodes in a cyclic quorum (loopback only), with cross-connections.
- `core(nNodes, threshold, mode, networkID)` — Fully-connected mesh of `nNodes` with shared quorum set.
- `cycle(nNodes, threshold, mode, networkID)` — Same quorum as `core`, but connections form a one-way ring.
- `branchedcycle(nNodes, ...)` — Ring plus antipodal shortcuts.
- `separate(nNodes, threshold, mode, networkID, numWatchers)` — Shared quorum, no connections (callers add connections). Optionally includes watcher nodes.
- `separateAllHighQuality(nNodes, mode, networkID, confGen)` — Uses automatic quorum generation with all nodes marked `VALIDATOR_HIGH_QUALITY`.
- `hierarchicalQuorum(nBranches, ...)` — 4-node core plus mid-tier branches, connected round-robin.
- `hierarchicalQuorumSimplified(coreSize, nbOuterNodes, ...)` — Core plus outer nodes that trust core + self.
- `customA(mode, networkID)` — 7-node topology (A–S) for resilience tests; node I is dead.
- `asymmetric(mode, networkID)` — 10-node `core` topology plus 4 extra nodes on one validator for asymmetry tests.

---

### `LoadGenMode` (enum)

Defines the type of load the `LoadGenerator` produces:
- `PAY` — Classic native payment transactions.
- `SOROBAN_UPLOAD` — Upload random Wasm blobs (overlay/herder testing).
- `SOROBAN_INVOKE_SETUP` — Deploy contracts for subsequent `SOROBAN_INVOKE`.
- `SOROBAN_INVOKE` — Invoke resource-intensive Soroban contracts.
- `SOROBAN_UPGRADE_SETUP` — Deploy the config-upgrade contract instance.
- `SOROBAN_CREATE_UPGRADE` — Submit a single config upgrade transaction.
- `MIXED_CLASSIC_SOROBAN` — Weighted blend of `PAY`, `SOROBAN_UPLOAD`, and `SOROBAN_INVOKE`.
- `PAY_PREGENERATED` — Read pre-serialized payment transactions from an XDR file.
- `SOROBAN_INVOKE_APPLY_LOAD` — Generate invoke transactions matching `ApplyLoad`'s V2 transaction shape.

### `GeneratedLoadConfig`

Value struct parameterizing a load-generation run.

**Key fields:**
- `mode` (`LoadGenMode`) — What type of transactions to generate.
- `nAccounts`, `offset`, `nTxs`, `txRate` — Account pool size, starting offset, total transactions, target tx/s.
- `spikeInterval`, `spikeSize` — Periodic traffic spikes.
- `maxGeneratedFeeRate` — When set, randomize fee rates up to this value.
- `skipLowFeeTxs` — If true, skip transactions rejected for low fee instead of failing.
- `preloadedTransactionsFile` — Path for `PAY_PREGENERATED` mode.

**Inner structs:**
- `SorobanConfig` — `nInstances`, `nWasms` counts for Soroban setup modes.
- `MixClassicSorobanConfig` — `payWeight`, `sorobanUploadWeight`, `sorobanInvokeWeight` for `MIXED_CLASSIC_SOROBAN`.

**Key methods:**
- `isDone()`, `areTxsRemaining()`, `isLoad()`, `isSoroban()`, `isSorobanSetup()` — State predicates.
- `modeInvokes()`, `modeSetsUpInvoke()`, `modeUploads()` — Check what sub-modes the current mode encompasses.
- `getStatus()` — Returns a JSON summary of the run.
- `copySorobanNetworkConfigToUpgradeConfig(base, updated)` — Copies diffs from Soroban network config into `SorobanUpgradeConfig`.
- Static factories: `createSorobanInvokeSetupLoad`, `createSorobanUpgradeSetupLoad`, `txLoad`, `pregeneratedTxLoad`.

### `SorobanUpgradeConfig`

Large struct of `std::optional<>` fields covering every Soroban network configuration parameter (compute, ledger access, bandwidth, state archival, parallel execution, SCP timing). Used by both `LoadGenerator` and `ApplyLoad` to construct on-chain config upgrade transactions.

---

### `LoadGenerator`

Drives transaction submission against a live herder at a configurable rate. Created per-`Application`.

**Key members:**
- `mTxGenerator` (`TxGenerator`) — Constructs all transaction types.
- `mApp` (`Application&`) — The application receiving transactions.
- `mLoadTimer` (`unique_ptr<VirtualTimer>`) — Fires every `STEP_MSECS` (100 ms) to submit a batch of transactions.
- `mStartTime` — Timestamp when load generation began; used to compute cumulative target count.
- `mTotalSubmitted` (`int64_t`) — Running count of successfully submitted transactions.
- `mAccountsInUse` / `mAccountsAvailable` (`unordered_set<uint64_t>`) — Track source-account availability to avoid submitting transactions with pending source accounts.
- `mContractInstanceKeys` (`UnorderedSet<LedgerKey>`) — Persists across runs; holds deployed contract instance keys.
- `mCodeKey` (`optional<LedgerKey>`) — The Wasm code key from `SOROBAN_INVOKE_SETUP`.
- `mContactOverheadBytes` (`uint64_t`) — Wasm size + overhead, used for resource estimation.
- `mContractInstances` (`UnorderedMap<uint64_t, ContractInstance>`) — Maps account IDs to their assigned contract instance (rebuilt each invoke run).
- `mRoot` (`TestAccountPtr`) — The root/genesis account.
- `mFailed`, `mStarted` (`bool`) — Run state flags.

**Key functions:**
- `generateLoad(GeneratedLoadConfig)` — Main entry point. Calls `start()` once, then runs the step loop: computes `txPerStep`, iterates creating and submitting transactions via `submitTx()`, decrements `cfg.nTxs`, and schedules the next step via `scheduleLoadGeneration()`.
- `start(cfg)` — One-time initialization: populates `mAccountsAvailable`, sets up contract instance mapping for invoke modes, opens preloaded transaction file if needed.
- `scheduleLoadGeneration(cfg)` — Validates configuration, checks protocol version, then schedules `generateLoad` via `mLoadTimer` if the app is synced (otherwise waits 10 s and retries).
- `submitTx(cfg, generateTx)` — Calls the transaction-generator lambda, submits via `execute()`, retries up to `TX_SUBMIT_MAX_TRIES` on `txBAD_SEQ` by refreshing sequence numbers.
- `execute(txf, mode, code)` — Submits a transaction to the herder via `recvTransaction()`, records per-mode metrics, broadcasts on success.
- `getTxPerStep(txRate, spikeInterval, spikeSize)` — Computes total target transactions based on elapsed time and spike schedule, returns the delta since last submission.
- `cleanupAccounts()` — Moves accounts from `mAccountsInUse` back to `mAccountsAvailable` once no longer pending in the herder.
- `waitTillComplete(cfg)` — After all transactions are submitted, waits up to `TIMEOUT_NUM_LEDGERS` for account sequence numbers and Soroban state to sync with the database, then marks completion or failure.
- `checkAccountSynced(app)` — Compares cached account sequence numbers against the database.
- `checkSorobanStateSynced(app, cfg)` — Verifies contract instance and code keys exist in the ledger.
- `checkMinimumSorobanSuccess(cfg)` — Checks whether the configured minimum Soroban success percentage was met.
- `stop()` — Cancels the load timer and resets state.
- `reset()` — Clears per-run state (accounts, timer, counters) but preserves `mContractInstanceKeys` and `mCodeKey`.
- `resetSorobanState()` — Clears contract keys and code key (only on setup failures).

**Control flow:**
1. External caller invokes `generateLoad(cfg)`.
2. `start()` initializes accounts and contract mappings.
3. Each 100 ms step: compute target tx count, generate and submit transactions in a loop, decrement remaining count.
4. When `nTxs` reaches 0, switch to `waitTillComplete()` which polls per-ledger until DB state is consistent.
5. Mark `mLoadgenComplete` or `mLoadgenFail` and call `reset()`.

**Constants:**
- `STEP_MSECS = 100` — Step interval.
- `TX_SUBMIT_MAX_TRIES = 10` — Max retries per transaction.
- `TIMEOUT_NUM_LEDGERS = 20` — Max ledgers to wait for completion.
- `MIN_UNIQUE_ACCOUNT_MULTIPLIER = 3` — Ensures enough unique accounts for sustained rate.

---

### `TxGenerator`

Constructs transaction frames for all supported load types. Shared by `LoadGenerator` and `ApplyLoad`.

**Key members:**
- `mApp` (`Application&`) — Application reference.
- `mAccounts` (`std::map<uint64_t, TestAccountPtr>`) — Account cache, keyed by numeric ID.
- `mMinBalance` (`int64`) — Cached minimum balance for account creation.
- `mApplySorobanSuccess / mApplySorobanFailure` (`medida::Counter&`) — Track Soroban apply outcomes.
- `mPrePopulatedArchivedEntries` / `mNextKeyToRestore` — State for simulating hot-archive disk reads in V2 load.

**Key constants:**
- `ROOT_ACCOUNT_ID = UINT64_MAX` — Sentinel for root account.
- `SAC_TX_INSTRUCTIONS = 250'000` — Instructions per SAC transfer.
- `BATCH_TRANSFER_TX_INSTRUCTIONS = 500'000` — Instructions per batch transfer.

**Transaction creation functions:**
- `paymentTransaction(...)` — Classic XLM payment of 1 stroop between two random accounts.
- `createUploadWasmTransaction(...)` — Uploads a Wasm blob via `HOST_FUNCTION_TYPE_UPLOAD_CONTRACT_WASM`.
- `createContractTransaction(...)` — Instantiates a contract from an uploaded Wasm.
- `createSACTransaction(...)` — Creates a Stellar Asset Contract for a given asset.
- `sorobanRandomWasmTransaction(...)` — Uploads a random-sized Wasm blob with randomized resources sampled from config distributions.
- `invokeSorobanLoadTransaction(...)` — V1 invoke: calls `do_work(guestCycles, hostCycles, numEntries, kbPerEntry)` on the loadgen contract. Resources (instructions, IO, tx size) are sampled from configurable distributions.
- `invokeSorobanLoadTransactionV2(...)` — V2 invoke: calls `do_cpu_only_work(guestCycles, hostCycles, eventCount)` with separate RW entries and archived-entry auto-restore. Used by `ApplyLoad`.
- `invokeSACPayment(...)` — Invokes SAC `transfer` function between accounts.
- `invokeBatchTransfer(...)` — Invokes a batch-transfer contract that performs multiple SAC transfers in one transaction.
- `invokeSorobanCreateUpgradeTransaction(...)` — Writes config upgrade bytes into the upgrade contract.
- `getConfigUpgradeSetFromLoadConfig(SorobanUpgradeConfig)` — Reads current config settings from the ledger, applies deltas from `SorobanUpgradeConfig`, serializes as `ConfigUpgradeSet`.

**Utility functions:**
- `findAccount(id, ledgerNum)` — Looks up or creates a `TestAccount` in the cache.
- `createAccounts(start, count, ledgerNum, initial)` — Creates `createAccount` operations in bulk.
- `createTransactionFramePtr(from, ops, maxFeeRate, byteCount)` — Wraps operations into a signed `TransactionFrame`, optionally padded to a target byte count.
- `generateFee(maxGeneratedFeeRate, opsCnt)` — Generates a fee, optionally randomized up to `maxGeneratedFeeRate`.
- `loadAccount(account)` — Refreshes an account's sequence number from the ledger.
- `pickAccountPair(...)` — Selects source and random destination accounts for payments.
- `reset()` — Clears the account cache.

---

### `ApplyLoadMode` (enum)

- `LIMIT_BASED` — Generate load within configured ledger limits, measure close time.
- `FIND_LIMITS_FOR_MODEL_TX` — Binary-search for max number of a "model" transaction that fits in target close time.
- `MAX_SAC_TPS` — Binary-search for max SAC transfer TPS, ignoring ledger limits.

### `ApplyLoad`

Offline benchmarking harness. Bypasses overlay/herder flooding; directly closes ledgers with generated transaction sets.

**Key members:**
- `mApp` (`Application&`), `mMode` (`ApplyLoadMode`), `mRoot` (`TestAccountPtr`).
- `mTxGenerator` (`TxGenerator`) — Owns its own `TxGenerator` (separate from `LoadGenerator`'s).
- `mNumAccounts` (`uint32_t`) — Total test accounts, computed from config and mode.
- `mUpgradeCodeKey`, `mUpgradeInstanceKey` — Keys for the config-upgrade contract.
- `mLoadCodeKey`, `mLoadInstance` — Keys/instance for the loadgen contract (V2 invocations).
- `mSACInstanceXLM` — SAC contract instance for native XLM.
- `mBatchTransferInstances` (`vector<ContractInstance>`) — One batch-transfer contract per parallel cluster.
- `mDataEntryCount`, `mDataEntrySize` — Dimensions of pre-populated bucket-list data entries.
- Utilization histograms: `mTxCountUtilization`, `mInstructionUtilization`, `mTxSizeUtilization`, `mDiskReadByteUtilization`, `mWriteByteUtilization`, `mDiskReadEntryUtilization`, `mWriteEntryUtilization`.

**Key functions:**
- `execute()` — Dispatches to `benchmarkLimits()`, `findMaxSacTps()`, or `findMaxLimitsForModelTransaction()`.
- `setup()` — Master setup: loads root account, upgrades max tx set size, calls `setupAccounts`, `setupUpgradeContract`, `setupLoadContract`, `setupXLMContract`, optionally `setupBatchTransferContracts` and `setupBucketList`.
- `setupAccounts()` — Creates `mNumAccounts` test accounts in batches via `closeLedger`.
- `setupUpgradeContract()` — Uploads the `write_bytes` Wasm, instantiates it; stores keys in `mUpgradeCodeKey` / `mUpgradeInstanceKey`.
- `setupLoadContract()` — Uploads the `test_wasm_loadgen` Wasm, instantiates it; stores in `mLoadCodeKey` / `mLoadInstance`.
- `setupXLMContract()` — Creates the native-asset SAC; stores in `mSACInstanceXLM`.
- `setupBatchTransferContracts()` — Uploads the SAC-transfer contract, creates one instance per cluster, funds each with XLM.
- `setupBucketList()` — Pre-populates the live and hot-archive bucket lists with synthetic contract data entries over simulated ledgers.
- `closeLedger(txs, upgrades, recordUtilization)` — Creates a `TxSet` from transactions, optionally records resource utilization metrics, then closes the ledger.
- `benchmarkLimits()` — Runs `APPLY_LOAD_NUM_LEDGERS` iterations of `benchmarkLimitsIteration`, logs timing and utilization statistics.
- `benchmarkLimitsIteration()` — Generates classic payments + Soroban V2 invoke transactions up to scaled ledger limits, closes one ledger with utilization recording.
- `findMaxLimitsForModelTransaction()` — Binary-searches over tx count: for each candidate, calls `updateSettingsForTxCount`, upgrades network config, runs `benchmarkLimitsIteration` several times, checks if mean close time is within `APPLY_LOAD_TARGET_CLOSE_TIME_MS`.
- `findMaxSacTps()` — Binary-searches over TPS: generates SAC payment transactions, times just the application phase, finds maximum sustainable TPS.
- `benchmarkSacTps(txsPerLedger)` — Runs `APPLY_LOAD_NUM_LEDGERS` SAC-payment ledgers, returns average close time.
- `generateSacPayments(txs, count)` — Generates SAC payment or batch-transfer transactions with unique destinations to avoid RW conflicts.
- `upgradeSettings()` — Applies the LIMIT_BASED upgrade config via `applyConfigUpgrade`.
- `upgradeSettingsForMaxTPS(txsToGenerate)` — Computes high-limit config for MAX_SAC_TPS mode.
- `applyConfigUpgrade(config)` — Creates an upgrade transaction, validates it, closes a ledger with the upgrade.
- `updateSettingsForTxCount(txsPerLedger)` — Computes rounded ledger limits for a given tx count, returns the config and actual max txs.
- `warmAccountCache()` — Loads all accounts into the BucketListDB cache.
- `successRate()` — Returns fraction of apply-time successes.
- `getKeyForArchivedEntry(index)` (static) — Deterministic `LedgerKey` for a pre-populated hot-archive entry.
- `calculateRequiredHotArchiveEntries(mode, config)` (static) — Estimates total hot-archive entries needed for disk-read simulation.

---

## Key Data Flows

### Simulation Node Lifecycle
1. `Simulation` constructor creates a master `VirtualClock` and an idle `Application`.
2. `addNode()` creates a `Node` with its own clock and an `ApplicationLoopbackOverlay` (or `TestApplication` for TCP mode).
3. `addPendingConnection()` queues connections; `startAllNodes()` starts apps and establishes connections.
4. `crankAllNodes()` drives the main clock and all node clocks forward in lockstep (virtual mode) or lets them run freely (real-time mode).
5. Tests call `haveAllExternalized()` to check consensus progress.

### LoadGenerator Transaction Flow
1. Caller invokes `generateLoad(cfg)` via the HTTP command interface or test code.
2. `start()` initializes account pools and contract mappings.
3. Every 100 ms step, `generateLoad` computes the target batch size, creates transactions via mode-specific lambdas calling `TxGenerator`, and submits each via `execute()` → `Herder::recvTransaction()`.
4. Successful transactions are broadcast to the overlay. Failed transactions (bad seq) trigger account reload and regeneration.
5. After all transactions are submitted, `waitTillComplete()` polls account/Soroban state consistency across ledger closes.

### ApplyLoad Benchmark Flow
1. `ApplyLoad` constructor calls `setup()` which creates accounts, deploys contracts, populates bucket lists, and applies config upgrades.
2. `execute()` dispatches to the selected benchmark mode.
3. In `LIMIT_BASED` mode: each iteration generates a full ledger of mixed classic + Soroban transactions, calls `closeLedger()`, records utilization.
4. In `FIND_LIMITS_FOR_MODEL_TX` mode: binary search adjusts ledger limits, runs multiple iterations per candidate, checks mean close time against target.
5. In `MAX_SAC_TPS` mode: binary search over TPS, generates SAC payments (individual or batched), times the application phase.

---

## Ownership Relationships

- `Simulation` owns `mNodes` (map of `Node` structs), each `Node` owns a `VirtualClock` and `Application`.
- `Simulation` owns `mLoopbackConnections` (shared pointers to `LoopbackPeerConnection`).
- `Simulation` owns `mIdleApp` for the master clock's IO context.
- `LoadGenerator` is owned by `Application` (one per app). It owns a `TxGenerator`, a `VirtualTimer`, and the account/contract state.
- `ApplyLoad` is a standalone object created for benchmarking. It owns its own `TxGenerator` and all contract/account state.
- `TxGenerator` is owned by either `LoadGenerator` or `ApplyLoad`. It owns the account cache (`mAccounts`).
- `GeneratedLoadConfig` and `SorobanUpgradeConfig` are value types passed by copy through the load-generation pipeline.

## Threading Model

- In `OVER_LOOPBACK` mode all nodes share a single thread; `crankAllNodes()` advances node clocks sequentially.
- In `OVER_TCP` mode each node has a real-time clock; `crankAllNodes()` spins and sleeps.
- `LoadGenerator` runs entirely on the main thread, driven by `VirtualTimer` callbacks.
- `ApplyLoad` runs synchronously on the main thread; it directly closes ledgers without involving the overlay or herder flooding.
