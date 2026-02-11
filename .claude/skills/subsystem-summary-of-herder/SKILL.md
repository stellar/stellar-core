---
name: subsystem-summary-of-herder
description: "read this skill for a token-efficient summary of the herder subsystem"
---

# Herder Subsystem Technical Summary

## Overview

The Herder subsystem drives the Stellar Consensus Protocol (SCP) for stellar-core. It is responsible for collecting transactions from the network, proposing transaction sets for consensus, validating SCP messages, managing protocol upgrades, and delivering externalized ledger values to the LedgerManager for application. All core Herder logic runs on the main thread.

---

## Key Classes and Data Structures

### Herder (Abstract Interface)
- **File:** `Herder.h`
- Pure virtual interface defining the public API for the subsystem.
- Defines constants: `TARGET_LEDGER_CLOSE_TIME_BEFORE_PROTOCOL_VERSION_23_MS` (5s), `MAX_SCP_TIMEOUT_SECONDS` (240s), `CONSENSUS_STUCK_TIMEOUT_SECONDS` (35s), `OUT_OF_SYNC_RECOVERY_TIMER` (10s), `LEDGER_VALIDITY_BRACKET` (100 slots), `MAX_TIME_SLIP_SECONDS` (60s), `TX_SET_GC_DELAY` (1 min).
- Defines state machine: `HERDER_BOOTING_STATE` → `HERDER_SYNCING_STATE` → `HERDER_TRACKING_NETWORK_STATE`.
- Defines `EnvelopeStatus`: `DISCARDED`, `SKIPPED_SELF`, `PROCESSED`, `FETCHING`, `READY`.

### HerderImpl (Core Implementation)
- **File:** `HerderImpl.h`, `HerderImpl.cpp` (~2679 lines)
- Concrete implementation of `Herder`. Owns all major sub-components:
  - `mTransactionQueue` (`ClassicTransactionQueue`) — classic transaction queue.
  - `mSorobanTransactionQueue` (`unique_ptr<SorobanTransactionQueue>`) — created lazily at protocol ≥ 20.
  - `mPendingEnvelopes` (`PendingEnvelopes`) — manages SCP envelope fetching/staging.
  - `mUpgrades` (`Upgrades`) — protocol upgrade management.
  - `mHerderSCPDriver` (`HerderSCPDriver`) — SCP driver implementation.
  - `mLastQuorumMapIntersectionState` (`shared_ptr<QuorumMapIntersectionState>`) — quorum intersection analysis state.
- Tracks consensus via `ConsensusData` struct (`mTrackingSCP`): `mConsensusIndex` and `mConsensusCloseTime`.
- State transitions: `BOOTING → SYNCING ↔ TRACKING`. Transition to `BOOTING` from `TRACKING`/`SYNCING` is disallowed.

### HerderSCPDriver (SCP Driver)
- **File:** `HerderSCPDriver.h`, `HerderSCPDriver.cpp` (~1517 lines)
- Implements `SCPDriver` interface. Bridges between SCP core and Herder.
- Owns `mSCP` (`SCP` instance), `mSCPTimers` (per-slot timer map), `mSCPExecutionTimes` (timing data per slot), `mTxSetValidCache` (`RandomEvictionCache` for tx set validity).
- Tracks `mMissingNodes` and `mDeadNodes` for dead node detection.
- Key inner struct `SCPTiming`: records nomination start, prepare start, timeout counts, and externalize timing.

### PendingEnvelopes
- **File:** `PendingEnvelopes.h`, `PendingEnvelopes.cpp` (~1013 lines)
- Manages the lifecycle of SCP envelopes from receipt to SCP processing.
- Per-slot state tracked via `SlotEnvelopes` struct:
  - `mDiscardedEnvelopes`, `mProcessedEnvelopes` (sets of `SCPEnvelope`).
  - `mFetchingEnvelopes` (map from envelope → timestamp).
  - `mReadyEnvelopes` (vector of `SCPEnvelopeWrapperPtr`).
  - `mReceivedCost` (cost tracking per validator `NodeID`).
- Caches: `mQsetCache` and `mTxSetCache` (both `RandomEvictionCache`), plus `mKnownQSets`/`mKnownTxSets` (weak reference maps).
- Owns `mTxSetFetcher` and `mQuorumSetFetcher` (`ItemFetcher` instances) — request missing data from peers.
- Owns `mQuorumTracker` (`QuorumTracker`) — maintains transitive quorum set.

### TransactionQueue (Base) / ClassicTransactionQueue / SorobanTransactionQueue
- **File:** `TransactionQueue.h`, `TransactionQueue.cpp` (~1416 lines)
- Stores pending transactions per source account (`AccountStates` map).
- `AccountState`: `mTotalFees`, `mAge`, `mTransaction` (optional `TimestampedTx`).
- `BannedTransactions`: deque of `UnorderedSet<Hash>` for ban tracking.
- Owns `mTxQueueLimiter` (`unique_ptr<TxQueueLimiter>`) for resource-based eviction.
- `ClassicTransactionQueue`: supports DEX arbitrage flood damping via `mArbitrageFloodDamping`.
- `SorobanTransactionQueue`: supports `resetAndRebuild()` triggered on config upgrades; has separate flood period (`FLOOD_SOROBAN_TX_PERIOD_MS`).
- AddResult codes: `PENDING`, `DUPLICATE`, `ERROR`, `TRY_AGAIN_LATER`, `FILTERED`.
- Key lifecycle: `tryAdd()` → `removeApplied()` / `ban()` → `shift()` (age-out) → `rebroadcast()`.

### TxSetXDRFrame / ApplicableTxSetFrame / TxSetPhaseFrame
- **File:** `TxSetFrame.h`, `TxSetFrame.cpp`
- `TxSetXDRFrame`: immutable wrapper around raw XDR (`TransactionSet` or `GeneralizedTransactionSet`). Safe for overlay exchange without validation. Created via `makeFromWire()`, `makeEmpty()`, `makeFromHistoryTransactions()`.
- `ApplicableTxSetFrame`: validated/interpreted tx set ready for application. Created from `TxSetXDRFrame::prepareForApply()` or `makeTxSetFromTransactions()`. Contains `TxSetPhaseFrame` per phase.
- `TxSetPhaseFrame`: represents one phase (CLASSIC or SOROBAN). May be sequential (flat `TxFrameList`) or parallel (`TxStageFrameList` with stages → clusters → txs). Has an `InclusionFeeMap` for per-tx base fees. Provides iterator for ordered traversal.
- Parallel structure types: `TxClusterFrame` (list of txs), `TxStageFrame` (list of clusters), `TxStageFrameList` (list of stages).

### SurgePricingUtils
- **File:** `SurgePricingUtils.h`, `SurgePricingUtils.cpp`
- `SurgePricingLaneConfig` (abstract): defines lane assignment, per-lane resource limits.
  - `DexLimitingLaneConfig`: lane 0 = generic, lane 1 = DEX-limited.
  - `SorobanGenericLaneConfig`: single generic lane for Soroban multi-dimensional resources.
- `SurgePricingPriorityQueue`: priority queue for transactions sorted by fee rate. Supports `add`, `erase`, `canFitWithEviction`, `visitTopTxs`, `popTopTxs`. Used by both tx queue limiter and tx set building.

### TxQueueLimiter
- **File:** `TxQueueLimiter.h`, `TxQueueLimiter.cpp`
- Enforces resource limits on the transaction queue using two `SurgePricingPriorityQueue`s:
  - `mTxs`: lowest-fee-first ordering for eviction decisions.
  - `mTxsToFlood`: highest-fee-first ordering for flood priority.
- Tracks `mLaneEvictedInclusionFee` (max evicted fee per lane).
- `canAddTx()` determines if a tx can fit, returning eviction candidates.

### TxSetUtils
- **File:** `TxSetUtils.h`, `TxSetUtils.cpp`
- Static utilities: `sortTxsInHashOrder`, `buildAccountTxQueues`, `getInvalidTxList`, `trimInvalid`.
- `AccountTransactionQueue`: helper deque for per-account tx ordering.

### ParallelTxSetBuilder
- **File:** `ParallelTxSetBuilder.h`, `ParallelTxSetBuilder.cpp`
- `buildSurgePricedParallelSorobanPhase()`: builds parallel processing stages from Soroban txs. Uses surge pricing to fill stages/clusters within network config limits.

### Upgrades / ConfigUpgradeSetFrame
- **File:** `Upgrades.h`, `Upgrades.cpp`
- `Upgrades::UpgradeParameters`: scheduled upgrade config (protocol version, base fee, max tx set size, base reserve, flags, Soroban config key, nomination timeout limit, expiration).
- `createUpgradesFor()`: creates upgrade steps based on LCL and current time.
- `isValid()` / `isValidForApply()`: validates upgrade steps.
- `removeUpgrades()`: strips applied upgrades from pending set.
- `ConfigUpgradeSetFrame`: wraps a `ConfigUpgradeSet` retrieved from ledger via `ConfigUpgradeSetKey`. Validates XDR hash match and applies Soroban network config changes.

### LedgerCloseData
- **File:** `LedgerCloseData.h`, `LedgerCloseData.cpp`
- Value object carrying: `mLedgerSeq`, `mTxSet` (`TxSetXDRFrameConstPtr`), `mValue` (`StellarValue`), optional `mExpectedLedgerHash`.
- Passed from Herder to `LedgerManager::valueExternalized()`.

### QuorumTracker
- **File:** `QuorumTracker.h`, `QuorumTracker.cpp`
- Tracks the transitive quorum graph rooted at the local node.
- `NodeInfo`: `mQuorumSet`, `mDistance` (from local node), `mClosestValidators` (set of local qset nodes shortest-distance to this node).
- `QuorumMap` = `UnorderedMap<NodeID, NodeInfo>`.
- `expand()`: incrementally adds a node; returns false if quorum set conflicts (triggers `rebuild()`).
- `rebuild()`: reconstructs entire quorum from a lookup function.

### QuorumIntersectionChecker / QuorumIntersectionCheckerImpl
- **File:** `QuorumIntersectionChecker.h`, `QuorumIntersectionCheckerImpl.h` (547 lines), `QuorumIntersectionCheckerImpl.cpp`
- V1 checker: C++ implementation based on Lachowski's algorithm (branch-and-bound enumeration of minimal quorums, checking for non-intersecting pairs). Runs on a background thread with `mInterruptFlag` for cooperative cancellation.
- Key refinements: complement checking, set contraction to maximal quorums, bottom-up enumeration, half-space pruning, SCC-based pruning.
- `getIntersectionCriticalGroups()`: finds node groups whose removal breaks quorum intersection.

### RustQuorumCheckerAdaptor
- **File:** `RustQuorumCheckerAdaptor.h`, `RustQuorumCheckerAdaptor.cpp`
- V2 checker: runs quorum intersection analysis via Rust SAT solver in a **separate process** (critical for memory safety — Rust allocator aborts on OOM).
- `runQuorumIntersectionCheckAsync()`: serializes quorum map to JSON, spawns `stellar-core --check-quorum-intersection` subprocess, reads results from output JSON.
- `networkEnjoysQuorumIntersection()`: in-process Rust entry point (used by subprocess).
- `QuorumCheckerMetrics`: tracks success/failure/abort counts, timing, memory usage.
- `QuorumMapIntersectionState`: shared state between main thread and background analysis — tracks last check ledger, hash, recalculating flag, results.

### HerderPersistence / HerderPersistenceImpl
- **File:** `HerderPersistence.h`, `HerderPersistenceImpl.h`, `HerderPersistenceImpl.cpp`
- Persists SCP history (envelopes + quorum map) to database per ledger.
- `saveSCPHistory()`: stores envelopes and quorum sets.
- Static helpers: `copySCPHistoryToStream()`, `getNodeQuorumSet()`, `getQuorumSet()`.

### HerderUtils
- **File:** `HerderUtils.h`, `HerderUtils.cpp`
- Utility functions: `toStellarValue()`, `getTxSetHashes()`, `getStellarValues()`, `toShortString()`, `toQuorumIntersectionMap()`, `parseQuorumMapFromJson()`.

### FilteredEntries
- **File:** `FilteredEntries.h`
- Constants for ledger keys to filter from Soroban transaction queue (production network only). Currently empty (`KEYS_TO_FILTER_P24_COUNT = 0`).

---

## Key Control Flows

### Startup Flow
1. `HerderImpl::start()` initializes `mMaxTxSize`, sets up Soroban queue if protocol ≥ 20, validates flow control config.
2. Sets tracking state from LCL. If not genesis and not `FORCE_SCP`, restores SCP state from database via `restoreSCPState()` and restores upgrades via `restoreUpgrades()`.
3. Starts `mTxSetGarbageCollectTimer` and `mCheckForDeadNodesTimer`.
4. If `FORCE_SCP`, calls `bootstrap()` which forces join via `mHerderSCPDriver.bootstrap()` + `setupTriggerNextLedger()`.

### Ledger Close / Trigger Flow
1. `setupTriggerNextLedger()`: computes next trigger time from last ballot start + expected close time. Sets `mTriggerTimer` to call `triggerNextLedger()`.
2. `triggerNextLedger(ledgerSeqToTrigger)`: gathers txs from both queues, computes close time, calls `makeTxSetFromTransactions()` to build proposed set with surge pricing, creates `StellarValue`, calls `mHerderSCPDriver.nominate()`.
3. SCP runs nomination → prepare → commit → externalize.
4. `HerderSCPDriver::valueExternalized()`: cancels SCP timers, records metrics, calls `mHerder.valueExternalized()`.
5. `HerderImpl::valueExternalized()`: records close time drift, dumps SCP info if slow, calls `processExternalized()` then `newSlotExternalized()`, then quorum intersection check.
6. `processExternalized()`: saves SCP history, removes applied upgrades, creates `LedgerCloseData`, calls `mLedgerManager.valueExternalized()`.
7. `lastClosedLedgerIncreased()` (callback from LedgerManager after apply): updates tx queue (`updateTransactionQueue()`), handles upgrades (`maybeHandleUpgrade()`), calls `setupTriggerNextLedger()` if latest.

### SCP Envelope Processing Flow
1. `recvSCPEnvelope()`: validates close time, checks ledger range, verifies signature, passes to `mPendingEnvelopes.recvSCPEnvelope()`.
2. `PendingEnvelopes::recvSCPEnvelope()`: checks if envelope is already processed/discarded, starts fetching missing tx sets and quorum sets via `ItemFetcher`.
3. When all dependencies are fetched, envelope becomes `READY`, added to `mReadyEnvelopes`.
4. `processSCPQueue()` / `processSCPQueueUpToIndex()`: pops ready envelopes and feeds them to `SCP::receiveEnvelope()`.

### Transaction Queue Flow
1. `recvTransaction()`: routes to classic or Soroban queue based on `tx->isSoroban()`. Enforces 1-tx-per-source-account-per-ledger across both queues.
2. `TransactionQueue::tryAdd()`: calls `canAdd()` which checks validity, sequence numbers, fee sufficiency via `TxQueueLimiter::canAddTx()`. May evict lower-fee txs.
3. `updateTransactionQueue()`: after ledger close, calls `removeApplied()` + `shift()` (age accounts) + `ban()` invalid txs + `rebroadcast()`.
4. `SorobanTransactionQueue::resetAndRebuild()`: triggered on config upgrade; extracts all txs, clears state, re-adds with new limits.

### Out-of-Sync Recovery
1. `trackingHeartBeat()`: resets `mTrackingTimer` (35s). If it fires, calls `herderOutOfSync()`.
2. `herderOutOfSync()`: transitions to `SYNCING`, starts `mOutOfSyncTimer` (10s periodic).
3. `outOfSyncRecovery()`: purges old slots, rebroadcasts own messages, calls `getMoreSCPState()` to ask 2 random peers for SCP messages.

### Quorum Intersection Checking
- V1 (`checkAndMaybeReanalyzeQuorumMap`): hashes current quorum map, if changed spawns background thread running C++ `QuorumIntersectionChecker`. Supports cooperative interruption.
- V2 (`checkAndMaybeReanalyzeQuorumMapV2`): serializes quorum map to JSON, spawns out-of-process `stellar-core --check-quorum-intersection` running Rust SAT solver. Results posted back to main thread.
- Controlled by `USE_QUORUM_INTERSECTION_CHECKER_V2` config flag.

---

## Timers and Periodic Tasks

| Timer | Period | Purpose |
|-------|--------|---------|
| `mTriggerTimer` | ~expected ledger close time | Triggers `triggerNextLedger()` for next consensus round |
| `mTrackingTimer` | 35s (`CONSENSUS_STUCK_TIMEOUT_SECONDS`) | Detects consensus stuck → `herderOutOfSync()` |
| `mOutOfSyncTimer` | 10s (`OUT_OF_SYNC_RECOVERY_TIMER`) | Periodic out-of-sync recovery attempts |
| `mTxSetGarbageCollectTimer` | 1 min (`TX_SET_GC_DELAY`) | Purges old persisted tx sets |
| `mCheckForDeadNodesTimer` | 15 min (`CHECK_FOR_DEAD_NODES_MINUTES`) | Detects nodes missing from SCP |
| `mBroadcastTimer` (per queue) | `FLOOD_TX_PERIOD_MS` / `FLOOD_SOROBAN_TX_PERIOD_MS` | Periodic tx rebroadcast |
| SCP timers (`mSCPTimers`) | Configurable per round (linear backoff, caps at 30 min) | SCP nomination/ballot protocol timeouts |

---

## Ownership Relationships

```
HerderImpl
├── mTransactionQueue (ClassicTransactionQueue, owned)
│   └── mTxQueueLimiter (TxQueueLimiter, unique_ptr)
│       ├── mTxs (SurgePricingPriorityQueue, unique_ptr) — eviction ordering
│       └── mTxsToFlood (SurgePricingPriorityQueue, unique_ptr) — flood ordering
├── mSorobanTransactionQueue (SorobanTransactionQueue, unique_ptr, created at protocol ≥ 20)
│   └── mTxQueueLimiter (same structure as above)
├── mPendingEnvelopes (PendingEnvelopes, owned)
│   ├── mEnvelopes (map<uint64, SlotEnvelopes>)
│   ├── mQsetCache / mKnownQSets — quorum set caches
│   ├── mTxSetCache / mKnownTxSets — tx set caches
│   ├── mTxSetFetcher (ItemFetcher) — fetches missing tx sets from peers
│   ├── mQuorumSetFetcher (ItemFetcher) — fetches missing quorum sets from peers
│   └── mQuorumTracker (QuorumTracker) — transitive quorum state
├── mUpgrades (Upgrades, owned)
├── mHerderSCPDriver (HerderSCPDriver, owned)
│   ├── mSCP (SCP instance, owned)
│   ├── mSCPTimers (map<slotIndex, map<timerID, VirtualTimer>>)
│   └── mTxSetValidCache (RandomEvictionCache)
├── mLastQuorumMapIntersectionState (shared_ptr<QuorumMapIntersectionState>)
└── Various VirtualTimers (mTriggerTimer, mTrackingTimer, mOutOfSyncTimer, etc.)
```

---

## Key Data Flows

### Transaction Ingestion → Consensus
```
Network/HTTP → recvTransaction() → TransactionQueue::tryAdd()
    → TxQueueLimiter::canAddTx() [surge pricing check]
    → AccountStates update
    → broadcastTx() [periodic via mBroadcastTimer]

triggerNextLedger():
    TransactionQueue::getTransactions() → makeTxSetFromTransactions()
    → SurgePricingPriorityQueue::getMostTopTxsWithinLimits() [for each phase]
    → TxSetXDRFrame + ApplicableTxSetFrame
    → HerderSCPDriver::nominate()
```

### SCP Message Processing
```
Network → recvSCPEnvelope()
    → checkCloseTime() + verifyEnvelope()
    → PendingEnvelopes::recvSCPEnvelope()
        → startFetch() [tx sets, quorum sets via ItemFetcher]
        → [when fetched] envelopeReady() → mReadyEnvelopes
    → processSCPQueue()
        → PendingEnvelopes::pop() → SCP::receiveEnvelope()
        → [on externalize] HerderSCPDriver::valueExternalized()
            → HerderImpl::valueExternalized()
                → processExternalized() → LedgerCloseData → LedgerManager
```

### Ledger Close Feedback
```
LedgerManager::valueExternalized()
    → [applies ledger]
    → Herder::lastClosedLedgerIncreased()
        → maybeSetupSorobanQueue()
        → maybeHandleUpgrade() [overlay flow control sizing]
        → updateTransactionQueue() [removeApplied, shift, ban invalid, rebroadcast]
        → setupTriggerNextLedger() [next round]
```

### Quorum Health Monitoring
```
valueExternalized() → checkAndMaybeReanalyzeQuorumMap[V2]()
    → getQmapHash(currentQuorum)
    → [if changed] spawn background analysis
        V1: background thread with QuorumIntersectionChecker
        V2: subprocess with Rust SAT solver
    → results posted to main thread → QuorumMapIntersectionState
    → exposed via getJsonTransitiveQuorumIntersectionInfo()
```

---

## Important Constants

- `TRANSACTION_QUEUE_TIMEOUT_LEDGERS = 4` — max age before tx eviction.
- `TRANSACTION_QUEUE_BAN_LEDGERS = 10` — ban duration for rejected txs.
- `TXSETVALID_CACHE_SIZE = 1000` — tx set validity cache entries.
- `QSET_CACHE_SIZE = 10000`, `TXSET_CACHE_SIZE = 10000` — pending envelope caches.
- `CLOSE_TIME_DRIFT_LEDGER_WINDOW_SIZE = 120` — window for clock drift detection.
- `CLOSE_TIME_DRIFT_SECONDS_THRESHOLD = 10` — drift warning threshold.
- `FEE_MULTIPLIER = 10` — fee multiplier for replace-by-fee.
- `MAX_TIMEOUT_MS = 1,800,000` (30 min) — maximum SCP timer timeout.
