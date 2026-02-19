---
name: subsystem-summary-of-invariant
description: "read this skill for a token-efficient summary of the invariant subsystem"
---

# Invariant Subsystem — Technical Summary

## Overview

The invariant subsystem provides a runtime correctness-checking framework for stellar-core. It defines a registry of invariant checks that are executed at key lifecycle events (operation apply, ledger commit, bucket apply, assume-state, and periodic background snapshots). When an invariant is violated, it either throws `InvariantDoesNotHold` (for strict invariants) or logs an error (for non-strict ones). Invariants are registered at application startup and enabled via configuration patterns (regex matching on invariant names).

## Key Files

- **Invariant.h** — Abstract base class `Invariant` with virtual `checkOn*` hooks.
- **InvariantManager.h** — Abstract interface for the invariant registry and dispatch system.
- **InvariantManagerImpl.h / .cpp** — Concrete implementation of `InvariantManager`; owns invariant registration, enablement, dispatch loops, failure handling, and background snapshot scheduling.
- **InvariantDoesNotHold.h** — Exception type thrown when a strict invariant fails.
- **ConservationOfLumens.h / .cpp** — Validates total lumen supply is conserved across operations and via full BucketList snapshot scans.
- **AccountSubEntriesCountIsValid.h / .cpp** — Validates `numSubEntries` on accounts matches actual sub-entry counts.
- **BucketListIsConsistentWithDatabase.h / .cpp** — Cross-checks BucketList entries against SQL database (offers) during catchup.
- **LedgerEntryIsValid.h / .cpp** — Validates structural correctness and field bounds of all `LedgerEntry` types.
- **LiabilitiesMatchOffers.h / .cpp** — Ensures buying/selling liabilities on accounts/trustlines match aggregated offer liabilities.
- **SponsorshipCountIsValid.h / .cpp** — Validates `numSponsoring`/`numSponsored` counters on accounts match sponsorship extensions.
- **ConstantProductInvariant.h / .cpp** — Ensures the constant-product AMM invariant (`reserveA * reserveB`) never decreases.
- **OrderBookIsNotCrossed.h / .cpp** — (BUILD_TESTS only) Maintains an in-memory order book and checks it is never crossed.
- **BucketListStateConsistency.h / .cpp** — Background snapshot invariant validating consistency between BucketList, InMemorySorobanState, and HotArchive.
- **ArchivedStateConsistency.h / .cpp** — Validates eviction and restore operations are consistent with live/archived state.
- **EventsAreConsistentWithEntryDiffs.h / .cpp** — Validates SAC (Stellar Asset Contract) events match ledger entry balance diffs.

---

## Core Framework

### `Invariant` (abstract base class)

The base class for all invariant implementations. Each subclass overrides one or more `checkOn*` virtual methods and returns an empty string on success or an error description string on failure.

**Key members:**
- `mStrict` (bool, const) — If true, failure throws `InvariantDoesNotHold` (fatal). If false, failure is logged as an error but execution continues.
- `getName()` — Pure virtual; returns the invariant's unique name string.
- `checkOnOperationApply(operation, result, ltxDelta, events, app)` — Called after each operation is applied within a transaction. Receives the operation, its result, the `LedgerTxnDelta` (all entry changes plus header changes), contract events, and an `AppConnector`.
- `checkOnBucketApply(bucket, oldestLedger, newestLedger, shadowedKeys)` — Called during catchup when a bucket is applied to the database.
- `checkAfterAssumeState(newestLedger)` — Called after the BucketList state has been assumed (end of catchup).
- `checkOnLedgerCommit(lclLiveState, lclHotArchiveState, persistentEvicted, tempAndTTLEvicted, restoredFromArchive, restoredFromLiveState)` — Called at ledger commit time with eviction/restore vectors.
- `checkSnapshot(liveSnapshot, hotArchiveSnapshot, inMemorySnapshot, isStopping)` — Called periodically on a background thread for expensive full-state scans.
- `snapshotForFuzzer()` / `resetForFuzzer()` — (BUILD_TESTS only) Snapshot/restore internal state for fuzzing rollback.

Helper function `shouldAbortInvariantScan(errorMsg, isStopping)` returns true if an error has been found or the node is shutting down, used to short-circuit long-running BucketList scans.

### `InvariantManager` (abstract interface)

Provides the public API for registering, enabling, and dispatching invariants.

**Key methods:**
- `create(Application&)` — Factory; returns `InvariantManagerImpl`.
- `registerInvariant(shared_ptr<Invariant>)` — Adds an invariant to the registry by name.
- `registerInvariant<T>(args...)` — Templated convenience for constructing + registering.
- `enableInvariant(name)` — Enables invariant(s) matching a regex pattern.
- `checkOnOperationApply(...)` / `checkOnBucketApply(...)` / `checkAfterAssumeState(...)` / `checkOnLedgerCommit(...)` — Dispatch calls to all enabled invariants.
- `runStateSnapshotInvariant(...)` — Runs `checkSnapshot` on all enabled invariants in a background thread.
- `shouldRunInvariantSnapshot()` / `markStartOfInvariantSnapshot()` — Coordinate snapshot timing with LedgerManager.
- `start(LedgerManager&)` — Initializes the snapshot timer if `INVARIANT_EXTRA_CHECKS` is enabled.
- `getJsonInfo()` — Returns JSON with failure history for the `/info` endpoint.
- `isBucketApplyInvariantEnabled()` — Checks if `BucketListIsConsistentWithDatabase` is enabled.

### `InvariantManagerImpl`

**Key data members:**
- `mConfig` — Reference to application config.
- `mInvariants` — `map<string, shared_ptr<Invariant>>`: registry of all invariants by name.
- `mEnabled` — `vector<shared_ptr<Invariant>>`: subset that is currently enabled.
- `mInvariantFailureCount` — Medida counter for total failures.
- `mStateSnapshotInvariantSkipped` — Medida counter for skipped snapshot runs.
- `mStateSnapshotInvariantRunning` — `atomic<bool>`: true while a background snapshot scan is in progress.
- `mShouldRunStateSnapshotInvariant` — `atomic<bool>`: flag set by the timer, read by LedgerManager.
- `mStateSnapshotTimer` — `VirtualTimer` scheduling periodic snapshot checks.
- `mFailureInformation` — `map<string, InvariantFailureInformation>` guarded by `mFailureInformationMutex`; tracks last failure ledger and message per invariant.

**Dispatch logic:**
Each `checkOn*` method iterates over `mEnabled`, calls the corresponding virtual method on each invariant, and if a non-empty error string is returned, calls `onInvariantFailure()`. The `onInvariantFailure` method increments the failure counter, records failure info, and calls `handleInvariantFailure()` which either throws `InvariantDoesNotHold` (strict) or logs an error (non-strict). In fuzzing builds, failures always `abort()`.

**Protocol version gating:**
`checkOnOperationApply` skips all invariants except `EventsAreConsistentWithEntryDiffs` for ledgers before protocol version 8.

**Snapshot scheduling:**
When `INVARIANT_EXTRA_CHECKS` is enabled, `start()` calls `scheduleSnapshotTimer()`. The timer fires `snapshotTimerFired()`, which sets `mShouldRunStateSnapshotInvariant = true` if no prior scan is running. LedgerManager reads `shouldRunInvariantSnapshot()` and, when true, snapshots the state and dispatches `runStateSnapshotInvariant()` on a background thread. The background thread iterates all enabled invariants calling `checkSnapshot()`. If the previous scan is still running when the timer fires, the run is skipped and a metric is incremented.

### `InvariantDoesNotHold`

A `std::runtime_error` subclass thrown when a strict invariant fails. Caught upstream (e.g., in LedgerManager) to trigger node shutdown.

---

## Individual Invariants

### `ConservationOfLumens` (non-strict)

**Purpose:** Ensures the total supply of lumens is conserved. During normal operations, `totalCoins` and `feePool` in the LedgerHeader must not change (except during inflation). The full BucketList snapshot mode sums all native balances across accounts, trustlines, claimable balances, liquidity pools, and Stellar Asset Contract balance entries (both live and hot-archived) and compares to `header.totalCoins`.

**Hooks used:** `checkOnOperationApply`, `checkSnapshot`.

**Key logic:**
- `calculateDeltaBalance()` computes the change in native asset balance for each entry delta, using `getAssetBalance()` which understands SAC contract data entries.
- On operation apply: sums all balance deltas across entries. For inflation, validates `deltaTotalCoins == inflationPayouts + deltaFeePool` and `deltaBalances == inflationPayouts`. For non-inflation, all deltas must be zero.
- On snapshot: iterates all buckets scanning for entry types that can hold native assets, handles shadowing via `countedKeys` sets, sums live + hot-archive balances + feePool, and compares to `totalCoins`. Only runs from protocol V24+.

**Constructor takes:** `AssetContractInfo` for the lumen SAC contract (contract ID, balance key symbol, amount symbol).

### `AccountSubEntriesCountIsValid` (non-strict)

**Purpose:** Validates that the `numSubEntries` field on each account matches the actual count of sub-entries (trustlines, offers, data entries, signers). Pool-share trustlines count as 2 sub-entries.

**Hook used:** `checkOnOperationApply`.

**Key logic:** Builds a `UnorderedMap<AccountID, SubEntriesChange>` tracking deltas in `numSubEntries` (from the account entry) vs. `calculatedSubEntries` (from counting sub-entry creates/deletes). Also checks that deleted accounts have no remaining sub-entries other than signers.

### `BucketListIsConsistentWithDatabase` (strict)

**Purpose:** Cross-checks entries in BucketList buckets against the SQL database during catchup/bucket-apply. Only checks entry types not supported by BucketListDB (currently only OFFERs).

**Hooks used:** `checkOnBucketApply`, `checkAfterAssumeState`.

**Key logic:**
- `checkOnBucketApply`: Iterates a single bucket, verifies ordering, checks `lastModifiedLedgerSeq` bounds, and compares each LIVE/INIT entry against the database and each DEAD entry is absent from the database. Validates total offer count matches.
- `checkAfterAssumeState`: Iterates the entire BucketList, checking all unshadowed offer entries against the database.
- `checkEntireBucketlist()`: Offline self-check entry point that loads the complete BucketList and compares against the database.

**Holds a reference to `Application`** for database and BucketManager access.

### `LedgerEntryIsValid` (non-strict)

**Purpose:** Validates structural correctness, field bounds, and immutability constraints for all ledger entry types after each operation.

**Hook used:** `checkOnOperationApply`.

**Key logic:** Dispatches to type-specific `checkIsValid()` overloads:
- **AccountEntry**: balance ≥ 0, seqNum ≥ 0, valid flags, sorted signers with valid weights, v2 extension constraints, numSubEntries + numSponsoring ≤ UINT32_MAX.
- **TrustLineEntry**: valid non-native asset, 0 ≤ balance ≤ limit, valid flags, pool-share has no liabilities, clawback flag immutability.
- **OfferEntry**: positive offerID/amount, valid assets, valid price (n > 0, d ≥ 1), valid flags.
- **DataEntry**: non-empty valid dataName.
- **ClaimableBalanceEntry**: must be sponsored, valid predicates (max depth 4), immutable once created, valid asset, positive amount, clawback not on native.
- **LiquidityPoolEntry**: V18+ only, constant-product type, valid ordered assets, fee = 30 bps, non-negative reserves/shares/counts, immutable params.
- **ContractDataEntry**: validates lumen SAC balance entries have correct structure (persistent, I128 amount within int64 range).
- **ContractCodeEntry**: `sha256(code) == hash`, hash/code immutable after creation.
- **TTLEntry**: keyHash immutable, liveUntilLedgerSeq non-decreasing.
- All: `lastModifiedLedgerSeq == current ledgerSeq`.

### `LiabilitiesMatchOffers` (non-strict)

**Purpose:** Ensures buying/selling liabilities on accounts and trustlines stay in sync with the aggregated liabilities implied by their offers, and that balances respect liability + reserve constraints.

**Hook used:** `checkOnOperationApply`.

**Key logic (V10+ only for liabilities):**
- Accumulates a `LiabilitiesMap` (per-account, per-asset liabilities delta) by adding current entry liabilities and subtracting previous entry liabilities for accounts, trustlines, and offers.
- For offers: selling liabilities = `exchangeV10WithoutPriceErrorThresholds(...)`.numWheatReceived; buying liabilities = numSheepSend.
- After accumulation, all per-account per-asset liability deltas must be zero (offers match account/trustline liabilities).
- Also checks: unauthorized trustlines cannot increase liabilities; balance ≥ minBalance + sellingLiabilities for accounts; balance ≥ sellingLiabilities and limit - balance ≥ buyingLiabilities for trustlines.
- `checkAuthorized()` validates authorization state transitions on trustlines.

### `SponsorshipCountIsValid` (non-strict)

**Purpose:** Validates per-account `numSponsoring` and `numSponsored` counters match actual sponsorship extensions on entries. Only active from protocol V14+.

**Hook used:** `checkOnOperationApply`.

**Key logic:**
- `updateCounters()` walks entry extensions: if `sponsoringID` is set, increments numSponsoring for the sponsor and numSponsored for the owning account (or claimableBalanceReserve for claimable balances). Multiplier depends on entry type (accounts = 2, pool-share trustlines = 2, claimable balances = number of claimants, others = 1). Also counts signer-level sponsorships from v2 account extensions.
- Compares computed deltas (`numSponsoring`/`numSponsored` maps) against the actual delta in account entries. Checks that no unmatched changes remain.

### `ConstantProductInvariant` (strict)

**Purpose:** Ensures the AMM constant product `reserveA * reserveB` never decreases for liquidity pool entries (except during withdrawals, SetTrustLineFlags, and AllowTrust operations which are excluded).

**Hook used:** `checkOnOperationApply`.

**Key logic:** For each modified liquidity pool entry, validates `currentReserveA * currentReserveB >= previousReserveA * previousReserveB` using 128-bit arithmetic (`uint128_t`).

### `OrderBookIsNotCrossed` (strict, BUILD_TESTS only)

**Purpose:** Maintains an in-memory order book and checks that buy/sell prices never cross (lowest ask ≤ highest bid only allowed if all offers at that price are passive).

**Hook used:** `checkOnOperationApply`.

**Not registered via normal config.** Only registered and enabled explicitly via `registerAndEnableInvariant()` from fuzzer code or dedicated tests, because it maintains state across calls and cannot handle rollbacks without the `snapshotForFuzzer`/`resetForFuzzer` mechanism.

**Key data structures:**
- `OrderBook` = `unordered_map<AssetPair, set<OfferEntry, OfferEntryCmp>>` — sorted by price, then passive-flag, then offerID.
- `mOrderBookSnapshot` — saved state for fuzzer rollback.

**Key logic:** `updateOrderBook()` processes LedgerTxnDelta to add/remove offers. `check()` iterates affected asset pairs and calls `checkCrossed()` which compares the lowest ask price to the inverse of the lowest bid price. Equal prices are allowed only if at least one side is entirely passive offers.

### `BucketListStateConsistency` (strict)

**Purpose:** Background snapshot invariant that validates consistency between the BucketList, `InMemorySorobanState` cache, and HotArchive for Soroban entries. Only runs from SOROBAN_PROTOCOL_VERSION+.

**Hook used:** `checkSnapshot`.

**Properties checked:**
1. Every live CONTRACT_DATA/CONTRACT_CODE entry in the BucketList exists in `InMemorySorobanState` with matching value.
2. No extra entries exist in the cache (validated via count comparison).
3. Each live soroban entry has a corresponding TTL entry with matching value in the cache.
4. No orphan TTL entries exist without a corresponding soroban entry.
5. No live entry in the live BL is also present in the hot archive BL.
6. Only persistent CONTRACT_DATA and CONTRACT_CODE entries exist in the hot archive.
7. Cached total entry sizes match the sum of actual entry sizes.

**Implementation:** Scans CONTRACT_DATA, CONTRACT_CODE, and TTL entries sequentially via `scanForEntriesOfType()`, tracking seen keys to handle shadowing. Uses `shouldAbortInvariantScan()` between scans for early termination.

### `ArchivedStateConsistency` (non-strict)

**Purpose:** Validates that eviction and restoration of Soroban entries are consistent with the live and hot-archive BucketList state. Only runs from the first protocol supporting persistent eviction.

**Hook used:** `checkOnLedgerCommit`.

**Key logic:**
- Preloads all relevant keys from live and archived snapshots in batch.
- **Eviction checks:** Archived entries must not already exist in the hot archive, must exist in live state, must have an expired TTL, and (from V24+) must match the latest live version. Temporary entries must also be expired. Count of TTL keys evicted must equal count of data/code entries evicted.
- **Restore checks:** Restored entries must be persistent. For hot-archive restores: entry must not be in live state and must exist in archive with matching value (from V24+). For live-state restores: entry must exist in live state with matching value, must not be in hot archive, and TTL must be expired.

### `EventsAreConsistentWithEntryDiffs` (strict)

**Purpose:** Validates that Stellar Asset Contract (SAC) events (transfer, mint, burn, clawback, set_authorized) are consistent with the actual ledger entry balance changes for each operation.

**Hook used:** `checkOnOperationApply`.

**Key data structures:**
- `AggregatedEvents` — accumulates net balance changes per `(SCAddress, Asset)` from events, and tracks `set_authorized` state changes.
- `stellarAssetContractIDs` — maps contract hashes to `Asset` for SAC identification.

**Key logic:**
1. `aggregateEventDiffs()` processes all contract events: transfer subtracts from source and adds to destination; mint adds; burn/clawback subtracts. Uses 128-bit arithmetic via Rust bridge (`rust_bridge::i128_add/sub`). Returns `nullopt` on malformed events.
2. For each entry delta, `calculateDeltaBalance()` computes the actual balance change and `consumeAmount()` retrieves the corresponding event amount. Checks they match for accounts, trustlines, claimable balances, liquidity pools, and SAC contract data balance entries.
3. After all entries are checked, any remaining unconsumed event amounts must be zero.
4. Handles protocol 23 hot-archive bug reconciliation via `getProtocol23CorruptionEventReconciler()`.
5. `checkAuthorization()` validates that trustline authorization changes match `set_authorized` events.

---

## Control Flow and Threading

### Main Thread Dispatch

All `checkOnOperationApply`, `checkOnBucketApply`, `checkAfterAssumeState`, and `checkOnLedgerCommit` calls happen synchronously on the main thread as part of transaction/ledger processing. They iterate the `mEnabled` vector and short-circuit on the first failure, calling `onInvariantFailure()`.

### Background Snapshot Thread

Expensive invariants (`checkSnapshot`) run on a background thread managed by LedgerManager:
1. `InvariantManagerImpl::start()` schedules `mStateSnapshotTimer` (period = `STATE_SNAPSHOT_INVARIANT_LEDGER_FREQUENCY` seconds).
2. Timer fires → `snapshotTimerFired()` sets `mShouldRunStateSnapshotInvariant = true` (atomic).
3. On next ledger close, `LedgerManager` checks `shouldRunInvariantSnapshot()`, snapshots state, calls `markStartOfInvariantSnapshot()` (sets running flag, clears should-run flag), and dispatches `runStateSnapshotInvariant()` on a background thread.
4. Background thread iterates all enabled invariants calling `checkSnapshot()`. On completion, clears `mStateSnapshotInvariantRunning` via `gsl::finally`.
5. If a snapshot scan throws, `printErrorAndAbort` is called to match strict invariant failure behavior.

### Thread Safety

- `mFailureInformation` is protected by `mFailureInformationMutex` (accessed from both main thread and background snapshot thread via `onInvariantFailure()`).
- `mStateSnapshotInvariantRunning` and `mShouldRunStateSnapshotInvariant` are `atomic<bool>` for lock-free coordination between the timer callback, LedgerManager, and background thread.

---

## Ownership and Data Flow

- `Application` owns the `InvariantManager` (via `unique_ptr`).
- `InvariantManagerImpl` owns all registered invariants (`shared_ptr<Invariant>` in `mInvariants` map and duplicated in `mEnabled` vector).
- Most invariants are stateless (compute results from the `LedgerTxnDelta` or snapshots passed in). Exceptions:
  - `BucketListIsConsistentWithDatabase` holds an `Application&` reference for DB/BucketManager access.
  - `OrderBookIsNotCrossed` maintains a `mOrderBook` across calls.
  - `ConservationOfLumens` and `LedgerEntryIsValid` store `AssetContractInfo` (computed at registration time from network ID).
  - `EventsAreConsistentWithEntryDiffs` stores a `Hash const&` to the network ID.

### Registration Pattern

Each invariant provides a static `registerInvariant(Application&)` method that constructs the invariant with any needed dependencies and calls `app.getInvariantManager().registerInvariant<T>(args...)`. Registration happens at application startup. Enablement happens separately via config patterns (`INVARIANT_CHECKS` config entries) that are regex-matched against registered invariant names.

### Data Flow Summary

```
LedgerManager / BucketManager
    │
    ├── checkOnOperationApply(op, result, LedgerTxnDelta, events, app)
    │       │
    │       └── for each enabled Invariant → checkOnOperationApply()
    │               └── returns "" (ok) or error string → onInvariantFailure()
    │
    ├── checkOnBucketApply(bucket, ledger, level, isCurr, shadowedKeys)
    │       └── for each enabled Invariant → checkOnBucketApply()
    │
    ├── checkOnLedgerCommit(liveState, archiveState, evicted, restored)
    │       └── for each enabled Invariant → checkOnLedgerCommit()
    │
    └── runStateSnapshotInvariant(liveSnap, archiveSnap, inMemSnap, isStopping)
            └── [background thread] for each enabled Invariant → checkSnapshot()
```
